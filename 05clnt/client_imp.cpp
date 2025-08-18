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
#include <stdlib.h>
#include <errno.h>
#include "00utils/utils.hpp"
#include "client_imp.hpp"
#include "io_executors.hpp"
#include <sys/stat.h>
#include <sys/ioctl.h>

namespace gusli {

global_clnt_context_imp& global_clnt_context_imp::get(void) noexcept {
	//static std::unique_ptr<global_clnt_context_imp> gc_ctx =  std::make_unique<global_clnt_context_imp>(); return *gc_ctx;
	static global_clnt_context_imp gc_ctx;
	return gc_ctx;
}
global_clnt_context_imp::global_clnt_context_imp() : base_library(LIB_NAME) {
	pr_info1("clnt_ctx[%p] - construct %lu[b]\n", this, sizeof(*this));
	shm_io_bufs = shm_io_bufs_global_t::get(LIB_NAME);
}
global_clnt_context_imp::~global_clnt_context_imp() {
	pr_info1("clnt_ctx[%p] - destruct\n", this);
	shm_io_bufs_global_t::put(LIB_NAME);
}

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

int bdev_config_params::init_parse(int version, const char* const argv[], int argc) noexcept {
	BUG_ON(version != 1, "Unsupported version type. Corrupted config?");
	int n_args_read = 0;
	char c;
	if (argc < 6) return -__LINE__;
	n_args_read += sscanf(argv[0], "%16s", id.set_invalid()->uuid);
	n_args_read += sscanf(argv[1], "%c", &c); type = (bdev_config_params::bdev_type)c;
	n_args_read += sscanf(argv[2], "%c", &c); how = (bdev_config_params::connect_how)c;
	n_args_read += sscanf(argv[3], "%c", &c); is_direct_io = (bool)((c == 'D')||(c == 'd'));
	n_args_read += sscanf(argv[4], "%64s", conn.any);
	n_args_read += sscanf(argv[5], "%15s", security_cookie);
	// pr_verb1("+Dev: uuid=%s|%c|%c|%u|con=%s\n", id.uuid, type, how, is_direct_io, conn.any);
	if (n_args_read != argc) return -__LINE__;
	return (is_valid()) ? 0 : -__LINE__;
}

/************************************ API ********************************/
int global_clnt_context_imp::init(const global_clnt_context::init_params& _par, const char* metadata_json_format) noexcept {
	if (is_initialized()) {
		pr_err1("already initialized: %u[devices], doing nothing\n", bdevs.n_devices());
		return EEXIST;	// Success
	}
	#define abort_exe_init_on_err() { pr_err1("Initialization Error in line %d\n", __LINE__); shutting_down = true; free((char*)par.client_name); return -__LINE__; }
	par = _par;
	tDbg::log_file_set(par.log);
	par.client_name = _par.client_name ? strdup(_par.client_name) : strdup("client1");	// dup client name string
	if (!io_csring::is_big_enough_for(par.max_num_simultaneous_requests))
		abort_exe_init_on_err()
	if (start() != 0)
		abort_exe_init_on_err()
	const int rv = parse_conf();
	pr_note1("initialized: %u devices, log_fd=%u rv=%d\n", bdevs.n_devices(), fileno(par.log), rv);
	if (rv != 0)
		abort_exe_init_on_err();
	sprintf(lib_info_json, metadata_json_format, LIB_NAME, __stringify(VER_TAGID) , COMMIT_ID, opt_level, TRACE_LEVEL, __stringify(COMPILATION_DATE));
	return rv;
}

int global_clnt_context_imp::destroy(void) noexcept {
	if (!is_initialized()) {
		pr_err1("not initialized, nothing to destroy\n");
		return ENOENT;
	}
	if (bdevs.has_any_bdev_open()) {
		pr_err1("Destroy aborted, client is still used\n");
		return -1;
	}
	free((char*)par.client_name);
	bdevs.clear();
	return finish(LIB_COLOR, 0);
}

enum connect_rv global_clnt_context_imp::bdev_connect(const backend_bdev_id& id) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	return bdev->connect(par.client_name);
}

