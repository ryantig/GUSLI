/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <time.h>
#include "shared_mem_bufs.hpp"
#include "dp_io_air_io.hpp"
#include "gusli_server_api.hpp"
namespace gusli {

/*********************** Client Server Ring ***********************/
/* Based on ideas from (no code stealing)
	https://github.com/deepseek-ai/3FS/blob/main/src/fuse/IoRing.h
	https://elixir.bootlin.com/linux/v6.14.5/source/tools/include/io_uring/mini_liburing.h
	https://github.com/anlongfei/libaio/blob/master/src/libaio.h
	https://github.com/Mulling/io-uring-ipc
	#include <aio.h>
*/
struct io_csring_sqe {					// IO submition queue entry
	using context_t = io_request::params_t;
	context_t user_data;
	uint32_t is_used : 8;
	uint32_t flags  : 24;
	void init_empty(void) { memset(this, 0, sizeof(*this)); }
	void destroy(void) {}
	void init_from(const context_t& p) {
		user_data = p;
		user_data.set_completion((void*)&p, NULL);	// Connect submission entry to clients IO pointer, callback function irrelevant
	}
	void extract_to(context_t* p) { *p = user_data; }
} __attribute__((aligned(sizeof(long))));

struct io_csring_cqe {
	using context_t = struct {
		server_io_req *io_ptr;			// Connect completion entry to clients IO pointer
		int64_t  rv;					// result code for the IO as returned from the server
	};
	context_t ctx;
	uint32_t is_used : 8;
	uint32_t flags  : 24;
	void init_empty(void) { memset(this, 0, sizeof(*this)); }
	void destroy(void) {}
	void init_from(const context_t& p) { ctx = p; }
	void extract_to(context_t* p) { *p = ctx; }
} __attribute__((aligned(sizeof(long))));

template <class T, unsigned int CAPACITY, const char dbg_name>
class io_csring_queue : no_constructors_at_all {	// Circular buffer, can hold up to CAPACITY-1 elements
	T arr[CAPACITY];
	t_lock_spinlock lock_;				// Producer/Consumer mutual exclusion
	uint32_t head;						// Next free entry to use. Increased by producer when inserting new   element
	uint32_t tail;						// Last handled entry.     Increased by consumer when removing oldest element
	uint32_t n_elem_in_queue;			// == (head - tail - 1) + ((head > tail) ? 0 : CAPACITY)		// [0..CAPACITY-1]
	uint32_t head_inc(void) { n_elem_in_queue++; return head = (head+1) % CAPACITY; }
	uint32_t tail_inc(void) { n_elem_in_queue--; return tail = (tail+1) % CAPACITY; }
 public:
	uint32_t in_queue(void) const { return n_elem_in_queue; }
	bool     is_full(void)  const { return head == tail; }	// Identical to (n_elem_in_queue == (CAPACITY-1))
	bool     is_empty(void) const { return n_elem_in_queue == 0; }
	void init(void) {
		BUILD_BUG_ON((CAPACITY <= 1));
		for (unsigned int i = 0; i < CAPACITY; i++)
			arr[i].init_empty();
		head = 0;
		tail = CAPACITY-1;
		lock_.init();
	}
	void destroy(void) {
		BUG_ON(lock_.is_locked(), "Circular buffer is still locked during destruction!");
		for (unsigned int i = 0; i < CAPACITY; i++)
			arr[i].destroy();
		lock_.destroy();
	}
	int insert(const T::context_t& p, bool *was_list_empty) {	// -1 on error, index of element [0..CAPACITY-1] on success
		t_lock_guard l(lock_);
		*was_list_empty = is_empty();
		// pr_verbs("%c: nelems=%u ++\n", dbg_name, in_queue());
		if (is_full())
			return -1;
		T *i = &arr[head];
		BUG_ON(i->is_used, "Element %u already used!", uint32_t(i-arr));
		i->init_from(p);
		i->is_used = true;
		head_inc();
		return (int)(i-arr);
	}
	int remove(T::context_t* p, bool *was_list_full) {			// -1 on error, index of element on success
		t_lock_guard l(lock_);
		*was_list_full = is_full();
		// pr_verbs("%c: nelems=%u --\n", dbg_name, in_queue());
		if (is_empty())
			return -1;
		T *i = &arr[tail_inc()];
		BUG_ON(!i->is_used, "Element %u is unused!", uint32_t(i-arr));
		i->extract_to(p);
		i->is_used = false;
		return (int)(i-arr);
	}
};

struct io_csring : no_constructors_at_all {	// Datapath mechanism to remote bdev, shared memory is initialized
	static constexpr int CAPACITY = 512;	// Maximal ammount of in air IO's + at least 1
	io_csring_queue<io_csring_sqe, CAPACITY, 's'> sq;
	io_csring_queue<io_csring_cqe, CAPACITY, 'c'> cq;
	void init(void) {						// Initialized by client (producer)
		sq.init();
		cq.init();
	}
	// datapath
	static bool is_big_enough_for(int num_max_inflight_io) { return CAPACITY >= (num_max_inflight_io+1); }	// +1 for debug, so according to max io's ring will never be full so producer will never have to block
	static size_t n_needed_bytes(size_t block_size) { return align_up(sizeof(io_csring), block_size); }
};

/***************************** Generic datapath ******************************/
template <class T_stats> class datapath_t {												// Datapath of block device
	bool verify_map_valid(     const io_map_t    &map) const {
		return (map.is_valid_for(binfo.block_size) &&
				(map.get_offset_end_lba() < binfo.get_bdev_size()) &&
				shm_io_bufs->does_include(map.data)); }
	io_csring *get(void) const { return (io_csring *)shm_ring.get_buf(); }
 public:
	t_shared_mem shm_ring;							// Mapped Submition/completion queues.
	shm_io_bufs_global_t *shm_io_bufs;				// Pointer to global structure
	shm_io_bufs_unuque_set_for_bdev reg_bufs_set;
	in_air_ios_holder in_air;
	T_stats stats;
	const bdev_info &binfo;
	datapath_t(const char* producer_name, shm_io_bufs_global_t *_ext_g, const bdev_info &bi) : in_air(bi.num_max_inflight_io), binfo(bi) {
		shm_io_bufs = _ext_g;
		if (producer_name) {    // Producer
			const size_t n_bytes = io_csring::n_needed_bytes(bi.block_size);
			ASSERT_IN_PRODUCTION(shm_ring.init_producer(producer_name, n_bytes) == 0);
			get()->init();
		}
	}

	uint32_t get_num_mem_reg_ranges(void) { return (uint32_t)reg_bufs_set.size(); }
	uint32_t get_num_in_air_ios(void)     { return (uint32_t)in_air.size(); }
	bool     is_still_used(void) { return (get_num_mem_reg_ranges() + get_num_in_air_ios()) > 0; }

	bool verify_io_param_valid(const server_io_req &io) const;
	int  clnt_send_io( io_request_base &io, bool *need_wakeup_srvr_consumer) const;
	int  clnt_receive_completion(           bool *need_wakeup_srvr_producer) const;
	int  srvr_receive_io(         server_io_req &io, bool *need_wakeup_clnt_producer) const;
	bool srvr_remap_io_bufs_to_my(server_io_req &io) const;	// IO bufs pointers are given in clients addresses, need to convert them to server addresses
	int  srvr_finish_io(          server_io_req &io, bool *need_wakeup_clnt_consumer) const;

	std::vector<io_buffer_t> registerd_bufs_get_list(void) const {
		return shm_io_bufs->get_all_bufs(reg_bufs_set);
	}

	void registerd_bufs_force_clean(void) {
		t_lock_guard l(shm_io_bufs->with_lock());
		for (auto it = reg_bufs_set.begin(); it != reg_bufs_set.end(); ++it) {
			base_shm_element *g_map = shm_io_bufs->find2(*it);
			BUG_ON(!g_map, PRINT_MMAP_PREFIX " GC: unknown buf [%d%c] globally", *it, 'i');
			shm_io_bufs->dec_ref(g_map);
		}
		reg_bufs_set.clear();
	}
};

template <class T>
inline bool datapath_t<T>::srvr_remap_io_bufs_to_my(server_io_req &io) const {
	io_map_t &map = io.params.change_map();
	if (!shm_io_bufs->remap_to_local(map.data))			// Remap the 1 range io buffer or scatter gather buffer
		return false;
	if (io.params.is_multi_range()) {
		const size_t sgl_len = map.data.byte_len;
		const io_multi_map_t* mm = io.get_multi_map();
		      io_multi_map_t* nm = nullptr;
		// Must Replace the sgl because we cant change it in shared memory. Why?
		//	1. Client owns sgl and may traverse it after io finishes
		//	2. Client may retry the io so sgl must remain as in first try
		if (posix_memalign((void **)&nm, 4096,sgl_len) != 0) {
			pr_err1("Realloc_sgl oom error len=0x%lx[b]\n", sgl_len);
			return false;
		}
		pr_verb1("Realloc_sgl len=0x%lx[b], n_entries=%u { %p -> %p }\n", sgl_len, mm->n_entries, mm, nm);
		memcpy(nm, mm, sgl_len);
		for (uint32_t i = 0; i < nm->n_entries; i++) {
			if (!shm_io_bufs->remap_to_local(nm->entries[i].data)) {
				free(nm);
				return false;
			}
		}
		map.data.ptr = (void*)nm;
	}
	return true;
}

template <class T>
inline bool datapath_t<T>::verify_io_param_valid(const server_io_req &io) const {
	if (!io.params.is_multi_range())
		return verify_map_valid(io.params.map());			// 1-range mapping is valid
	const io_multi_map_t* mm = io.get_multi_map();
	if (!mm->is_valid())									// Scatter gather is valid
		return false;
	for (uint32_t i = 0; i < mm->n_entries; i++) {
		if (!verify_map_valid(mm->entries[i]))				// Each entry is valid
			return false;
	}
	return shm_io_bufs->does_include(io.params.map().data);	// Scatter-gather is accesible to server
}

template <class T>
inline int datapath_t<T>::clnt_send_io(io_request_base &io, bool *need_wakeup_srvr) const {
	server_io_req *sio = (server_io_req*)&io;
	int rv = 0;
	if (unlikely(io.params.is_polling_mode())) {				// Polling mode not supported yet, todo, add support
		sio->set_error(io_error_codes::E_INVAL_PARAMS);
		return -1;
	}
	if (!io.params.is_safe_io() && !verify_io_param_valid(*sio)) {
		sio->set_error(io_error_codes::E_INVAL_PARAMS);
		return -1;
	}
	io_csring *r = get();
	rv = r->sq.insert(io.params, need_wakeup_srvr);
	if (rv < 0) {
		sio->set_error(io_error_codes::E_THROTTLE_RETRY_LATER);
		return -1;
	} // Note: here io can already be free() because completion arrived
	return rv;
}

template <class T>
inline int datapath_t<T>::clnt_receive_completion(bool *need_wakeup_srvr) const {
	io_csring *r = get();
	io_csring_cqe::context_t comp;
	const int cqe = r->cq.remove(&comp, need_wakeup_srvr);
	if (cqe < 0)
		return cqe;		// No completions arrived, client poller can go to sleep
	server_io_req* io = comp.io_ptr;
	BUG_ON(!io, "Server did not return back the client context, Client cant find the completed io");
	BUG_ON(!io->params.has_callback(), "How else would we notify the sender that IO finished?");
	pr_verb1(PRINT_IO_REQ_FMT PRINT_IO_CQE_ELEM_FMT ".rv[%ld]\n", PRINT_IO_REQ_ARGS(io->params), cqe, comp.rv);
	io->set_success(comp.rv);
	return cqe;
}

template <class T>
inline int datapath_t<T>::srvr_receive_io(server_io_req &io, bool *need_wakeup_clnt) const {
	io_csring *r = get();
	return r->sq.remove(&io.params, need_wakeup_clnt);
}

template <class T>
inline int datapath_t<T>::srvr_finish_io(server_io_req &io, bool *need_wakeup_clnt) const {
	io_csring *r = get();
	io_csring_cqe::context_t comp{(server_io_req*)io.get_comp_ctx(), io.get_raw_rv()};
	int rv = r->cq.insert(comp, need_wakeup_clnt);
	ASSERT_IN_PRODUCTION(rv >= 0);		// Todo: Server has to block to let client process the completions
	return rv;
}

/******************************** Control Path ***********************/
class MGMT : no_constructors_at_all {		// CLient<-->Server control path API
	static constexpr int msg_type_len = 6;	// Header of all messages is 6 bytes long. See below
 public:
	static constexpr int COMM_PORT = 2051;	// Communication port
	static constexpr bool set_large_io_buffers = false;
	static sock_t::type get_com_type(const char* addr) {
		if (addr[0] == '/') return sock_t::type::S_UDS;	// Path: Fastest communication
		if (addr[0] == 'u') return sock_t::type::S_UDP; // UDP: Fast
		if (addr[0] == 't') return sock_t::type::S_TCP; // TCP: Slow, for debugging?
		return sock_t::type::S_UNK;
	}
	static constexpr const uint64_t shm_cookie = 0xa8a9aaabacadaeafLL;	// Cookie which initializes shared memory

