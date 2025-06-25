#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "server_imp.hpp"

namespace gusli {
#include "spdk_api.hpp"

/******************************** Communicate with client ********************/
int global_srvr_context_imp::__clnt_bufs_register(const MGMT::msg_content &msg) {
	const auto *pr = &msg.pay.c_register_buf;
	const uint64_t n_bytes = (uint64_t)pr->num_blocks * par.binfo.block_size;
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
	pr_info1("Register[%d%c].vec[%d] " PRINT_IO_BUF_FMT ", n_blocks=0x%lx, clnt_ptr=0x%lx, rv=%d, name=%s\n", buf_idx, (pr->is_io_buf ? 'i' : 'r'), (int)dp.shm_io_bufs.size()-1, PRINT_IO_BUF_ARGS(buf), (n_bytes / par.binfo.block_size), pr->client_pointer, rv, pr->name);
	BUG_ON(rv, "Todo: instead of relying that clnt/server addresses are identical, IO should talk in buffer indices");
	return rv;
}

int global_srvr_context_imp::__clnt_bufs_unregist(const MGMT::msg_content &msg) {
	const auto *pr = &msg.pay.c_unreg_buf;
	const int vec_idx = dp.shared_buf_find(pr->buf_idx);
	if (vec_idx < 0) return -1;
	const uint64_t n_bytes = (uint64_t)pr->num_blocks * par.binfo.block_size;
	const io_buffer_t buf = {.ptr = NULL, .byte_len = n_bytes};
	ASSERT_IN_PRODUCTION(pr->is_io_buf == true);
	const int rv = 0;
	const int buf_idx = (pr->is_io_buf ? (int)pr->buf_idx : -1);
	dp.shm_io_bufs.erase(dp.shm_io_bufs.begin() + vec_idx);
	pr_info1("UnRegist[%d%c].vec[%d] " PRINT_IO_BUF_FMT ", n_blocks=0x%lx, clnt_ptr=0x%lx, rv=%d, name=%s\n", buf_idx, (pr->is_io_buf ? 'i' : 'r'), vec_idx, PRINT_IO_BUF_ARGS(buf), (n_bytes / par.binfo.block_size), pr->client_pointer, rv, pr->name);
	return rv;
}

int global_srvr_context_imp::send_to(sock_t& io_sock, MGMT::msg_content &msg, size_t n_bytes, const struct connect_addr &addr) const {
	ssize_t send_rv;
	if (msg.is(MGMT::msg::dp_complete))
		pr_verb1(" >> type=%c, fd=%d, msg=%s\n", io_sock.get_type(), io_sock.fd(), msg.raw());
	else
		pr_info1(" >> type=%c, fd=%d, msg=%s\n", io_sock.get_type(), io_sock.fd(), msg.raw());
	send_rv = io_sock.send_msg(msg.raw(), n_bytes, addr);
	if (send_rv != (ssize_t)n_bytes)
		pr_emerg("%s: Send Error rv=%ld, n_bytes=%lu, msg=%s\n", LIB_NAME, send_rv, n_bytes, msg.raw());
	return (send_rv == (ssize_t)n_bytes) ? 0 : -1;
}

int global_srvr_context_imp::run(void) {
	MGMT::msg_content msg;
	connect_addr addr = this->ca;
	sock_t io_sock;
	int rv = 0, client_fd = -1;
	if (sock.uses_connection()) {
		if ((client_fd = sock.srvr_accept_clnt(addr)) < 0) {
			pr_info1("accept failed: " PRINT_EXTERN_ERR_FMT "\n", PRINT_EXTERN_ERR_ARGS);
		}
		char clnt_path[32];
		sock.print_address(clnt_path, addr);
		pr_info1("accept a_fd=%d, c_fd=%d, path=%s\n", sock.fd(), client_fd, clnt_path);
		io_sock = sock_t(client_fd, sock.get_type());
		io_sock.set_blocking(false);
	} else {
		io_sock = sock;
	}
	io_sock.set_io_buffer_size(1<<19, 1<<19);
	while (!shutting_down) {
		if (__read_1_full_message(io_sock, msg, true, false, addr) != ios_ok) {
			pr_err1("receive type=%c, " PRINT_EXTERN_ERR_FMT "\n", sock.get_type(), PRINT_EXTERN_ERR_ARGS);
			/*if ((errno == EAGAIN || errno == EINTR)) {	// Woke up after 1 second, without incomming message
				if (!addr.is_empty()) {
					const size_t n_send_bytes = msg.build_ping();
					strcpy(msg.pay.s_kal.extra_info, "??");
					if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
						return -1;
				}
				continue;
			}*/
			return rv;
		}
		char clnt_path[32];
		sock.print_address(clnt_path, addr);
		if (msg.is(MGMT::msg::dp_submit))
			pr_verb1("<< |%s|\n", msg.raw());
		else
			pr_info1("<< |%s| from %s\n", msg.raw(), clnt_path);
		if (msg.is(MGMT::msg::hello)) {
			const auto *p = &msg.pay.c_hello;
			char cid[sizeof(p->client_id)+1];
			sprintf(cid, "%.*s", (int)sizeof(p->client_id), p->client_id);
			BUG_ON(p->security_cookie[0] == 0, "Wrong secutiry from client %s\n", cid);
			#if SUPPORT_SPDK
				const int rv_open = spdk_bdev_connect(this);
			#else
				const int rv_open = par.vfuncs.open(par.vfuncs.caller_context, cid);
			#endif
			const size_t n_send_bytes = msg.build_hel_ack();
			msg.pay.s_hello_ack.info = par.binfo;
			if (rv_open != 0)
				msg.pay.s_hello_ack.info.bdev_descriptor = -1;
			// Todo: Initialize datapath of consumer here
			if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
				return -1;
		} else if (msg.is(MGMT::msg::register_buf)) {
			const int reg_rv = __clnt_bufs_register(msg);
			const size_t n_send_bytes = msg.build_reg_ack();
			msg.pay.s_register_ack.server_pointer = 0x0;		// Not needed
			msg.pay.s_register_ack.rv = reg_rv;		// Leave other fields untouched
			if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
				return -1;
		} else if (msg.is(MGMT::msg::unreg_buf)) {
			const int reg_rv = __clnt_bufs_unregist(msg);
			const size_t n_send_bytes = msg.build_unr_ack();
			msg.pay.s_unreg_ack.server_pointer = 0x0;		// Not needed
			msg.pay.s_unreg_ack.rv = reg_rv;		// Leave other fields untouched
			if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
				return -1;
		} else if (msg.is(MGMT::msg::close_nice)) {
			#if SUPPORT_SPDK
				this->spdk_dev.close();
			#else
				par.vfuncs.close(par.vfuncs.caller_context, "????");
			#endif
			char str[256];
			stats.print_stats(str, sizeof(str));
			pr_info1("stats{%s}\n", str);
			const size_t n_send_bytes = msg.build_cl_ack();
			dp.destroy();
			if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
				return -1;
		} else if (msg.is(MGMT::msg::die_now)) {
			this->shutting_down = true;
			const size_t n_send_bytes = msg.build_die_ack();
			if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
				return -1;
		} else if (msg.is(MGMT::msg::log)) {
			const char*p = msg.pay.c_log.extra_info;
			while (*p == ' ') p++;
			pr_flush();
			pr_alert("\n\n\n%s\n", p);
			const size_t n_send_bytes = msg.build_ping();
			strcpy(msg.pay.s_kal.extra_info, " LOG-OK");
			if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
				return -1;
		} else if (msg.is(MGMT::msg::dp_submit)) {
			io_request io;
			bool need_wakeup_clnt_io_submitter = false, need_wakeup_clnt_comp_reader = false, wake = false;
			int idx = dp.srvr_receive_io(io, &need_wakeup_clnt_io_submitter);
			if (idx < 0) {
				//pr_info1("...ignore comp\n");
				continue;
			}
			for (; idx >= 0; idx = dp.srvr_receive_io(io, &wake)) {
				need_wakeup_clnt_io_submitter |= wake;
				stats.inc(io);
				#if SUPPORT_SPDK
					__spdk_exec_io(io);
				#else
					par.vfuncs.exec_io(par.vfuncs.caller_context, io);	// Execute IO on backend, syncronously....
				#endif
				// Assume IO finished with success: io.out.rv = 0;
				const int cmp_idx = dp.srvr_finish_io(io, &wake);
				need_wakeup_clnt_comp_reader |= wake;
				pr_verb1(PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT PRINT_IO_CQE_ELEM_FMT ".clnt_io_ptr=%p, doorbell=%u\n", PRINT_IO_REQ_ARGS(io.params), idx, cmp_idx, io.params.completion_context, wake);
			}
			if (need_wakeup_clnt_io_submitter || need_wakeup_clnt_comp_reader) {
				// Send wakeup completion to client on all executed IO's
				const size_t n_send_bytes = msg.build_dp_comp();
				auto *p = &msg.pay.dp_complete;
				p->sender_added_new_work = need_wakeup_clnt_comp_reader;
				p->sender_ready_for_work = need_wakeup_clnt_io_submitter;
				stats.inc(*p);
				if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
					return -1;
			}
		} else {
			strncpy(msg.pay.wrong_cmd.extra_info, msg.raw(), sizeof(msg.hdr));		// Copy header of wrong message
			const size_t n_send_bytes = msg.build_wrong();
			if (send_to(io_sock, msg, n_send_bytes, addr) < 0)
				return -1;
		}
	}
	if (client_fd > 0) close(client_fd);
	return 0;
}

int global_srvr_context_imp::init(void) {
	int rv = 0;
	#define abort_exe_init_on_err() { pr_err1("Error in line %d\n", __LINE__); shutting_down = true; return -__LINE__; }
	if (!io_csring::is_big_enough_for(par.binfo.num_max_inflight_io))
		abort_exe_init_on_err()
	if (this->start() != 0)
		abort_exe_init_on_err()
	if (     MGMT::com_type == sock_t::type::S_UDP) rv = sock.srvr_listen(MGMT::COMM_PORT, false     , ca);
	else if (MGMT::com_type == sock_t::type::S_TCP) rv = sock.srvr_listen(MGMT::COMM_PORT, true      , ca);
	else if (MGMT::com_type == sock_t::type::S_UDS) rv = sock.srvr_listen(MGMT::COMM_UNIX_DOMAIN_SOCK, ca);
	else BUG_NOT_IMPLEMENTED();
	if (rv < 0)
		abort_exe_init_on_err();
	pr_note1("initialized: conn=%c, {port=%u/uds=%s}, rv=%d\n", sock.get_type(), MGMT::COMM_PORT, MGMT::COMM_UNIX_DOMAIN_SOCK, rv);
	return 0;
}

/********************************************************/
int global_srvr_context::run(const struct init_params& _par) {
	global_srvr_context_imp* g = _impl(this);
	g->par = _par;
	int rv;
	rv = g->init(); if (rv < 0) return rv;
	spdk_init();
	rv = g->run();
	pr_flush();
	unlink(MGMT::COMM_UNIX_DOMAIN_SOCK);
	g->sock.nice_close();
	return g->finish(NV_COL_PURPL, 0);
}

} // namespace gusli