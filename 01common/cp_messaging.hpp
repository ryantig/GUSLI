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
#include "gusli_server_api.hpp"
#include "shared_mem_bufs.hpp"

/* This file defines the control path messaging (communication) between client and server
	Including handshake, notifications etc. */
namespace gusli {
class MGMT : no_constructors_at_all {		// CLient<-->Server control path API
	static constexpr int msg_type_len = 6;	// Header of all messages is 6 bytes long. See below
 public:
	static constexpr int COMM_PORT = 2051;				// Communication port, Relevant for TCP/UDP. Not for UDS
	static constexpr bool set_large_io_buffers = false;	// Sockets dont need large buffers as send messages are very small
	static sock_t::type get_com_type(const char* addr) {
		if (addr[0] == '/') return sock_t::type::S_UDS;	// Path: Fastest communication (unix domain socket)
		if (addr[0] == 'u') return sock_t::type::S_UDP; // UDP: Fast
		if (addr[0] == 't') return sock_t::type::S_TCP; // TCP: Slow, for debugging?
		return sock_t::type::S_UNK;
	}
	static constexpr const uint64_t shm_cookie = 0xa8a9aaabacadaeafLL;	// Cookie which initializes shared memory

	struct msg {
		struct type_t { const char *str; uint8_t idx; };
		// Front channel:  Srvr->Clnt
		static constexpr type_t hello          {"Chello", 0};	// Clnt->Srvr: Hello message (open bdev)
		static constexpr type_t hello_ack =    {"Shello", 1};	//   \--> Server accepts/reject client
		static constexpr type_t register_buf = {"Creg__", 2};	// Clnt->Srvr: Register memory buffers
		static constexpr type_t register_ack = {"SregOk", 3};	//   \--> Server accepts/reject client register
		static constexpr type_t unreg_buf =    {"CUnreg", 4};	// Clnt->Srvr: Un-Register previously mapped memory buffers
		static constexpr type_t unreg_ack =    {"SunrOk", 5};	//   \--> Server accepts/reject client un-register
		static constexpr type_t close_nice =   {"Cclose", 6};	// Clnt->Srvr: Close bdev (socket remains open for future messages)
		static constexpr type_t close_ack =    {"Sclose", 7};	//   \--> Server response on the above
		static constexpr type_t die_now =      {"C_DIE!", 8};	// Clnt->Srvr: Order to Kill yourself. In case of detected data corruption or debug
		static constexpr type_t die_ack =      {"S_RIP!", 9};	//   \--> Server Answer: Suicide ack. Answer sent last before server goes done.
		static constexpr type_t log =          {"C_LOG_",10};	// Clnt->Srvr: Add the following message to log. Server responds with keep alive
		static constexpr type_t keepalive =    {"SC_KAL",11};	//    \--> Server Answer: Keep alive with appropriate extra info

		static constexpr type_t srvr_reboot =  {"C_Rebt",12};	// Clnt->Srvr: Reboot your connection with me. Emulate server crash and reconnect back
		// Back channel:  Srvr->Clnt
		static constexpr type_t server_kick =  {"Ssorry",13};	// Srvr->Clnt: Server wants client to disconnect nicely
																//   \---> Client may optionally reply with 'close_nice'.
		// Error handling
		static constexpr type_t wrong_cmd =    {"SC_UNK",14};	// Both Answer: Unknown command received, should never happen