enum connect_rv server_bdev::connect(const char* client_name) {
	static constexpr const mode_t blk_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // rw-r--r--
	server_bdev *bdev = this;
	const int o_flag = (O_RDWR | O_CREAT | O_LARGEFILE) | (bdev->conf.is_direct_io ? O_DIRECT : 0);
	pr_info1("Open " PRINT_BDEV_ID_FMT ", flag=0x%x\n", PRINT_BDEV_ID_ARGS(*bdev), o_flag);
	if (bdev->is_alive())
		return C_REMAINS_OPEN;
	bdev_info *info = &bdev->b.info;
	info->clear();
	info->num_max_inflight_io = 256;
	if (bdev->conf.is_dummy()) {
		info->bdev_descriptor = 100000001;
		info->block_size = 4096;
		info->num_max_inflight_io = 4;
		strcpy(info->name, (bdev->conf.type == bdev_config_params::bdev_type::DUMMY_DEV_FAIL) ? "FAIL_DEV" : "STUCK_DEV");
		info->num_total_blocks = (1 << 10);		// 4[MB] dummy
		MGMT::msg_content msg;
		const int rv_dp_init = bdev->b.create_dp(bdev->conf.id, msg);
		return (rv_dp_init >= 0) ? C_OK : C_NO_RESPONSE;
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_FS_FILE) {
		info->bdev_descriptor = open(bdev->conf.conn.local_file_path, o_flag, blk_mode);
		if (info->bdev_descriptor > 0) {
			info->block_size = 1;
			strcpy(info->name, "LocalFile");
			info->num_total_blocks = (1 << 30);	// 1[GB] file
			MGMT::msg_content msg;
			const int rv_dp_init = bdev->b.create_dp(bdev->conf.id, msg);
			return (rv_dp_init >= 0) ? C_OK : C_NO_RESPONSE;
		} else {
			pr_err1(PRINT_BDEV_ID_FMT " Cannot open! " PRINT_EXTERN_ERR_FMT "\n", PRINT_BDEV_ID_ARGS(*bdev), PRINT_EXTERN_ERR_ARGS);
			return C_NO_RESPONSE;
		}
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_BLK_KERNEL) {
		info->bdev_descriptor = open(bdev->conf.conn.local_bdev_path, o_flag, blk_mode);
		if (info->bdev_descriptor > 0) {
			struct stat sb;
			const int rv0 = fstat(info->bdev_descriptor, &sb);
			if (rv0 != 0) {
				close(info->bdev_descriptor);
				return C_NO_DEVICE;
			}
			info->block_size = sb.st_blksize;
			snprintf(info->name, sizeof(info->name), "Kernel_bdev(%u,0x%x)", (int)sb.st_dev, (int)sb.st_rdev);
			if (sb.st_size) {
				info->num_total_blocks = sb.st_size;
			} else {
				const int rv1 = ioctl(info->bdev_descriptor, BLKGETSIZE64, &sb.st_size);
				if ((rv1 >=0) && (sb.st_size != 0)) {
					info->num_total_blocks = sb.st_size;
				} else {
					pr_err1(PRINT_BDEV_ID_FMT " Cannot determine size. Setting default!\n", PRINT_BDEV_ID_ARGS(*bdev));
					info->num_total_blocks = (1 << 30); // Default[GB]
				}
			}
			MGMT::msg_content msg;
			const int rv_dp_init = bdev->b.create_dp(bdev->conf.id, msg);
			return (rv_dp_init >= 0) ? C_OK : C_NO_RESPONSE;
		} else {
			pr_err1(PRINT_BDEV_ID_FMT " Cannot open! " PRINT_EXTERN_ERR_FMT "\n", PRINT_BDEV_ID_ARGS(*bdev), PRINT_EXTERN_ERR_ARGS);
			return C_NO_DEVICE;
		}
	} else if (bdev->conf.is_bdev_remote()) {
		bdev->b.hand_shake(bdev->conf, client_name);
		if (info->is_valid())
			return C_OK;
		return C_NO_RESPONSE;
	}
	return C_NO_DEVICE;
}

enum connect_rv global_clnt_context_imp::bdev_bufs_register(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	if (bufs.empty())
		return C_WRONG_ARGUMENTS;			// Empty vector is invalid
	t_lock_guard l(bdev->control_path_lock);
	if (!bdev->is_alive())
		return C_NO_RESPONSE;
	return bdev->b.map_buf_do_vec(bufs);
}

enum connect_rv bdev_backend_api::bdev_ctl_log_msg1(const std::string &s) const noexcept {
	if (!sock.is_alive())
		return C_NO_RESPONSE;
	MGMT::msg_content msg;
	const size_t size = msg.build_log(s);
	const int send_rv = send_to(msg, size);
	return (send_rv < 0) ? C_NO_RESPONSE : C_OK;
}

enum connect_rv global_clnt_context_imp::bdev_ctl_log_msg2(const backend_bdev_id& id, const std::string &s) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	if (!bdev->is_alive())
		return C_NO_RESPONSE;
	if (!bdev->conf.is_bdev_remote()) {
		pr_info1(PRINT_BDEV_ID_FMT " Just local print %s\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str());
		return C_OK;
	}
	const enum connect_rv rv = bdev->b.bdev_ctl_log_msg1(s);
	if (rv != C_OK)
		pr_err1(PRINT_BDEV_ID_FMT " Error logging to server %s, rv=%d\n", PRINT_BDEV_ID_ARGS(*bdev), s.c_str(), rv);
	return rv;
}