	struct msg {
		struct type_t { const char *str; uint8_t idx; };
		// Front channel:  Srvr->Clnt
		static constexpr type_t hello          {"Chello", 0};	// Clnt->Srvr: Hello message
		static constexpr type_t hello_ack =    {"Shello", 1};	//   \--> Server accepts/reject client
		static constexpr type_t register_buf = {"Creg__", 2};	// Clnt->Srvr: Register memory buffers
		static constexpr type_t register_ack = {"SregOk", 3};	//   \--> Server accepts/reject client register
		static constexpr type_t unreg_buf =    {"CUnreg", 4};	// Clnt->Srvr: Un-Register preveiously mapped memory buffers
		static constexpr type_t unreg_ack =    {"SunrOk", 5};	//   \--> Server accepts/reject client un-register
		static constexpr type_t close_nice =   {"Cclose", 6};	// Clnt->Srvr: Close connection
		static constexpr type_t close_ack =    {"Sclose", 7};	//   \--> Server response on the above
		static constexpr type_t die_now =      {"C_DIE!", 8};	// Clnt->Srvr: Order to Kill yourself. In case of detected data corruption or debug
		static constexpr type_t die_ack =      {"S_RIP!", 9};	//   \--> Server Answer: Suicide ack. Answer sent last before server goes done.
		static constexpr type_t log =          {"C_LOG_",10};	// Clnt->Srvr: Add the following message to log. Server responds with keep alive
		static constexpr type_t keepalive =    {"SC_KAL",11};	//    \--> Server Answer: Keep alive with appropriate extra info

