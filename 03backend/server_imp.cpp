#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "server_imp.hpp"
//#include "io_executors.hpp"

namespace gusli {

/******************************** Communicate with client ********************/
int global_srvr_context_imp::__clnt_bufs_register(const MGMT::msg_content &msg) {
	const auto *pr = &msg.pay.c_register_buf;
	const uint64_t n_bytes = (uint64_t)pr->num_blocks * binfo.block_size;
	t_shared_mem *shm_ptr;
	if (pr->is_io_buf) {
		base_shm_element *new_map = &dp.shm_io_bufs.emplace_back();
		new_map->buf_idx = pr->buf_idx;
		shm_ptr = &new_map->mem;
	} else {
		shm_ptr = &dp.shm;
	}
	int rv = shm_ptr->init_consumer(pr->name, n_bytes, (pr->is_io_buf ? (void*)pr->client_pointer : NULL));
	if (rv == 0) {	// Verify cookie
		rv = (*(u_int64_t*)shm_ptr->get_buf() == MGMT::shm_cookie) ? 0 : -EIO;
	}
	const io_buffer_t buf = {.ptr = shm_ptr->get_buf(), .byte_len = n_bytes};
	const int buf_idx = (pr->is_io_buf ? (int)pr->buf_idx : -1);
	pr_infoS(this, "Register[%d%c].vec[%d] " PRINT_IO_BUF_FMT ", n_blocks=0x%lx, clnt_ptr=0x%lx, rv=%d, name=%s\n", buf_idx, (pr->is_io_buf ? 'i' : 'r'), (int)dp.shm_io_bufs.size()-1, PRINT_IO_BUF_ARGS(buf), (n_bytes / binfo.block_size), pr->client_pointer, rv, pr->name);
	BUG_ON(rv, "Todo: instead of relying that clnt/server addresses are identical, IO should talk in buffer indices");
	return rv;
}

int global_srvr_context_imp::__clnt_bufs_unregist(const MGMT::msg_content &msg) {
	const auto *pr = &msg.pay.c_unreg_buf;
	const int vec_idx = dp.shared_buf_find(pr->buf_idx);
	if (vec_idx < 0) return -1;
	const uint64_t n_bytes = (uint64_t)pr->num_blocks * binfo.block_size;
	const io_buffer_t buf = {.ptr = NULL, .byte_len = n_bytes};
	ASSERT_IN_PRODUCTION(pr->is_io_buf == true);
	const int rv = 0;
	const int buf_idx = (pr->is_io_buf ? (int)pr->buf_idx : -1);
	dp.shm_io_bufs.erase(dp.shm_io_bufs.begin() + vec_idx);
	pr_infoS(this, "UnRegist[%d%c].vec[%d] " PRINT_IO_BUF_FMT ", n_blocks=0x%lx, clnt_ptr=0x%lx, rv=%d, name=%s\n", buf_idx, (pr->is_io_buf ? 'i' : 'r'), vec_idx, PRINT_IO_BUF_ARGS(buf), (n_bytes / binfo.block_size), pr->client_pointer, rv, pr->name);
	return rv;
}

void global_srvr_context_imp::send_to(const MGMT::msg_content &msg, size_t n_bytes, const struct connect_addr &addr) {
	ssize_t send_rv;
	if (msg.is(MGMT::msg::dp_complete))
		pr_verbS(this, " >> type=%c, fd=%d, msg=%s\n", io_sock.get_type(), io_sock.fd(), msg.raw());
	else
		pr_infoS(this, " >> type=%c, fd=%d, msg=%s\n", io_sock.get_type(), io_sock.fd(), msg.raw());
	send_rv = io_sock.send_msg(msg.raw(), n_bytes, addr);
	if (send_rv != (ssize_t)n_bytes) {
		pr_emerg("%s: Send Error rv=%ld, n_bytes=%lu, msg=%s\n", LIB_NAME, send_rv, n_bytes, msg.raw());
		client_reject();
		do_shut_down(-__LINE__);
	}
	//return (send_rv == (ssize_t)n_bytes) ? 0 : -1;
}

void global_srvr_context_imp::__clnt_close(const char* reason) {
	par.vfuncs.close1(par.vfuncs.caller_context, reason);
	char str[256];
	stats.print_stats(str, sizeof(str));
	pr_infoS(this, "stats{%s}\n", str);
	stats.clear();
	dp.destroy();
}

void global_srvr_context_imp::client_accept(connect_addr& addr) {
	if (sock.uses_connection()) {
		const int client_fd = sock.srvr_accept_clnt(addr);
		if (client_fd < 0) {
			pr_infoS(this, "accept failed: c_fd=%d" PRINT_EXTERN_ERR_FMT "\n", client_fd, PRINT_EXTERN_ERR_ARGS);
			return;
		}
		char clnt_path[32];
		sock.print_address(clnt_path, addr);
		pr_infoS(this, "accept a_fd=%d, c_fd=%d, path=%s\n", sock.fd(), client_fd, clnt_path);
		io_sock = sock_t(client_fd, sock.get_type());
		io_sock.set_blocking(true);			// Todo: Dont block on incomming io because same thread sends done io completions
	} else {
		io_sock = sock;
	}
	if (MGMT::set_large_io_buffers)
		io_sock.set_io_buffer_size(1<<19, 1<<19);
}

void global_srvr_context_imp::client_reject(void) {
	if (sock.uses_connection() && sock.is_alive()) {
		io_sock.nice_close();
		ca.clean();
	}
}

class backend_io_executor {
	server_io_req io;					// IO to pass to backend
	global_srvr_context_imp *srv = NULL;
	void* client_io_ctx = NULL;			// Original pointer to client io to pass back via completion entry
	connect_addr addr;					// Address on which to wakeup client if needed
	int sqe_indx;
	bool need_wakeup_clnt_io_submitter = false;
	bool need_wakeup_clnt_comp_reader = false;