enum connect_rv global_clnt_context_imp::bdev_stop_all_ios(const backend_bdev_id& id, bool do_reconnect) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	const bool is_alive = bdev->is_alive();
	if (!is_alive && !do_reconnect) {
		pr_info(PRINT_BDEV_ID_FMT " Force stop on stopped blockdevice, do nothing!\n", PRINT_BDEV_ID_ARGS(*bdev));
		return C_OK;
	}

	std::vector<io_buffer_t> bufs;
	if (is_alive) {
		const uint32_t num_ios = bdev->b.dp->get_num_in_air_ios();
		if (/*!bdev->b.dp->is_still_used() &&*/ (num_ios == 0) && do_reconnect) {
			pr_info(PRINT_BDEV_ID_FMT " Refresh does nothing!\n", PRINT_BDEV_ID_ARGS(*bdev));
			return C_OK;				// Nothing to refresh
		}
		if (bdev->conf.is_bdev_remote()) {
			pr_info(PRINT_BDEV_ID_FMT " Force disconnect from server to stop getting io completions\n", PRINT_BDEV_ID_ARGS(*bdev));
			bdev->b.force_break_connection();
		}

		pr_info(PRINT_BDEV_ID_FMT " draining %u io's in air\n", PRINT_BDEV_ID_ARGS(*bdev), num_ios);
		server_io_req *stuck_io = bdev->b.dp->in_air.get_next_in_air_io();
		while (stuck_io) {	// For local block devices this loop may do less iterations than num_ios (because io's keep completing). For remote this is impossible because we broke the socket
			const enum io_request::cancel_rv rv = stuck_io->try_cancel(false);
			ASSERT_IN_PRODUCTION(rv == io_request::cancel_rv::G_CANCELED);
			stuck_io = bdev->b.dp->in_air.get_next_in_air_io();
		}

		// Save the list of registerd bufs to reregister them later
		bufs = bdev->b.dp->registerd_bufs_get_list();
		bdev->b.dp->registerd_bufs_force_clean();

		const connect_rv rv_disconnect = bdev->disconnect(false);
		ASSERT_IN_PRODUCTION(rv_disconnect == connect_rv::C_OK);
	}

	if (do_reconnect) {
		const connect_rv rv_reconnect = bdev->connect(par.client_name);
		ASSERT_IN_PRODUCTION(rv_reconnect == connect_rv::C_OK);
		pr_info(PRINT_BDEV_ID_FMT " reregistering %u mem bufs\n", PRINT_BDEV_ID_ARGS(*bdev), (uint32_t)bufs.size());
		const connect_rv rv_rereg = bdev->b.map_buf_do_vec(bufs);
		ASSERT_IN_PRODUCTION(rv_rereg == connect_rv::C_OK);
	}
	return C_OK;
}

enum connect_rv global_clnt_context_imp::bdev_bufs_unregist(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	if (bufs.empty())
		return C_WRONG_ARGUMENTS;			// Empty vector is invalid
	t_lock_guard l(bdev->control_path_lock);
	if (!bdev->is_alive())
		return C_NO_RESPONSE;
	const uint32_t num_ios = bdev->b.dp->get_num_in_air_ios();	// Should be 0. Attempt to unregister bufs with in air IO may lead to memory corruption. Prevent this
	if (num_ios) {
		pr_err1("Error " PRINT_BDEV_ID_FMT " attemt to unregister mem with %u io's in air. This is a wrong flow that may lead to memory corruption!\n", PRINT_BDEV_ID_ARGS(*bdev), num_ios);
		return C_WRONG_ARGUMENTS;
	}
	return bdev->b.map_buf_un_vec(bufs);
}

enum connect_rv server_bdev::disconnect(const bool do_suicide) {
	server_bdev *bdev = this;
	const backend_bdev_id& id = bdev->conf.id;
	enum connect_rv rv = C_OK;
	datapath_t<bdev_stats_clnt> *dp = bdev->b.dp;
	if (!bdev->is_alive()) {
		rv = C_NO_RESPONSE;
	} else if (dp->is_still_used()) {
		pr_err1("Error: name=%s, still have %u[mapped-buffers], %u[ios]\n", bdev->b.info.name, dp->get_num_mem_reg_ranges(), dp->get_num_in_air_ios());
		return C_REMAINS_OPEN;
	} else if (bdev->conf.is_dummy()) {
		bdev->b.disconnect(id, do_suicide);
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_FS_FILE) {
		close(bdev->get_fd());
		const int remove_rv = remove(bdev->conf.conn.local_file_path);
		rv = (remove_rv == 0) ? C_OK : C_NO_RESPONSE;
		bdev->b.disconnect(id, do_suicide);
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DEV_BLK_KERNEL) {
		close(bdev->get_fd());
		bdev->b.disconnect(id, do_suicide);
	} else if (bdev->conf.is_bdev_remote()) {
		bdev->b.disconnect(id, do_suicide);
	} else {
		rv = C_NO_DEVICE;
	}
	pr_info1("Close " PRINT_BDEV_ID_FMT ", rv=%d\n", PRINT_BDEV_ID_ARGS(*bdev), rv);
	bdev->get_fd() = -1;
	return rv;
}

enum connect_rv global_clnt_context_imp::bdev_disconnect(const backend_bdev_id& id) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	return bdev->disconnect(false);
}

void global_clnt_context_imp::bdev_ctl_report_di(const backend_bdev_id& id, uint64_t offset_lba_bytes) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (bdev) {
		pr_err1("Error: User reported data corruption on " PRINT_BDEV_ID_FMT ", lba=0x%lx[B]\n", PRINT_BDEV_ID_ARGS(*bdev), offset_lba_bytes);
		t_lock_guard l(bdev->control_path_lock);
		bdev->disconnect(true);
	} else {
		pr_err1("Error: User reported data corruption on unknown " PRINT_BDEV_UUID_FMT ", lba=0x%lx[B]\n", id.uuid, offset_lba_bytes);
	}
}