		// Back channel:  Srvr->Clnt
		static constexpr type_t server_kick =  {"Ssorry",12};	// Srvr->Clnt: Force kick client
																//   \---> Client may optionally reply with 'close_nice'.
		// Error handling
		static constexpr type_t wrong_cmd =    {"SC_UNK",13};	// Both Answer: Unknown command received

		// Datapath
		static constexpr type_t dp_submit =    {"C_DP>S",14};	// Clients doorbell to notify server that new io is waiting for execution or client can receive completions
		static constexpr type_t dp_complete =  {"S_DP>C",15};	// Server reply that some completions should be handled or server is ready to receive a new IO
	};
	struct msg_content {
		struct t_header {
			char type[msg_type_len];
			char idx;
			char version = '0';			// Version of message/protocol
			void init(const msg::type_t mt) { memcpy(type, mt.str, msg_type_len); idx = (0x40 ^ mt.idx); version = '0'; }
			bool is(  const msg::type_t mt) const { return (idx == (0x40 ^ mt.idx)); }
		} hdr;
		union t_payload {
			struct t_hello {
				char security_cookie[8];
				char client_id[8];
				backend_bdev_id volume;			// Requested volume to connect to
				int64_t reserved;
				void fill(backend_bdev_id _v, const char* _clnt, const char* sec) {
					memcpy(security_cookie, sec, sizeof(security_cookie));
					const size_t nc = min(strlen(_clnt) + 1, sizeof(client_id));
					memcpy(client_id, _clnt, nc);
					volume = _v;
					reserved = 0UL;
				}
			} c_hello;
			struct t_hello_ack {
				bdev_info info;					// .bdev_descriptor < 0 means error
			} s_hello_ack;
			struct t_register_buf {
				char name[8 + sizeof(backend_bdev_id)];			// +8 Because Starting with '/gs' + 4ybtes suffix + \0
				uint64_t client_pointer;
				uint32_t num_blocks;							// Blocks as defined by the server
				uint32_t buf_idx   : 16;
				uint32_t is_io_buf : 16;
				void build_scheduler(const backend_bdev_id &volume, uint32_t _num_blocks) {
					snprintf(name, sizeof(name), "/gs%.16sring", volume.uuid);
					client_pointer = 0xdeadbeef99UL;			// Irrelevant
					num_blocks = _num_blocks;
					buf_idx = 0xffff;							// Irrelevant
					is_io_buf = false;
				}
				void build_io_buffer(const base_shm_element &m, uint32_t block_size) {
					snprintf(name, sizeof(name), "%s", m.mem.get_producer_name());
					client_pointer = (uint64_t)m.mem.get_buf();
					num_blocks = (m.mem.get_n_bytes() / (size_t)block_size);
					buf_idx = m.buf_idx;
					is_io_buf = true;
				}
				int  get_buf_idx( void) const { return is_io_buf ? (int)buf_idx : -1; }
				char get_buf_type(void) const { return is_io_buf ? 'i' : 'r'; }
			 } c_register_buf, c_unreg_buf;
			struct t_register_ack {
				char name[8 + sizeof(backend_bdev_id)];
				uint64_t server_pointer;
				int32_t rv;
				uint32_t buf_idx   : 16;
				uint32_t is_io_buf : 16;
				void init_with(void* srvr_ptr, int _rv) { server_pointer = (uint64_t)srvr_ptr; rv = _rv; }		// Leave other fields (buf_idx, is_io_buf) untouched
				int  get_buf_idx( void) const { return is_io_buf ? (int)buf_idx : -1; }
				char get_buf_type(void) const { return is_io_buf ? 'i' : 'r'; }
			} s_register_ack, s_unreg_ack;
			struct t_close_nice {
				backend_bdev_id volume;
				int64_t reserved;
			} c_close;
			struct t_close_ack  {
				backend_bdev_id volume;
				int32_t rv;
				int32_t reserved;
			} s_close;
			struct t_keep_alive  {
				char extra_info[56];					// Debug info
			} c_log, s_kal, s_kick, c_die, s_die;
			struct t_wrong_cmd  {
				char extra_info[56];					// Information about wrong command
			} wrong_cmd;
			struct t_dp_cmd  {							// Client is producer of submitions, Server is producer of completions
				uint64_t reserved;
				uint32_t sender_added_new_work;			// Boolean: Producer notifies consumer that it added new work for it to consume (Unblock consumer from blocked-read)
				uint32_t sender_ready_for_work;			// Boolean: Consumer notifies producer that it is ready to consume new work     (Unblock producer from blocked-write)
			} dp_submit, dp_complete;
		} pay; // __attribute__((packed));
		      char *raw(void)       { return (char*)this; }
		const char *raw(void) const { return (const char*)this; };
		bool is(msg::type_t mt) const { return hdr.is(mt); }
		size_t get_msg_size(void) const { return sizeof(t_header) + sizeof(pay); }		// All messages have constant small size
		#define BUIL_MSG_RET const size_t rv = get_msg_size(); return (rv);
		size_t build_hello(  void) { hdr.init(MGMT::msg::hello);		BUIL_MSG_RET }
		size_t build_hel_ack(void) { hdr.init(MGMT::msg::hello_ack);	BUIL_MSG_RET }
		size_t build_reg_buf(void) { hdr.init(MGMT::msg::register_buf);	BUIL_MSG_RET }
		size_t build_reg_ack(void) { hdr.init(MGMT::msg::register_ack);	BUIL_MSG_RET }
		size_t build_unr_buf(void) { hdr.init(MGMT::msg::unreg_buf);	BUIL_MSG_RET }
		size_t build_unr_ack(void) { hdr.init(MGMT::msg::unreg_ack);	BUIL_MSG_RET }
		size_t build_close(void) {   hdr.init(MGMT::msg::close_nice);	pay.c_close.reserved = 0UL; BUIL_MSG_RET }
		size_t build_cl_ack(void) {  hdr.init(MGMT::msg::close_ack);	BUIL_MSG_RET }
		size_t build_die(    void) { hdr.init(MGMT::msg::die_now);		pay.c_die.extra_info[0] = 0; BUIL_MSG_RET }
		size_t build_die_ack(void) { hdr.init(MGMT::msg::die_ack);		pay.s_die.extra_info[0] = 0; BUIL_MSG_RET }
		size_t build_ping(void) {    hdr.init(MGMT::msg::keepalive);	BUIL_MSG_RET }
		size_t build_skick(void) {   hdr.init(MGMT::msg::server_kick);	BUIL_MSG_RET }
		size_t build_wrong(void) {   hdr.init(MGMT::msg::wrong_cmd);	BUIL_MSG_RET }
		size_t build_dp_subm(void) { hdr.init(MGMT::msg::dp_submit);	pay.dp_submit.reserved = 0UL;   BUIL_MSG_RET }
		size_t build_dp_comp(void) { hdr.init(MGMT::msg::dp_complete);	pay.dp_complete.reserved = 0UL; BUIL_MSG_RET }