	void _io_done_cb(void) {
		pr_verbS(srv, "exec[%p].Server io__cb_: rv=%ld\n", this, io.get_raw_rv());
		BUG_ON(client_io_ctx == NULL, "client will not be able to acciciate completion of this io");
		io.params._comp_ctx = client_io_ctx;			// Restore client context
		const int cmp_idx = srv->dp.srvr_finish_io(io, &need_wakeup_clnt_comp_reader);
		pr_verbS(srv, PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT PRINT_IO_CQE_ELEM_FMT PRINT_CLNT_IO_PTR_FMT ", doorbell={s=%u,c=%u}\n", PRINT_IO_REQ_ARGS(io.params), sqe_indx, cmp_idx, client_io_ctx, need_wakeup_clnt_io_submitter, need_wakeup_clnt_comp_reader);
		if (need_wakeup_clnt_io_submitter || need_wakeup_clnt_comp_reader) {
			MGMT::msg_content msg;	// Send wakeup completion to client on all executed IO's
			const size_t n_send_bytes = msg.build_dp_comp();
			auto *p = &msg.pay.dp_complete;
			p->sender_added_new_work = need_wakeup_clnt_comp_reader;
			p->sender_ready_for_work = need_wakeup_clnt_io_submitter;
			srv->stats.inc(*p);
			srv->send_to(msg, n_send_bytes, addr);
		}
		delete this;
	}
	static void static_io_done_cb(backend_io_executor *me) { me->_io_done_cb();	}
 public:
	backend_io_executor(const connect_addr& _addr, global_srvr_context_imp& _srv) {
		addr = _addr;
		srv = &_srv;
		sqe_indx = srv->dp.srvr_receive_io(io, &need_wakeup_clnt_io_submitter);
		if (unlikely(need_wakeup_clnt_io_submitter))
			pr_errS(srv, PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT "Client sent more than allowed > %u[ios], if it blocked will try waking him up\n", PRINT_IO_REQ_ARGS(io.params), sqe_indx, srv->binfo.num_max_inflight_io);
	}
	bool has_io_to_do(void) const { return (sqe_indx >= 0); }
	bool run(void) {
		if (has_io_to_do()) {
			srv->stats.inc(io);
			client_io_ctx = io.params._comp_ctx;
			pr_verbS(srv, "exec[%p].Server io_start " PRINT_IO_SQE_ELEM_FMT "\n", this, sqe_indx);
			DEBUG_ASSERT(io.is_valid());										// Verify no other executor connected to io
			io.params.set_completion(this, backend_io_executor::static_io_done_cb);
			srv->par.vfuncs.exec_io(srv->par.vfuncs.caller_context, io);		// Launch io execution
			return true;
		} else {
			delete this;
			return false;
		}
	}
};

void global_srvr_context_imp::__clnt_on_io_receive(const MGMT::msg_content &msg, const connect_addr& addr) {
	(void)msg;
	bool should_continue = true;
	int n_ios = 0;
	if (0) {	// Slow method, produces too much client wakeup messages, may process infinite amount of io's in a single loop
		do {
			auto *io = new backend_io_executor(addr, *this);
			should_continue = io->run();
		} while (should_continue);
	} else if (0) {	// Bound amount of ios but if < ring capacity will stuck. For debug only
		static constexpr const int n_max_batch_size = io_csring::CAPACITY;
		do {
			auto *io = new backend_io_executor(addr, *this);
			should_continue = io->run();
			n_ios++;
		} while (should_continue && (n_ios < n_max_batch_size));
	} else {		// Best method, remove all io's from submition ring and send them all to execution
		static constexpr const int n_max_batch_size = io_csring::CAPACITY;
		backend_io_executor *arr[n_max_batch_size];
		do {
			BUG_ON(n_ios > n_max_batch_size, "Impossible: %d > %d\n", n_ios , n_max_batch_size);
			arr[n_ios] = new backend_io_executor(addr, *this);
			should_continue = arr[n_ios]->has_io_to_do();
			n_ios++;
		} while (should_continue);
		for (int i = 0; i < n_ios; i++)
			arr[i]->run();
	}
}

int global_srvr_context_imp::run_once_impl(void) {
	if (exit_error_code)
		return exit_error_code;
	if (!has_conencted_client()) {
		client_accept(this->ca);		// Blocking accept client because nothing else to do, no io
		return exit_error_code;
	}
	MGMT::msg_content msg;
	connect_addr addr = this->ca;
	const enum io_state io_st = __read_1_full_message(io_sock, msg, false, addr);
	if (io_st != ios_ok) {
		BUG_ON(io_st == ios_block, "Server listener is blocking");
		pr_errS(this, "receive type=%c, io_state=%d, " PRINT_EXTERN_ERR_FMT "\n", sock.get_type(), io_st, PRINT_EXTERN_ERR_ARGS);
		client_reject();
		return exit_error_code;			// On next iteration, accept new client
	}
	char clnt_path[32];
	sock.print_address(clnt_path, addr);
	if (msg.is(MGMT::msg::dp_submit))
		pr_verbS(this, "<< |%s|\n", msg.raw());
	else
		pr_infoS(this, "<< |%s| from %s\n", msg.raw(), clnt_path);
	if (msg.is(MGMT::msg::hello)) {
		const auto *p = &msg.pay.c_hello;
		char cid[sizeof(p->client_id)+1];
		sprintf(cid, "%.*s", (int)sizeof(p->client_id), p->client_id);
		BUG_ON(p->security_cookie[0] == 0, "Wrong secutiry from client %s\n", cid);
		this->binfo = par.vfuncs.open1(par.vfuncs.caller_context, cid);
		const size_t n_send_bytes = msg.build_hel_ack();
		ASSERT_IN_PRODUCTION(io_csring::is_big_enough_for(binfo.num_max_inflight_io));
		msg.pay.s_hello_ack.info = binfo;
		if (binfo.bdev_descriptor <= 0) {
			pr_errS(this, "Open failed by backend rv=%d\n", binfo.bdev_descriptor);
			msg.pay.s_hello_ack.info.bdev_descriptor = 0;
		}
		// Todo: Initialize datapath of consumer here
		send_to(msg, n_send_bytes, addr);
	} else if (msg.is(MGMT::msg::register_buf)) {
		const int reg_rv = __clnt_bufs_register(msg);
		const size_t n_send_bytes = msg.build_reg_ack();
		msg.pay.s_register_ack.server_pointer = 0x0;		// Not needed
		msg.pay.s_register_ack.rv = reg_rv;		// Leave other fields untouched
		send_to(msg, n_send_bytes, addr);
	} else if (msg.is(MGMT::msg::unreg_buf)) {
		const int reg_rv = __clnt_bufs_unregist(msg);
		const size_t n_send_bytes = msg.build_unr_ack();
		msg.pay.s_unreg_ack.server_pointer = 0x0;		// Not needed
		msg.pay.s_unreg_ack.rv = reg_rv;		// Leave other fields untouched
		send_to(msg, n_send_bytes, addr);
	} else if (msg.is(MGMT::msg::close_nice)) {
		__clnt_close("nice_close");
		const size_t n_send_bytes = msg.build_cl_ack();
		send_to(msg, n_send_bytes, addr);
	} else if (msg.is(MGMT::msg::die_now)) {
		do_shut_down(__LINE__);			// Successfull kill by client
		__clnt_close("suicide");
		const size_t n_send_bytes = msg.build_die_ack();
		strcpy(msg.pay.s_die.extra_info, "cdie");
		send_to(msg, n_send_bytes, addr);
		client_reject();
	} else if (msg.is(MGMT::msg::log)) {
		const char*p = msg.pay.c_log.extra_info;
		while (*p == ' ') p++;
		pr_flush();
		pr_alert("\n\n\n%s\n", p);
		const size_t n_send_bytes = msg.build_ping();
		strcpy(msg.pay.s_kal.extra_info, " LOG-OK");
		send_to(msg, n_send_bytes, addr);
	} else if (msg.is(MGMT::msg::dp_submit)) {
		__clnt_on_io_receive(msg, addr);
	} else {
		strncpy(msg.pay.wrong_cmd.extra_info, msg.raw(), sizeof(msg.hdr));		// Copy header of wrong message
		const size_t n_send_bytes = msg.build_wrong();
		send_to(msg, n_send_bytes, addr);
	}
	return exit_error_code;
}

int global_srvr_context_imp::init_impl(void) {
	int rv = 0;
	#define abort_exe_init_on_err() { pr_errS(this, "Error in line %d\n", __LINE__); do_shut_down(-__LINE__); return this->exit_error_code; }
	if (this->start() != 0)
		abort_exe_init_on_err()
	const sock_t::type s_type = MGMT::get_com_type(par.listen_address);
	const bool blocking_accept = true;
	if (     s_type == sock_t::type::S_UDP) rv = sock.srvr_listen(MGMT::COMM_PORT, false, ca, blocking_accept);
	else if (s_type == sock_t::type::S_TCP) rv = sock.srvr_listen(MGMT::COMM_PORT, true , ca, blocking_accept);
	else if (s_type == sock_t::type::S_UDS) rv = sock.srvr_listen(par.listen_address,     ca, blocking_accept);
	else {
		pr_errS(this, "unsupported listen address |%s|\n", par.listen_address);
		abort_exe_init_on_err()
	}
	if (rv < 0)
		abort_exe_init_on_err();
	pr_infoS(this, "initialized: conn=%c, {%s:%u}, rv=%d\n", sock.get_type(), par.listen_address, MGMT::COMM_PORT, rv);
	is_initialized = true;
	return 0;
}

int global_srvr_context_imp::destroy_impl(void) {
	pr_flush();
	pr_infoS(this, "terminate: conn=%c, {%s:%u}\n", sock.get_type(), par.listen_address, MGMT::COMM_PORT);
	if (sock.get_type() == sock_t::type::S_UDS)
		unlink(par.listen_address);
	sock.nice_close();
	free((char*)par.server_name);
	const int rv = finish(NV_COL_PURPL, 0);
	is_initialized = false;
	return rv;
}

/********************************************************/
int global_srvr_context::init(const struct init_params& _par) noexcept {
	global_srvr_context_imp* g = _impl(this);
	if (g->is_initialized) {
		pr_err1("already initialized\n");
		return EEXIST;	// Success
	}
	g->par = _par;
	g->par.server_name = _par.server_name ? strdup(_par.server_name) : strdup("GS");	// dup srvr name string
	return g->init_impl();
}

int global_srvr_context::destroy(void) noexcept {
	global_srvr_context_imp* g = _impl(this);
	if (!g->is_initialized) {
		pr_err1("not initialized, nothing to destroy\n");
		return ENOENT;
	}
	return g->destroy_impl();
}

int global_srvr_context::run(void) noexcept {
	global_srvr_context_imp* g = _impl(this);
	if (!g->is_initialized) {
		pr_err1("not initialized, cannot run\n");
		return ENOENT;
	}
	if (g->par.has_external_polling_loop) {
		return g->run_once_impl();
	} else {
		int run_rv;
		for (run_rv = 0; run_rv == 0; run_rv = g->run_once_impl()) {
		}
		return run_rv;
	}
}

} // namespace gusli