		// Datapath
		static constexpr type_t dp_submit =    {"C_DP>S",15};	// Clients doorbell to notify server that new io is waiting for execution or client can receive completions
		static constexpr type_t dp_complete =  {"S_DP>C",16};	// Server reply that some completions should be handled or server is ready to receive a new IO
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
				char name[8 + sizeof(backend_bdev_id)];			// +8 Because Starting with '/gs' + 4 bytes suffix + \0
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
				void fill(void) { extra_info[0] = 0; }
				void fill(const std::string& s) {
					const char *p = s.c_str(); while (char_is_space(*p)) p++;
					strncpy_no_trunc_warning(extra_info, p, 56);
				}
			} c_log, s_kal, s_kick, c_die, s_die, c_reb, wrong_cmd;
			struct t_dp_cmd  {							// Client is producer of submissions, Server is producer of completions
				uint64_t reserved;
				uint32_t sender_added_new_work;			// Boolean: Producer notifies consumer that it added new work for it to consume (Unblock consumer from blocked-read)
				uint32_t sender_ready_for_work;			// Boolean: Consumer notifies producer that it is ready to consume new work     (Unblock producer from blocked-write)
			} dp_submit, dp_complete;
		} pay; // __attribute__((packed));
		      char *raw(void)       { return (char*)this; }
		const char *raw(void) const { return (const char*)this; };
		bool is(msg::type_t mt) const { return hdr.is(mt); }
		size_t get_msg_size(void) const { return sizeof(t_header) + sizeof(pay); }		// All messages have constant small size
		size_t build_hello(  void) { hdr.init(MGMT::msg::hello);		return get_msg_size(); }
		size_t build_hel_ack(void) { hdr.init(MGMT::msg::hello_ack);	return get_msg_size(); }
		size_t build_reg_buf(void) { hdr.init(MGMT::msg::register_buf);	return get_msg_size(); }
		size_t build_reg_ack(void) { hdr.init(MGMT::msg::register_ack);	return get_msg_size(); }
		size_t build_unr_buf(void) { hdr.init(MGMT::msg::unreg_buf);	return get_msg_size(); }
		size_t build_unr_ack(void) { hdr.init(MGMT::msg::unreg_ack);	return get_msg_size(); }
		size_t build_close(void) {   hdr.init(MGMT::msg::close_nice);	pay.c_close.reserved = 0UL; return get_msg_size(); }
		size_t build_cl_ack(void) {  hdr.init(MGMT::msg::close_ack);	return get_msg_size(); }
		size_t build_log(   const std::string& s) { hdr.init(MGMT::msg::log);         pay.c_log.fill(s); return get_msg_size(); }
		size_t build_reboot(const std::string& s) { hdr.init(MGMT::msg::srvr_reboot); pay.c_log.fill(s); return get_msg_size(); }
		size_t build_die(    void) { hdr.init(MGMT::msg::die_now);		pay.c_die.fill(); return get_msg_size(); }
		size_t build_die_ack(void) { hdr.init(MGMT::msg::die_ack);		pay.s_die.fill(); return get_msg_size(); }
		size_t build_ping(void) {    hdr.init(MGMT::msg::keepalive);	return get_msg_size(); }
		size_t build_skick(void) {   hdr.init(MGMT::msg::server_kick);	return get_msg_size(); }
		size_t build_wrong(void) {   hdr.init(MGMT::msg::wrong_cmd);	pay.wrong_cmd.fill(); return get_msg_size(); }
		size_t build_dp_subm(void) { hdr.init(MGMT::msg::dp_submit);	pay.dp_submit.reserved = 0UL;   return get_msg_size(); }
		size_t build_dp_comp(void) { hdr.init(MGMT::msg::dp_complete);	pay.dp_complete.reserved = 0UL; return get_msg_size(); }

		bool is_full(int n_bytes) const { return (n_bytes >= (int)sizeof(t_header)) && (n_bytes == (int)get_msg_size()); }
	} __attribute__((aligned(sizeof(long))));
};

inline enum io_state __read_1_full_message(sock_t& sock, MGMT::msg_content& msg, bool with_epoll, connect_addr &addr) { // Todo: Use readn()
	socklen_t sinlen = addr.get_len(sock.is_remote());
	int n_bytes; nvTODO("Refactor this function, consider using readn() from utils.hpp");
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
	int n_bytes; nvTODO("Refactor this function, consider using readn() from utils.hpp");
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

} // namespace