uint32_t global_clnt_context_imp::bdev_ctl_get_n_in_air_ios(const backend_bdev_id& id) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (bdev) {
		t_lock_guard l(bdev->control_path_lock);
		if (bdev->is_alive()) {
			const uint32_t rv = bdev->b.dp->get_num_in_air_ios();
			pr_verb1(PRINT_BDEV_ID_FMT ".n_ios=%u\n", PRINT_BDEV_ID_ARGS(*bdev), rv);
			return rv;
		}
	}
	return 0;
}

enum connect_rv global_clnt_context_imp::bdev_get_info(const backend_bdev_id& id, bdev_info *ret_val) noexcept {
	server_bdev *bdev = bdevs.find_by(id);
	if (!bdev) return C_NO_DEVICE;
	if (!bdev->is_alive()) return C_NO_RESPONSE;
	t_lock_guard l(bdev->control_path_lock);
	*ret_val = bdev->b.info;
	return C_OK;
}

/********************************* global_clnt_context ********************************************/
global_clnt_context::global_clnt_context(const init_params& par) {
	auto& instance = global_clnt_context_imp::get();
	const int init_rv = instance.init(par, this->metadata_json_format);
	if (init_rv < 0)
		throw clnt_init_exception(init_rv, "Failed to initialize");
	else if (init_rv > 0)
		throw clnt_init_exception(init_rv, "Already initialized");
}

global_clnt_context::~global_clnt_context() noexcept {
	(void)global_clnt_context_imp::get().destroy();
}

const char *global_clnt_context::get_metadata_json(void) const noexcept {
	return global_clnt_context_imp::get().get_metadata_json();
}

enum connect_rv global_clnt_context::bdev_connect(const backend_bdev_id& id) const noexcept {
	return global_clnt_context_imp::get().bdev_connect(id);
}
enum connect_rv global_clnt_context::bdev_bufs_register(const backend_bdev_id& id, const mem_list& bufs) const noexcept {
	return global_clnt_context_imp::get().bdev_bufs_register(id, bufs);
}

enum connect_rv global_clnt_context::bdev_force_close(const backend_bdev_id& id, bool do_reconnect) const noexcept {
	return global_clnt_context_imp::get().bdev_stop_all_ios(id, do_reconnect);
}

enum connect_rv global_clnt_context::bdev_ctl_log_msg(const backend_bdev_id& id, const std::string &s) const noexcept {
	return global_clnt_context_imp::get().bdev_ctl_log_msg2(id, s);
}

enum connect_rv global_clnt_context::bdev_bufs_unregist(const backend_bdev_id& id, const mem_list& bufs) const noexcept{
	return global_clnt_context_imp::get().bdev_bufs_unregist(id, bufs);
}

enum connect_rv global_clnt_context::bdev_disconnect(const backend_bdev_id& id) const noexcept {
	return global_clnt_context_imp::get().bdev_disconnect(id);
}

enum connect_rv global_clnt_context::open__bufs_register(const backend_bdev_id& id, const mem_list& bufs) const noexcept {
	global_clnt_context_imp &c = global_clnt_context_imp::get();
	server_bdev *bdev = c.bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	if (!bdev->is_alive()) {						// Try to auto open bdev
		const enum connect_rv rv = c.bdev_connect(id);
		if (rv != C_OK)
			return rv;
	}
	return c.bdev_bufs_register(id, bufs);
}
enum connect_rv global_clnt_context::close_bufs_unregist(const backend_bdev_id& id, const mem_list& bufs, bool stop_server) const noexcept {
	global_clnt_context_imp &c = global_clnt_context_imp::get();
	server_bdev *bdev = c.bdevs.find_by(id);
	if (!bdev)
		return C_NO_DEVICE;
	t_lock_guard l(bdev->control_path_lock);
	if (!bufs.empty()) {
		const enum connect_rv rv = c.bdev_bufs_unregist(id, bufs);
		if (rv != C_OK)
			return rv;
	}
	if (!bdev->b.dp->is_still_used())
		bdev->disconnect(stop_server);		// User has nothing to do with close failure
	return C_OK;
}

enum connect_rv global_clnt_context::bdev_get_info(const backend_bdev_id& id, bdev_info &rv) const noexcept {
	return global_clnt_context_imp::get().bdev_get_info(id, &rv);
}

int32_t global_clnt_context::bdev_get_descriptor(const backend_bdev_id& id) const noexcept {
	bdev_info rv;
	if (global_clnt_context_imp::get().bdev_get_info(id, &rv) == connect_rv::C_OK)
		return rv.bdev_descriptor;
	return -1;
}

void global_clnt_context::bdev_ctl_report_data_corruption(const backend_bdev_id& id, uint64_t offset_lba_bytes) const noexcept {
	global_clnt_context_imp::get().bdev_ctl_report_di(id, offset_lba_bytes);
}

uint32_t global_clnt_context::bdev_ctl_get_num_in_air_ios(const backend_bdev_id& id) const noexcept {
	return global_clnt_context_imp::get().bdev_ctl_get_n_in_air_ios(id);
}