		bool is_full(int n_bytes) const { return (n_bytes >= (int)sizeof(t_header)) && (n_bytes == (int)get_msg_size()); }
	} __attribute__((aligned(sizeof(long))));
};

inline enum io_state __read_1_full_message(sock_t& sock, MGMT::msg_content& msg, bool with_epoll, connect_addr &addr) { // Todo: Use readn()
	socklen_t sinlen = addr.get_len(sock.is_remote());
	int n_bytes;
	while (true) {
		if (with_epoll)
			sock.epoll_reply_wait("\t->clnt:zzzz_read");
		if (sock.uses_connection()) {		// If multi-srvr, find server by fd
			n_bytes = read(    sock.fd(), msg.raw(), sizeof(msg));
		} else {							// If multi-srvr, find server by addr
			n_bytes = recvfrom(sock.fd(), msg.raw(), sizeof(msg), 0 /* | MSG_DONTWAIT*/, &addr.u.b, &sinlen);
		}
		if (n_bytes == 0) {	// Socket closed
			return ios_close;
		} else if (n_bytes < 0) {
			if (errno == EINTR) {
				continue;	// Retry
			} else if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) { // Only if socket is non blocking
				return ios_block;
			} else {
				return ios_error;
			}
		} else {
		}
		BUG_ON(!msg.is_full(n_bytes), "Message fragmentation not supported yet, msg=|%s|,size=%lu, n_bytes=%u\n", msg.raw(), msg.get_msg_size()+8, n_bytes);
		return ios_ok;
	}
	return ios_error;
}

