#include <stdlib.h>
#include <errno.h>
#include "00utils/utils.hpp"
#include "client_imp.hpp"
#include "io_executors.hpp"
#include <memory>				// unique_ptr static initialization

namespace gusli {

global_clnt_context& global_clnt_context::get(void) noexcept {
	static std::unique_ptr<global_clnt_context_imp> gc_ctx =  std::make_unique<global_clnt_context_imp>();
	return *gc_ctx;
}
static inline       global_clnt_context_imp* _impl(      global_clnt_context* g) { return       (global_clnt_context_imp*)g; }
static inline const global_clnt_context_imp* _impl(const global_clnt_context* g) { return (const global_clnt_context_imp*)g; }

/************************************ parsing ********************************/
int global_clnt_context_imp::parse_conf(void) {
	if (par.config_file == NULL)
		return -__LINE__;
	const bool is_file_path = (strncmp(par.config_file, "# ", 2) ? true : false);
	char buf[1024], *buf_end = NULL;
	int parse_rv = 0;
	if (is_file_path) {
		FILE* f = fopen(par.config_file, "rt");
		if (f) {		// Read the config to buffer
			const long rv = fread(buf, 1, sizeof(buf), f);
			if (rv >= 0) {
				buf_end = &buf[rv];
				buf[rv] = 0;
				if (!feof(f))
					parse_rv = -__LINE__;	// Buffer is too small to read the entire config
			} else {
				parse_rv = -__LINE__;
			}
			fclose(f);
		} else {
			parse_rv = -__LINE__;
		}
	} else {
		const int rv = strlen(par.config_file);
		if (rv >= (int)sizeof(buf))	// Buffer is too small to read the entire config
			parse_rv = -__LINE__;
		else
			strcpy(buf, par.config_file);
		buf_end = buf + rv;
	}
	if (parse_rv == 0) {
		pr_info1("library config: %s\n", buf);
		parse_rv = bdevs.parse_conf(buf, buf_end);
	}
	return parse_rv;
}

int global_clnt_context::init(const struct init_params& _par) noexcept {
	global_clnt_context_imp* g = _impl(this);
	if (g->is_initialized()) {
		pr_err1("already initialized: %u[devices], doing nothing\n", g->bdevs.n_devices);
		return EEXIST;	// Success
	}
	#define abort_exe_init_on_err() { pr_err1("Error in line %d\n", __LINE__); g->shutting_down = true; return -__LINE__; }
	g->par = _par;
	tDbg::log_file_set(g->par.log);
	g->par.client_name = _par.client_name ? strdup(_par.client_name) : strdup("client1");	// dup client name string
	if (!io_csring::is_big_enough_for(g->par.max_num_simultaneous_requests))
		abort_exe_init_on_err()
	if (g->start() != 0)
		abort_exe_init_on_err()
	const int rv = g->parse_conf();
	pr_note1("initialized: %u devices, log_fd=%u rv=%d\n", g->bdevs.n_devices, fileno(g->par.log), rv);
	sprintf(g->lib_info_json, this->metadata_json_format, LIB_NAME, __stringify(VER_TAGID) , COMMIT_ID, this->BREAKING_VERSION);
	return rv;
}

const char *global_clnt_context::get_metadata_json(void) const noexcept {
	return _impl(this)->lib_info_json;
}

int global_clnt_context::destroy(void) noexcept {
	global_clnt_context_imp* g = _impl(this);
	if (!g->is_initialized()) {
		pr_err1("not initialized, nothing to destroy\n");
		return ENOENT;
	}
	if (g->bdevs.has_any_bdev_open())
		return -1;
	free((char*)g->par.client_name);
	g->bdevs.clear();
	memset(g->lib_info_json, 0, sizeof(g->lib_info_json));
	ASSERT_IN_PRODUCTION(g->is_initialized() == false);
	return g->finish(LIB_COLOR, 0);
}

#include <sys/stat.h>
#include <sys/ioctl.h>
enum connect_rv global_clnt_context::bdev_connect(const struct backend_bdev_id& id) noexcept {
	const global_clnt_context_imp* g = _impl(this);
	server_bdev *bdev = g->bdevs.find_by(id);
	static constexpr const mode_t blk_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // rw-r--r--
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	const int o_flag = (O_RDWR | O_CREAT | O_LARGEFILE) | (bdev->conf.is_direct_io ? O_DIRECT : 0);
	pr_info1("Open bdev uuid=%.16s, type=%c, path=%s, flag=0x%x\n", id.uuid, bdev->conf.type, bdev->conf.conn.local_bdev_path, o_flag);
	if (bdev->is_alive())
		return C_REMAINS_OPEN;
	struct bdev_info *info = &bdev->b.info;
	info->clear();
	info->num_max_inflight_io = 256;
	if (bdev->conf.type == DUMMY_DEV_FAIL) {
		info->bdev_descriptor = 2;
		info->block_size = 4096;
		strcpy(info->name, "FAIL_DEV");
		info->num_total_blocks = (1 << 10);		// 4[MB] dummy
		return C_OK;
	} else if (bdev->conf.type == FS_FILE) {
		info->bdev_descriptor = open(bdev->conf.conn.local_file_path, o_flag, blk_mode);
		if (info->bdev_descriptor > 0) {
			info->block_size = 1;
			strcpy(info->name, "LocalFile");
			info->num_total_blocks = (1 << 30);	// 1[GB] file
			return C_OK;
		} else
			return C_WRONG_ARGUMENTS;
	} else if (bdev->conf.type == KERNEL_BDEV) {
		info->bdev_descriptor = open(bdev->conf.conn.local_bdev_path, o_flag, blk_mode);
		if (info->bdev_descriptor > 0) {
			struct stat sb;
			const int rv0 = fstat(info->bdev_descriptor, &sb);
			// unsigned long long size = 0; const int rv1 = ioctl(info->bdev_descriptor, BLKGETSIZE64, &size);
			if (rv0 != 0) {
				close(info->bdev_descriptor);
				return C_NO_DEVICE;
			}
			info->block_size = sb.st_blksize;
			if (sb.st_size) {
				info->num_total_blocks = sb.st_size;
			}
			snprintf(info->name, sizeof(info->name), "Kernel_bdev(%u,0x%x)", (int)sb.st_dev, (int)sb.st_rdev);
			return C_OK;
		} else {
			return C_NO_DEVICE;
		}
	} else if (bdev->conf.type == NVMESH_UM) {
		bdev->b.hand_shake(id, bdev->conf.conn.remot_sock_addr, g->par.client_name);
		if (info->is_valid())
			return C_OK;
		return C_NO_RESPONSE;
	}
	return C_NO_DEVICE;
}

enum connect_rv global_clnt_context::bdev_bufs_register(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	server_bdev *bdev = _impl(this)->bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	if (!bdev->is_alive()) {
		return C_NO_RESPONSE;
	} else if (bdev->conf.type == NVMESH_UM) {
		enum connect_rv rv = C_WRONG_ARGUMENTS;
		for (size_t i = 0; i < bufs.size(); i++) {
			const int map_rv = bdev->b.map_buf(id, bufs[i]);
			rv = (map_rv == 0) ? C_OK : C_WRONG_ARGUMENTS;
		}
		return rv;
	} else if ((bdev->conf.type == FS_FILE) || (bdev->conf.type == KERNEL_BDEV)) {
		bdev->b.f.n_mapped_bufs += (uint32_t)bufs.size();
		pr_info1("Register[%u]bufs, remains[%u] name=%s\n", (uint32_t)bufs.size(), bdev->b.f.n_mapped_bufs, bdev->b.info.name);
		return C_OK;
	} else {
		return C_NO_DEVICE;
	}
}

enum connect_rv global_clnt_context::bdev_bufs_unregist(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	server_bdev *bdev = _impl(this)->bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	if (!bdev->is_alive()) {
		return C_NO_RESPONSE;
	} else if (bdev->conf.type == NVMESH_UM) {
		enum connect_rv rv = C_WRONG_ARGUMENTS;
		for (int i = (int)bufs.size()-1; i >= 0; i--) {
			const int map_rv = bdev->b.map_buf_un(id, bufs[i]);
			rv = (map_rv == 0) ? C_OK : C_WRONG_ARGUMENTS;
		}
		return rv;
	} else if ((bdev->conf.type == FS_FILE) || (bdev->conf.type == KERNEL_BDEV)) {
		if (bdev->b.f.n_mapped_bufs < bufs.size())		// Only counter, not really map of buffers
			return C_WRONG_ARGUMENTS;
		bdev->b.f.n_mapped_bufs -= (uint32_t)bufs.size();
		pr_info1("UnRegist[%u]bufs, remains[%u] name=%s\n", (uint32_t)bufs.size(), bdev->b.f.n_mapped_bufs, bdev->b.info.name);
		return C_OK;
	} else {
		return C_NO_DEVICE;
	}
}

uint32_t server_bdev::get_num_uses(void) const {
	nvTODO("Count in air io's, not only registered mem bufs");
	if ((conf.type == FS_FILE) || (conf.type == KERNEL_BDEV))
		return b.f.n_mapped_bufs;
	if (conf.type == NVMESH_UM)
		return (uint32_t)b.dp.shm_io_bufs.size();
	return 0;
}

static enum connect_rv __bdev_disconnect(server_bdev *bdev, const bool do_suicide) {
	const struct backend_bdev_id& id = bdev->id;
	enum connect_rv rv = C_OK;
	if (!bdev->is_alive()) {
		rv = C_NO_RESPONSE;
	} else if (bdev->is_still_used()) {
		pr_err1("Error: name=%s, still have %u[mapped-buffers]\n", bdev->b.info.name, bdev->get_num_uses());
		return C_REMAINS_OPEN;
	} else if (bdev->conf.type == DUMMY_DEV_FAIL) {
	} else if (bdev->conf.type == FS_FILE) {
		close(bdev->get_fd());
		const int remove_rv = remove(bdev->conf.conn.local_file_path);
		rv = (remove_rv == 0) ? C_OK : C_NO_RESPONSE;
	} else if (bdev->conf.type == KERNEL_BDEV) {
		close(bdev->get_fd());
	} else if (bdev->conf.type == NVMESH_UM) {
		bdev->b.close(id, do_suicide);
	} else {
		rv = C_NO_DEVICE;
	}
	pr_info1("Close bdev uuid=%.16s name=%s, type=%c, rv=%d\n", id.uuid, bdev->b.info.name, bdev->conf.type, rv);
	bdev->get_fd() = -1;
	return rv;
}

enum connect_rv global_clnt_context::bdev_disconnect(const struct backend_bdev_id& id) noexcept {
	server_bdev *bdev = _impl(this)->bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	return __bdev_disconnect(bdev, false);
}

void global_clnt_context::bdev_report_data_corruption(const backend_bdev_id& id, uint64_t offset_lba_bytes) noexcept {
	server_bdev *bdev = _impl(this)->bdevs.find_by(id);
	pr_err1("Error: User reported data corruption on uuid=%.16s, lba=0x%lx[B]\n", id.uuid, offset_lba_bytes);
	if (bdev) {
		t_lock_guard l(bdev->control_path_lock);
		__bdev_disconnect(bdev, true);
	}
}

enum connect_rv global_clnt_context::bdev_get_info(const struct backend_bdev_id& id, struct bdev_info *ret_val) noexcept {
	server_bdev *bdev = _impl(this)->bdevs.find_by(id);
	if (!bdev) return C_NO_DEVICE;
	if (!bdev->is_alive()) return C_NO_RESPONSE;
	t_lock_guard l(bdev->control_path_lock);
	*ret_val = bdev->b.info;
	return C_OK;
}

enum connect_rv global_clnt_raii::bufs_register(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	global_clnt_context &c = global_clnt_context::get();
	global_clnt_context_imp* g = _impl(&c);
	server_bdev *bdev = g->bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	if (!bdev->is_alive()) {						// Try to auto open bdev
		const enum connect_rv rv = c.bdev_connect(id);
		if (rv != C_OK)
			return rv;
	}
	return global_clnt_context::get().bdev_bufs_register(id, bufs);
}
enum connect_rv global_clnt_raii::bufs_unregist(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	global_clnt_context &c = global_clnt_context::get();
	global_clnt_context_imp* g = _impl(&c);
	server_bdev *bdev = g->bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	const enum connect_rv rv = c.bdev_bufs_unregist(id, bufs);
	if (rv != C_OK)
		return rv;
	if (!bdev->is_still_used())
		(void)__bdev_disconnect(bdev, false);		// User has nothing to do with close failure
	return C_OK;
}

int32_t global_clnt_raii::get_bdev_descriptor(const backend_bdev_id& id) noexcept {
	bdev_info rv;
	if (global_clnt_context::get().bdev_get_info(id, &rv) == connect_rv::C_OK)
		return rv.bdev_descriptor;
	return -1;
}

/*****************************************************************************/
void io_request::submit_io(void) noexcept {
	BUG_ON(out.rv == io_error_codes::E_IN_TRANSFER, "memory corruption: attempt to retry io[%p] before prev execution completed!", this);
	out.rv = io_error_codes::E_IN_TRANSFER;
	server_bdev *bdev = NULL;
	if (params.bdev_descriptor > 0)
		bdev = ((global_clnt_context_imp*)&global_clnt_context::get())->bdevs.find_by(params.bdev_descriptor);
	if (unlikely(!bdev)) {
		pr_err1("Error: Invalid bdev descriptor of io=%d. Open bdev to obtain a valid descriptor\n", params.bdev_descriptor);
		io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
	} else if ((bdev->conf.type == FS_FILE) || (bdev->conf.type == KERNEL_BDEV)) {
		BUG_ON(_exec != NULL, "BUG: IO is still running! wait for completion or cancel, before retrying it");
		#if defined(HAS_URING_LIB)
			if (!has_callback() && params.try_using_uring_api)	// Uring does not support async callback mode
				_exec = new uring_request_executor(*this);
		#endif
		if (!_exec) {
			if (!this->is_blocking_io()) {					// Async IO, with / without completion
				_exec = new aio_request_executor(*this);
			} else {										// Blocking IO
				_exec = new sync_request_executor(*this);
			}
		}
		if (_exec)
			_exec->run();									// Will auto delete exec upon IO finish;
		else {
			io_autofail_executor(*this, io_error_codes::E_INTERNAL_FAULT); // Out of memory error
		}
	} else if (bdev->conf.type == DUMMY_DEV_FAIL) {
		io_autofail_executor(*this, io_error_codes::E_PERM_FAIL_NO_RETRY);	// Here, injection of all possible errors
	} else if (bdev->conf.type == NVMESH_UM) {
		const bool should_block = this->is_blocking_io();
		if (unlikely(should_block)) {
			_exec = new remote_aio_blocker(*this);
			if (_exec)
				_exec->run();
		}
		bool need_wakeup_srvr;
		const int rv = bdev->b.dp.clnt_send_io(*this, &need_wakeup_srvr);
		if (rv >= 0) {
			pr_verb1(PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT "        .clnt_io_ptr=%p, doorbell=%d\n", PRINT_IO_REQ_ARGS(params), rv, this, need_wakeup_srvr);
			if (need_wakeup_srvr)
				ASSERT_IN_PRODUCTION(bdev->b.dp_wakeup_server() == 0);
			// Callback will come in future
		} else {
			/* Callback already arrived, 'this' can be free already as it returned a callback on failed async io */
		}
		if (should_block)
			_exec->is_still_running();		// Blocking wait, for success or failure
	} else {
		io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
	}
}

io_request_executor_base* io_request::__disconnect_executor_atomic(void) noexcept {
	if (!_exec)
		return nullptr;
	uint64_t *ptr = (uint64_t *)&_exec;
	return (io_request_executor_base*)__atomic_exchange_n(ptr, NULL, __ATOMIC_SEQ_CST);
}

enum io_error_codes io_request::get_error(void) noexcept {
	if (out.rv == io_error_codes::E_IN_TRANSFER) {
		DEBUG_ASSERT(!is_blocking_io());
		if (params._async_no_comp) {
			BUG_ON(!_exec, "IO has not finished yet, It must have a valid executor, rv=%ld", out.rv);
			if (_exec->is_still_running() == io_error_codes::E_IN_TRANSFER)
				return io_error_codes::E_IN_TRANSFER;
			// Here executor terminated but maybe did not update the io rv yet, so check it again.
			if (out.rv == io_error_codes::E_IN_TRANSFER)
				return io_error_codes::E_IN_TRANSFER;
		} else {
			return io_error_codes::E_IN_TRANSFER;	// Cannot touch async executor
		}
	}
	auto* orig_exec = __disconnect_executor_atomic();	// IO finished
	if (orig_exec) {
		ASSERT_IN_PRODUCTION(out.rv != io_error_codes::E_IN_TRANSFER);	// IO has finished
		orig_exec->detach_io();	// Disconnect executor from io
	}
	if (out.rv > 0) {
		DEBUG_ASSERT(out.rv == (int64_t)params.buf_size());					// No partial io
		return E_OK;
	}
	return (enum io_error_codes)out.rv;
}

enum io_request::cancel_rv io_request::try_cancel(void) noexcept {
	if (out.rv == io_error_codes::E_IN_TRANSFER) {
		DEBUG_ASSERT(!is_blocking_io());			// Impossible for blocking io as out.rv would be already set
		auto* orig_exec = __disconnect_executor_atomic();		// IO finished / Canceled
		if (orig_exec) {		// Executor still running
			const enum cancel_rv crv = orig_exec->cancel();
			if (crv == cancel_rv::G_CANCELED) {
				out.rv = (int64_t)io_error_codes::E_CANCELED_BY_CALLER;
				return io_request::cancel_rv::G_CANCELED;
			} // Else: Already done
		}
	}
	return io_request::cancel_rv::G_ALLREADY_DONE;
}
}	// namespace
/************************* communicate with server *****************************************/
namespace gusli {

int bdev_backend_api::send_to(MGMT::msg_content &msg, size_t n_bytes) const {
	if (msg.is(MGMT::msg::dp_submit))
		pr_verb1(" >> %s: %s\n", srv_addr, msg.raw());
	else
		pr_info1(" >> %s: %s\n", srv_addr, msg.raw());
	//ssize_t send_rv = sock.send_msg(msg.raw(), n_bytes, ca);
	return (ios_ok == __send_1_full_message(sock, msg, false, n_bytes, ca, LIB_NAME)) ? 0 : -1;
}

int bdev_backend_api::hand_shake(const struct backend_bdev_id& id, const char* addr, const char *clnt_name) {
	const sock_t::type s_type = MGMT::get_com_type(addr);
	MGMT::msg_content msg;
	int conn_rv;
	srv_addr = addr;
	(void)id;
	const bool blocking_connect = true;
	if (     s_type == sock_t::type::S_UDP) conn_rv = sock.clnt_connect_to_srvr_udp(MGMT::COMM_PORT, &srv_addr[1], ca, blocking_connect);
	else if (s_type == sock_t::type::S_TCP) conn_rv = sock.clnt_connect_to_srvr_tcp(MGMT::COMM_PORT, &srv_addr[1], ca, blocking_connect);
	else if (s_type == sock_t::type::S_UDS) conn_rv = sock.clnt_connect_to_srvr_uds(                  srv_addr   , ca, blocking_connect);
	else {
		pr_err1("unsupported server address |%s|\n", addr);
		info.bdev_descriptor = -__LINE__;
		return info.bdev_descriptor;
	}
	if (conn_rv < 0) {
		pr_err1("Cannot conenct to |%s|, rv=%d. is server up?\n", addr, conn_rv);
		info.bdev_descriptor = -1;
		goto _out;
	}
	is_control_path_ok = false;
	io_listener_tid = 0;
	{
		const size_t size = msg.build_hello();
		auto *p = &msg.pay.c_hello;
		p->fill(id, clnt_name, security_cookie);
		if (send_to(msg, size) >= 0)
			check_incoming();
		else
			goto _out;
	} {
		if (!info.is_valid())
			goto _out;
		const size_t size = msg.build_reg_buf();
		auto *pr = &msg.pay.c_register_buf;
		// Initialize datapath of producer
		dp.block_size = info.block_size;
		dp.num_total_bytes = info.num_total_blocks * info.block_size;
		const uint64_t n_bytes = align_up(sizeof(io_csring), (uint64_t)dp.block_size);
		pr->build_scheduler(id, n_bytes / (uint64_t)info.block_size);
		ASSERT_IN_PRODUCTION(dp.shm.init_producer(pr->name, n_bytes) == 0);
		*(uint64_t*)dp.shm.get_buf() = MGMT::shm_cookie;		// Insert cookie for the server to verify
		ASSERT_IN_PRODUCTION(sem_init(&wait_control_path, 0, 0) == 0);
		if (send_to(msg, size) >= 0) {
			check_incoming();
		} else
			goto _out;
		dp.get()->init();
	}
	if (MGMT::set_large_io_buffers)
		sock.set_io_buffer_size(1<<19, 1<<19);
	info.bdev_descriptor = sock.fd();
	{
		const int err = pthread_create(&io_listener_tid, NULL, (void* (*)(void*))io_completions_listener, this);
		ASSERT_IN_PRODUCTION(err <= 0);
	}
_out:
	if (info.bdev_descriptor <= 0)
		this->close(id);				// Cleanup open with close
	return info.bdev_descriptor;
}

int bdev_backend_api::map_buf(const backend_bdev_id& id, const io_buffer_t io) {
	if (!io.is_valid_for(info.block_size)) return -1;
	MGMT::msg_content msg;
	const size_t size = msg.build_reg_buf();
	auto *pr = &msg.pay.c_register_buf;
	pr->build_io_buffer(id, io.ptr, (io.byte_len / info.block_size), dp.shm_io_file_running_idx++);
	pr_info1("Register[%ui].vec[%u] " PRINT_IO_BUF_FMT ", n_blocks=0x%lx, name=%s\n", pr->buf_idx, (int)dp.shm_io_bufs.size(), PRINT_IO_BUF_ARGS(io), (io.byte_len / info.block_size), pr->name);
	base_shm_element *new_map = &dp.shm_io_bufs.emplace_back();
	ASSERT_IN_PRODUCTION(new_map->mem.init_producer(pr->name, io.byte_len, (void*)io.ptr) == 0);
	new_map->buf_idx = pr->buf_idx;
	*(uint64_t*)new_map->mem.get_buf() = MGMT::shm_cookie;		// Insert cookie for the server to verify
	ASSERT_IN_PRODUCTION(sem_init(&wait_control_path, 0, 0) == 0);
	if (send_to(msg, size) >= 0) {
		ASSERT_IN_PRODUCTION(sem_wait(&wait_control_path) == 0);
		return 0;
	} else {
		return -1;
	}
}

int bdev_backend_api::map_buf_un(const backend_bdev_id& id, const io_buffer_t io) {
	const int vec_idx = dp.shared_buf_find(io);
	if (vec_idx < 0) return -1;
	MGMT::msg_content msg;
	const size_t size = msg.build_unr_buf();
	auto *pr = &msg.pay.c_unreg_buf;
	pr->build_io_buffer(id, io.ptr, (io.byte_len / info.block_size), dp.shm_io_bufs[vec_idx].buf_idx);
	pr_info1("UnRegist[%di].vec[%d] " PRINT_IO_BUF_FMT ", n_blocks=0x%lx, name=%s\n", pr->buf_idx, vec_idx, PRINT_IO_BUF_ARGS(io), (io.byte_len / info.block_size), pr->name);
	ASSERT_IN_PRODUCTION(strncmp(pr->name, dp.shm_io_bufs[vec_idx].mem.get_producer_name(), sizeof(pr->name)) == 0);
	ASSERT_IN_PRODUCTION(sem_init(&wait_control_path, 0, 0) == 0);
	if (send_to(msg, size) >= 0) {
		ASSERT_IN_PRODUCTION(sem_wait(&wait_control_path) == 0);
		dp.shm_io_bufs.erase(dp.shm_io_bufs.begin() + vec_idx);
		return 0;
	} else {
		return -1;
	}
}

int bdev_backend_api::close(const struct backend_bdev_id& id, const bool do_kill_server) {
	if (io_listener_tid) {
		MGMT::msg_content msg;
		size_t size;
		if (do_kill_server) {
			size = msg.build_die();
		} else {
			size = msg.build_close();
			msg.pay.c_close.volume = id;
		}
		ASSERT_IN_PRODUCTION(send_to(msg, size) >= 0);
		pr_info1("going to join listener_thread tid=0x%lx\n", (long)io_listener_tid);
		const int err = pthread_join(io_listener_tid, NULL);
		ASSERT_IN_PRODUCTION(err == 0);
		io_listener_tid = 0;
	}
	char str[256];
	stats.print_stats(str, sizeof(str));
	pr_info1("stats{%s}\n", str);
	dp.destroy();
	sock.nice_close();
	info.clear();
	return 0;
}

bool bdev_backend_api::check_incoming() {
	connect_addr addr = this->ca;
	MGMT::msg_content msg;
	bool rv = false;
	const enum io_state io_st = __read_1_full_message(sock, msg, false, addr);
	if (io_st != ios_ok) {
		pr_info1("receive type=%c, io_state=%d, " PRINT_EXTERN_ERR_FMT "\n", sock.get_type(), io_st, PRINT_EXTERN_ERR_ARGS);
		if (io_state_broken(io_st))
			is_control_path_ok = false;
		BUG_ON(io_st == ios_block, "Client listener should have blocking wait for io completion, It has no other job to do");
		return rv;
	}
	char server_path[32];
	sock.print_address(server_path, addr);	// Todo: Multiple servers, find the correct one
	if (msg.is(MGMT::msg::dp_complete))
		pr_verb1("<< |%s|\n", msg.raw());
	else
		pr_info1("<< |%s| from %s\n", msg.raw(), server_path);
	if (true) {
		on_keep_alive_received(); // Every message is considered a keep alive
		if (msg.is(MGMT::msg::keepalive)) {
			return false;	// Nothing special to do
		} else if (msg.is(MGMT::msg::hello_ack)) {
			this->info = msg.pay.s_hello_ack.info;
			ASSERT_IN_PRODUCTION(io_csring::is_big_enough_for(info.num_max_inflight_io));
			ASSERT_IN_PRODUCTION(info.block_size >= 1);
		} else if (msg.is(MGMT::msg::register_ack)) {
			const auto *pr = &msg.pay.s_register_ack;
			BUG_ON(pr->rv != 0, "rv=%d", pr->rv);
			ASSERT_IN_PRODUCTION(sem_post(&wait_control_path) == 0);	// Unlock caller which waits for buffer registration
		} else if (msg.is(MGMT::msg::unreg_ack)) {
			const auto *pr = &msg.pay.s_unreg_ack;
			BUG_ON(pr->rv != 0, "rv=%d", pr->rv);
			ASSERT_IN_PRODUCTION(sem_post(&wait_control_path) == 0);	// Unlock caller which waits for buffer registration
		} else if (msg.is(MGMT::msg::server_kick)) {
			BUG_NOT_IMPLEMENTED();
		} else if (msg.is(MGMT::msg::close_ack)) {
			pr_info1("\t%s remote closed\n", this->info.name);
			is_control_path_ok = false;
		} else if (msg.is(MGMT::msg::die_ack)) {
			pr_info1("\t%s Server dies, reason:%s\n", this->info.name, msg.pay.s_die.extra_info);
			is_control_path_ok = false;
		} else if (msg.is(MGMT::msg::dp_complete)) {
			bool need_wakeup_srvr;
			int idx = dp.clnt_receive_completion(&need_wakeup_srvr);
			// BUG_ON(need_wakeup_srvr, LIB_NAME ": completion list full, need to wakup server. Not implemented");
			if (idx < 0)
				return true;
			for (; idx >= 0; idx = dp.clnt_receive_completion(&need_wakeup_srvr)) {
				// BUG_ON(need_wakeup_srvr, LIB_NAME ": completion list full, need to wakup server. Not implemented");
			}
		} else {
			BUG_ON(true, "Unhandled message %s\n", msg.hdr.type);
		}
		return true;	// Need to respond to control path change
	}
	return rv;
}

int bdev_backend_api::dp_wakeup_server(void) {
	MGMT::msg_content msg;
	const size_t size = msg.build_dp_subm();
	auto *p = &msg.pay.dp_submit;
	p->sender_added_new_work = true;
	p->sender_ready_for_work = false;
	stats.n_doorbels_wakeup_srvr++;
	return send_to(msg, size);
}

void* bdev_backend_api::io_completions_listener(bdev_backend_api *bdev) {
	{	// Rename thread
		char old_name[32], new_name[32];
		snprintf(new_name, sizeof(new_name), "%sciol", global_clnt_context::thread_names_prefix);
		pthread_getname_np(pthread_self(), old_name, sizeof(old_name));
		pr_info1("\t\t\tListener started, renaming %s->%s\n", old_name, new_name);
		const int rename_rv = pthread_setname_np(pthread_self(), new_name);
		if (rename_rv != 0)
			pr_err1("rename failed rv=%d " PRINT_EXTERN_ERR_FMT "\n", rename_rv, PRINT_EXTERN_ERR_ARGS);
	}
	for (bdev->is_control_path_ok = true; bdev->is_control_path_ok; ) {
		bdev->check_incoming();
	}
	pr_info1("\t\t\tListener end\n");
	return NULL;
}

}	// namespace