/*****************************************************************************/
/* IO api race conditions after io was submitted. Worst case - it has an async callback and it is polled:
	* Multiple user different threads poll IO status via get_error().
	* Multiple user different threads call try_cancel() due to timeout or any other reason.
	* When IO completes and gives callback to user. This callback can also execute get_error() or try_cancel()
	* Once try_cancel() succeeds, IO can be freed so callback must not arrive on canceled ios
	* With server backend - completion ring wakes up and calls client_receive_server_finish_io(), race with cancel

  Key points:
	* Once IO status becomes not in transfer (success / failure) user can immediately free it (possibly polling thread).
	* This implies that in the code we cannot refer to io after rv was set.
	* Inherent race condition: seting io rv and issuing callback, but user does polling in parallel to callback.
*/
void server_io_req::client_receive_server_finish_io(int64_t rv) {
	_exec->extern_notify_completion(rv);	// Here is a race condition with already canceled io, todo, fix me
}

void io_request_base::submit_io(void) noexcept {
	BUG_ON(out.rv == io_error_codes::E_IN_TRANSFER, "memory corruption: attempt to retry io[%p] before prev execution completed!", this);
	out.rv = io_error_codes::E_IN_TRANSFER;
	const server_bdev *bdev = NULL;
	BUG_ON(_exec != NULL, "BUG: IO is still running! wait for completion or cancel, before retrying it");
	if (params.get_bdev_descriptor() > 0)
		bdev = global_clnt_context_imp::get().bdevs.find_by_io(params.get_bdev_descriptor());
	if (unlikely(!bdev)) {
		pr_err1("Error: Invalid bdev descriptor of io=%d. Open bdev to obtain a valid descriptor\n", params.get_bdev_descriptor());
		io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
		return;
	}
	server_io_req *sio = (server_io_req*)this;
	sio->is_remote_set(bdev->conf.is_bdev_remote());
	if (bdev->conf.is_bdev_local()) {
		if (!params.is_safe_io()) {
			if (!bdev->b.dp->verify_io_param_valid(*sio)) {
				io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
				return;
			}
		}
		#if defined(HAS_URING_LIB)
			if (!params.has_callback() && params.may_use_uring())	// Uring does not support async callback mode
				_exec = new uring_request_executor(bdev->b.dp->in_air, *this);
		#endif
		if (!_exec) {
			if (!params.is_blocking_io()) {					// Async IO, with / without completion
				_exec = new aio_request_executor(bdev->b.dp->in_air, *this);
			} else {										// Blocking IO
				_exec = new sync_request_executor(bdev->b.dp->in_air, *this);
			}
		}
		_exec->run();									// Will auto delete exec upon IO finish;
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DUMMY_DEV_FAIL) {
		io_autofail_executor(*this, io_error_codes::E_PERM_FAIL_NO_RETRY);	// Here, injection of all possible errors
	} else if (bdev->conf.type == bdev_config_params::bdev_type::DUMMY_DEV_STUCK) {
		_exec = new never_reply_executor(bdev->b.dp->in_air, *this);
		_exec->run();
	} else if (bdev->conf.is_bdev_remote()) {
		const bool should_block = params.is_blocking_io();
		if (unlikely(should_block)) {
			_exec = new wrap_remote_io_exec_blocking(bdev->b.dp->in_air, *this);
		} else {
			_exec = new wrap_remote_io_exec_async(bdev->b.dp->in_air, *this);
		}
		if (_exec->run() < 0)
			return;
		bool need_wakeup_srvr;
		const int send_rv = bdev->b.dp->clnt_send_io(*this, &need_wakeup_srvr);
		if (send_rv >= 0) {
			pr_verb1(PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT PRINT_CLNT_IO_PTR_FMT ", doorbell=%d\n", PRINT_IO_REQ_ARGS(params), send_rv, this, need_wakeup_srvr); // This print is mem corrupt
			if (need_wakeup_srvr) {
				const int wakeup_rv = bdev->b.dp_wakeup_server();
				if (wakeup_rv < 0) // PRINT here is mem corrupt
					pr_err1(PRINT_IO_REQ_FMT PRINT_IO_SQE_ELEM_FMT PRINT_CLNT_IO_PTR_FMT ", error waiking up server, may stuck...\n", PRINT_IO_REQ_ARGS(params), send_rv, this);
			}
			// Callback will come in future
		} else { // Submition failed, Give callback
			((server_io_req *)this)->client_receive_server_finish_io(send_rv);
		}
		if (should_block)
			_exec->is_still_running();		// Blocking wait, for success or failure
	} else {
		io_autofail_executor(*this, io_error_codes::E_INVAL_PARAMS);
	}
}

io_request_executor_base* io_request_base::__disconnect_executor_atomic(void) noexcept {
	if (!_exec)
		return nullptr;
	uint64_t *ptr = (uint64_t *)&_exec;
	return (io_request_executor_base*)__atomic_exchange_n(ptr, (unsigned long)NULL, __ATOMIC_SEQ_CST);
}