inline enum io_state __send_1_full_message(const sock_t& sock, MGMT::msg_content& msg, bool with_epoll, int n_todo_bytes, const connect_addr &addr, const char* who) { // Todo: Use writen()
	int n_bytes;
	while (true) {
		n_bytes = sock.send_msg(msg.raw(), n_todo_bytes, addr);
		if (n_bytes == 0) {	// Socket closed
			pr_err1("%s: Send Error %d, " PRINT_EXTERN_ERR_FMT "\n", who, n_bytes, PRINT_EXTERN_ERR_ARGS);
			return ios_close;
		} else if (n_bytes < 0) {
			if (errno == EINTR) {
				if (with_epoll) sock.epoll_reply_wait("\t->clnt:zzzz_send");
				continue;	// Retry
			} else if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				if (with_epoll) sock.epoll_reply_wait("\t->clnt:zzzz_send");
				continue;	// Retry //return ios_block;
			} else {
				pr_err1("%s: Send Error %d, " PRINT_EXTERN_ERR_FMT "\n", who, n_bytes, PRINT_EXTERN_ERR_ARGS);
				return ios_error;
			}
		} else {
		}
		BUG_ON(n_bytes != n_todo_bytes, "Message fragmentation not supported yet, msg=|%s|,size=%u, n_bytes=%u\n", msg.raw(), n_todo_bytes, n_bytes);
		return ios_ok;
	}
	return ios_error;
}

inline void __compilation_verification(void) {
	BUILD_BUG_ON(sizeof(backend_bdev_id) != 16);
	BUILD_BUG_ON(sizeof(MGMT::msg_content::t_header) != 8);
	BUILD_BUG_ON(sizeof(MGMT::msg_content::t_payload::c_register_buf) != 40);
	BUILD_BUG_ON(sizeof(MGMT::msg_content::t_payload::s_register_ack) != 40);
	BUILD_BUG_ON(sizeof(MGMT::msg_content) != 64);
	BUILD_BUG_ON(sizeof(io_request::params_t) != 48);
	BUILD_BUG_ON(sizeof(io_request) != 64);
	BUILD_BUG_ON(offset_of(io_request, params) != 0);
	BUILD_BUG_ON(sizeof(backend_io_req) != sizeof(io_request));		// Same class, just add functions for the executor of the io
	BUILD_BUG_ON(sizeof(server_io_req) != sizeof(io_request));		// Same class, just add functions for the executor of the io
	BUILD_BUG_ON(sizeof(io_multi_map_t) != 8);
	BUILD_BUG_ON(sizeof(bdev_info) != 56);
}

} // namespace gusli