enum io_error_codes io_request_base::get_error(void) noexcept {
	if (out.rv == io_error_codes::E_IN_TRANSFER) {
		DEBUG_ASSERT(!params.is_blocking_io());		// Impossible for blocking io as out.rv would be already set
		if (params.is_polling_mode()) {
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

io_request::~io_request() {				// Needed because user may not call get error nor cancel and executor will be stuck. So force call get error
	if (out.rv == io_error_codes::E_IN_TRANSFER) {
		pr_err1(PRINT_IO_REQ_FMT PRINT_CLNT_IO_PTR_FMT ", User wrong flow, Destroying io while it is still in air (running)\n", PRINT_IO_REQ_ARGS(params), this);
		if (params.is_polling_mode()) {
			const cancel_rv crv = try_cancel();
			pr_err1(PRINT_IO_REQ_FMT PRINT_CLNT_IO_PTR_FMT ", cancel_rv=%d\n", PRINT_IO_REQ_ARGS(params), this, crv);
		}
		BUG_ON(out.rv == io_error_codes::E_IN_TRANSFER, "Destroying io while it is still in air (running)!");
	}
	if (_exec) {
		pr_err1(PRINT_IO_REQ_FMT PRINT_CLNT_IO_PTR_FMT ", User wrong flow, Started IO and never checked that it finished (rv=%ld). Force cleanup\n", PRINT_IO_REQ_ARGS(params), this, out.rv);
		out.rv = io_error_codes::E_INVAL_PARAMS;
		(void)get_error();
	}
}

enum io_request::cancel_rv io_request_base::try_cancel(bool blocking_wait) noexcept {
	if (out.rv != io_error_codes::E_IN_TRANSFER)
		return io_request::cancel_rv::G_ALLREADY_DONE;
	DEBUG_ASSERT(!params.is_blocking_io());		// Impossible for blocking io as out.rv would be already set
	if (unlikely(blocking_wait)) {				// Wait for IO to finish, may stuck for a long time in this loop
		while (get_error() == gusli::io_error_codes::E_IN_TRANSFER) {
			std::this_thread::sleep_for(std::chrono::microseconds(10));
		}
		BUG_ON(_exec, "IO has was finished, It must not have a valid executor, rv=%ld", out.rv);
		return io_request::cancel_rv::G_ALLREADY_DONE;
	}
	auto* orig_exec = __disconnect_executor_atomic();		// IO finished / Canceled
	if (orig_exec) {		// Executor still running
		const enum cancel_rv crv = orig_exec->cancel();
		if (crv == cancel_rv::G_CANCELED) {
			out.rv = (int64_t)io_error_codes::E_CANCELED_BY_CALLER;
			return io_request::cancel_rv::G_CANCELED;
		} // Else: Already done during call to executor cancel
	}     // Else: Already done before call to executor cancel
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

int bdev_backend_api::create_dp(const backend_bdev_id &id, MGMT::msg_content &msg) {
	if (!info.is_valid())
		return -__LINE__;
	const size_t size = msg.build_reg_buf();
	auto *pr = &msg.pay.c_register_buf;
	// Initialize datapath of producer
	const uint64_t n_bytes = io_csring::n_needed_bytes(info.block_size);
	pr->build_scheduler(id, n_bytes / (uint64_t)info.block_size);
	if (has_remote()) {
		dp = new datapath_t<bdev_stats_clnt>(pr->name, global_clnt_context_imp::get().shm_io_bufs, info);
		*(uint64_t*)dp->shm_ring.get_buf() = MGMT::shm_cookie;		// Insert cookie for the server to verify
		const io_buffer_t buf = io_buffer_t::construct(dp->shm_ring.get_buf(), dp->shm_ring.get_n_bytes());
		pr_verb1(PRINT_REG_BUF_FMT ", to=%s\n", PRINT_REG_BUF_ARGS(pr, buf), srv_addr);
		ASSERT_IN_PRODUCTION(sem_init(&wait_control_path, 0, 0) == 0);
		if (send_to(msg, size) < 0) {
			delete dp; dp = nullptr;
			return -__LINE__;
		}
		check_incoming();
	} else {	// Same datapath but without server side
		dp = new datapath_t<bdev_stats_clnt>(nullptr, global_clnt_context_imp::get().shm_io_bufs, info);
	}
	return 0;
}

int bdev_backend_api::hand_shake(const bdev_config_params &conf, const char *clnt_name) {
	srv_addr = conf.conn.remot_sock_addr;
	const sock_t::type s_type = MGMT::get_com_type(srv_addr);
	MGMT::msg_content msg;
	int conn_rv;
	const bool blocking_connect = true; // (s_type != sock_t::type::S_UDP);
	pr_info1("Connecting to |%s|, blocking=%u\n", srv_addr, blocking_connect);
	if (     s_type == sock_t::type::S_UDP) conn_rv = sock.clnt_connect_to_srvr_udp(MGMT::COMM_PORT, &srv_addr[1], ca, blocking_connect);
	else if (s_type == sock_t::type::S_TCP) conn_rv = sock.clnt_connect_to_srvr_tcp(MGMT::COMM_PORT, &srv_addr[1], ca, blocking_connect);
	else if (s_type == sock_t::type::S_UDS) conn_rv = sock.clnt_connect_to_srvr_uds(                  srv_addr   , ca, blocking_connect);
	else {
		pr_err1("unsupported server address |%s|\n", srv_addr);
		info.bdev_descriptor = -__LINE__;
		return info.bdev_descriptor;
	}
	if (conn_rv < 0) {
		pr_err1("Cannot connenct to |%s|, rv=%d. is server up?\n", srv_addr, conn_rv);
		info.bdev_descriptor = -1;
		goto _out;
	}
	{
		const size_t size = msg.build_hello();
		auto *p = &msg.pay.c_hello;
		p->fill(conf.id, clnt_name, conf.security_cookie);
		if (send_to(msg, size) >= 0)
			check_incoming();
		else
			goto _out;
	}
	if (create_dp(conf.id, msg) < 0)
		goto _out;
	if (MGMT::set_large_io_buffers)
		sock.set_io_buffer_size(1<<19, 1<<19);
	info.bdev_descriptor = sock.fd();
	{
		const int err = pthread_create(&io_listener_tid, NULL, (void* (*)(void*))io_completions_listener, this);
		ASSERT_IN_PRODUCTION(err <= 0);
	}
_out:
	if (!info.is_valid())
		disconnect(conf.id);				// Cleanup open with close
	return info.bdev_descriptor;
}

int bdev_backend_api::map_buf(const io_buffer_t io) {
	if (!io.is_valid_for(info.block_size))
		return -__LINE__;
	MGMT::msg_content msg;
	const size_t size = msg.build_reg_buf();
	{
		t_lock_guard l(dp->shm_io_bufs->with_lock());
		const base_shm_element *g_map = dp->shm_io_bufs->insert_on_client(io);
		if (!g_map)
			return -__LINE__;			// Buffer rejected by global hash
		if (!dp->reg_bufs_set.add(g_map->buf_idx)) {
			pr_err1("%s: Wrong register buf request to " PRINT_IO_BUF_FMT ", it is already registered to this bdevs as " PRINT_REG_IDX_FMT "\n", info.name, PRINT_IO_BUF_ARGS(io), g_map->buf_idx);
			dp->shm_io_bufs->dec_ref(g_map, false);	// Abort, dec ref. Verify cannot free the global because its ref must be >= 2
			return -__LINE__;			// Already mapped to this block device
		}
		auto *pr = &msg.pay.c_register_buf;
		// Note: Dont touch *(uint64_t*)g_map->mem.get_buf() - It might be used now with io to a different bdev, possible on different server
		pr->build_io_buffer(*g_map, info.block_size);
		pr_info1(PRINT_REG_BUF_FMT ", name=%s, ref_cnt[%u]\n", PRINT_REG_BUF_ARGS(pr, io), pr->name, g_map->ref_count);
	}	// Release lock here
	if (has_remote()) {
		ASSERT_IN_PRODUCTION(sem_init(&wait_control_path, 0, 0) == 0);
		if (send_to(msg, size) < 0)
			return -__LINE__;
		ASSERT_IN_PRODUCTION(sem_wait(&wait_control_path) == 0);
	}
	return 0;
}

enum connect_rv bdev_backend_api::map_buf_do_vec(const std::vector<io_buffer_t>& bufs) {
	for (size_t i = 0; i < bufs.size(); i++) {
		const int map_rv = map_buf(bufs[i]);
		if (map_rv != 0)
			return C_WRONG_ARGUMENTS;
	}
	return C_OK;
}

int bdev_backend_api::map_buf_un(const io_buffer_t io) {
	MGMT::msg_content msg;
	const size_t size = msg.build_unr_buf();
	{
		t_lock_guard l(dp->shm_io_bufs->with_lock());
		const base_shm_element *g_map = dp->shm_io_bufs->find1(io);
		if (!g_map) {
			pr_err1("Wrong unregister buf request to " PRINT_IO_BUF_FMT ", it does not exist\n", PRINT_IO_BUF_ARGS(io));
			return -__LINE__;
		}
		if (!dp->reg_bufs_set.has(g_map->buf_idx)) {
			pr_err1("Wrong unregister buf request to " PRINT_IO_BUF_FMT ", it is registered to other bdevs, not to this one\n", PRINT_IO_BUF_ARGS(io));
			return -__LINE__;
		}
		auto *pr = &msg.pay.c_unreg_buf;
		pr->build_io_buffer(*g_map, info.block_size);
		pr_info1(PRINT_UNR_BUF_FMT ", name=%s, srvr_ptr=%p, ref_cnt[%u]--\n", PRINT_REG_BUF_ARGS(pr, io), pr->name, g_map->other_party_ptr, g_map->ref_count);
	}	// Release lock here
	if (has_remote()) {
		ASSERT_IN_PRODUCTION(sem_init(&wait_control_path, 0, 0) == 0);
		if (send_to(msg, size) < 0)
			return -__LINE__;
		ASSERT_IN_PRODUCTION(sem_wait(&wait_control_path) == 0);
	}
	{	// After server replied to us, remove the buffer
		t_lock_guard l(dp->shm_io_bufs->with_lock());
		const base_shm_element *g_map = dp->shm_io_bufs->find1(io);
		ASSERT_IN_PRODUCTION(g_map && dp->reg_bufs_set.del(g_map->buf_idx));
		dp->shm_io_bufs->dec_ref(g_map);
	}
	return 0;
}

enum connect_rv bdev_backend_api::map_buf_un_vec(const std::vector<io_buffer_t>& bufs) {
	for (int i = (int)bufs.size()-1; i >= 0; i--) {			// Assuming same vector as register - do reverse order to reduce vector moves
		const int map_rv = map_buf_un(bufs[i]);
		if (map_rv != 0)
			return C_WRONG_ARGUMENTS;
	}
	return C_OK;
}

int bdev_backend_api::disconnect(const backend_bdev_id& id, const bool do_kill_server) {
	if (io_listener_tid) {
		int send_rv = 0;
		if (sock.is_alive()) {
			MGMT::msg_content msg;
			size_t size;
			if (do_kill_server) {
				size = msg.build_die();
			} else {
				size = msg.build_close();
				msg.pay.c_close.volume = id;
			}
			send_rv = send_to(msg, size);
			if (send_rv < 0)
				pr_err1(PRINT_BDEV_UUID_FMT " Unable to disconnect from server %s, rv=%d\n",  id.uuid, srv_addr, send_rv);
		}
		pr_info1("going to join listener_thread tid=0x%lx, sock_alive=%d\n", (long)io_listener_tid, sock.is_alive());
		const int err = pthread_join(io_listener_tid, NULL);
		ASSERT_IN_PRODUCTION(err == 0);
	}
	delete dp; dp = nullptr;
	clean_srvr();
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
			ASSERT_IN_PRODUCTION(info.is_valid());
		} else if (msg.is(MGMT::msg::register_ack)) {
			const auto *pr = &msg.pay.s_register_ack;
			if (pr->is_io_buf) {
				t_lock_guard l(dp->shm_io_bufs->with_lock());
				BUG_ON(!dp->shm_io_bufs->find2(pr->buf_idx), "Server gave ack on unknown registered memory buf_idx=%u, srvr_ptr=0x%lx\n", pr->buf_idx, pr->server_pointer);
			}
			pr_info1("RegisterAck[%d%c] name=%s, srvr_ptr=0x%lx, rv=%d\n", pr->get_buf_idx(), pr->get_buf_type(), pr->name, pr->server_pointer, rv);
			BUG_ON(pr->rv != 0, "rv=%d", pr->rv);
			ASSERT_IN_PRODUCTION(sem_post(&wait_control_path) == 0);	// Unlock caller which waits for buffer registration
		} else if (msg.is(MGMT::msg::unreg_ack)) {
			const auto *pr = &msg.pay.s_unreg_ack;
			pr_info1("UnRegistAck[%d%c] name=%s, srvr_ptr=0x%lx, rv=%d\n", pr->get_buf_idx(), pr->get_buf_type(), pr->name, pr->server_pointer, rv);
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
			// Drain all awaiting completions
			bool need_wakeup_srvr = false, wakup_on_msg;
			int idx = dp->clnt_receive_completion(&wakup_on_msg);
			for (; idx >= 0; idx = dp->clnt_receive_completion(&wakup_on_msg)) {
				need_wakeup_srvr |= wakup_on_msg;
				pr_verb1(PRINT_IO_CQE_ELEM_FMT " processed completion. Need_wakeup=%d\n", idx, wakup_on_msg);
			}
			if (need_wakeup_srvr) { // We dont send more than ring size ios so server never runs out of completion entries
				// Find bdev by message address and call bdev->b.dp_wakeup_server()); // because client is ready to accept new completions
				pr_err1("%s: completion list full (throttling not implemented). IO may stuck. Probably user app sent too much IO's in air (more than allowed)...\n", server_path);
			}
		} else {
			BUG_ON(true, "Unhandled message %s\n", msg.hdr.type);
		}
		return true;	// Need to respond to control path change
	}
	return rv;
}

int bdev_backend_api::dp_wakeup_server(void) const {
	MGMT::msg_content msg;
	const size_t size = msg.build_dp_subm();
	auto *p = &msg.pay.dp_submit;
	p->sender_added_new_work = true;
	p->sender_ready_for_work = false;
	dp->stats.n_doorbels_wakeup_srvr++;
	return send_to(msg, size);
}

void* bdev_backend_api::io_completions_listener(bdev_backend_api *bdev) {
	{	// Rename thread
		char old_name[32], new_name[32];	// Linux allows thread names of at most 16[b]
		const int srvr_prefix_len = (int)strncpy_no_trunc_warning(new_name, bdev->info.name, 12);
		strncpy_no_trunc_warning(&new_name[srvr_prefix_len], "cIOl", 5);
		pthread_getname_np(pthread_self(), old_name, sizeof(old_name));
		pr_info1("\t\t\tListener started, renaming %s->%s for %s\n", old_name, new_name, bdev->info.name);
		const int rename_rv = pthread_setname_np(pthread_self(), new_name);
		if (rename_rv != 0)
			pr_err1("rename failed rv=%d " PRINT_EXTERN_ERR_FMT "\n", rename_rv, PRINT_EXTERN_ERR_ARGS);
	}
	for (bdev->is_control_path_ok = true; bdev->is_control_path_ok; ) {
		bdev->check_incoming();
	}
	pr_info1("\t\t\tListener name=%s addr=%s end\n", bdev->info.name, bdev->srv_addr);
	return NULL;
}

}	// namespace
