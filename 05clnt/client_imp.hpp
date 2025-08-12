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
#include "utils.hpp"
#include "gusli_client_api.hpp"
#define LIB_NAME "GUSLIc"
#define LIB_COLOR NV_COL_NONE
#include "03backend/server_clnt_api.hpp"
#include <semaphore.h>

namespace gusli {

/*****************************************************************************/
class bdev_stats_clnt {
	void clear(void) { memset(this, 0, sizeof(*this)); }
	int print_stats(char* buf, int buf_len) {
		return scnprintf(buf, buf_len, "d={%lu}", n_doorbels_wakeup_srvr);
	}
 public:
	uint64_t n_doorbels_wakeup_srvr;
	bdev_stats_clnt() { clear(); }
	~bdev_stats_clnt() {
		char str[256];
		print_stats(str, sizeof(str));
		pr_info1("stats{%s}\n", str);
	}
};

class bdev_backend_api {									// API to server 1 block device
	// Communication with server
	sock_t sock;											// Socket through which clnt talks to server
	connect_addr ca;										// Connected server address
	time_t last_keepalive;
	const char* srv_addr;
	bool is_control_path_ok;								// State of control path
	pthread_t io_listener_tid;
	sem_t wait_control_path;
	void clean_srvr(void) { sock.nice_close(); srv_addr = nullptr; is_control_path_ok = false; io_listener_tid = 0; info.clear(); }
	bool has_remote(void) const { return srv_addr != NULL; }
	bool check_incoming();
	int  send_to(MGMT::msg_content &msg, size_t n_bytes) const __attribute__((warn_unused_result));
	void on_keep_alive_received(void) { time(&last_keepalive); }
 public:
	datapath_t<bdev_stats_clnt> *dp = nullptr;
	bdev_info info;											// block device information visible for user
	bdev_backend_api() { clean_srvr(); }
	int hand_shake(const bdev_config_params &conf, const char *clnt_name);	// Constructor: Connect to remote server
	int create_dp( const backend_bdev_id &id, MGMT::msg_content &msg);		// Constructor: Connect to local bdev (serverless)
	int map_buf(   const backend_bdev_id& id, const io_buffer_t buf);
	int map_buf_un(const backend_bdev_id& id, const io_buffer_t buf);
	int disconnect(const backend_bdev_id& id, const bool do_kill_server = false);	// Destructor
	int dp_wakeup_server(void) const;
	static void* io_completions_listener(bdev_backend_api *_self);
};

struct server_bdev {					// Reflection of server (how to communicate with it)
	bdev_config_params conf;			// Config of how to connect to this block device
	bdev_backend_api b;					// Remote connection
	t_lock_mutex_recursive control_path_lock;
	server_bdev() { control_path_lock.init(); }
	int get_fd(void) const { return b.info.bdev_descriptor; }
	int& get_fd(void) { return b.info.bdev_descriptor; }
	bool is_alive(void) const {
		const bool rv = ((conf.type != conf.DUMMY_DEV_INVAL) && (get_fd() > 0));
		if (rv)
			ASSERT_IN_PRODUCTION(b.dp != nullptr);
		return rv;
	}
};

class bdevs_hash { 					// Hash table of connected servers
	std::vector<struct server_bdev> arr;
 public:
	const server_bdev *find_by_io(int fd) const {		// Datapath: find by io
		for (const server_bdev& i : arr) {
			if (fd == i.get_fd())
				return &i;
		}
		return NULL;
	}
	server_bdev *find_by(const backend_bdev_id& id) {	// Control path, non const because need to lock bdev
		for (server_bdev& i : arr) {
			if (id == i.conf.id)
				return &i;
		}
		return NULL;
	}
	bool has_any_bdev_open(void) const {
		for (const server_bdev& i : arr)
			if (i.is_alive()) {
				pr_info1("Still open: bdev " PRINT_BDEV_ID_FMT "\n", PRINT_BDEV_ID_ARGS(i));
				return true;
			}
		return false;
	}
	bool should_skip_comment(char* &p) const {
		while (char_is_space(*p)) p++;					// Skip empty characters at line start
		if ((p[0] == 0) || (p[0] == '#')) 				// Empty line or single line comment
			return true;
		char *eol_comment = strchr(p, '#'); 			// Skip optional end of line comment
		if (eol_comment)
			*eol_comment = 0;
		return false;
	}

	int parse_conf(char *buf, char* buf_end) {
		int line_no = 0;
		int version = 0;							// Uninitialized
		for (char *p = buf, *line_end; (p < buf_end); p = line_end + 1, line_no++ ) {
			line_end = strchr(p, '\n');
			if (line_end)
				*line_end = 0;						// Split lines
			else
				line_end = buf_end;					// Last line in a file
			if (should_skip_comment(p))
				continue;
			if (version == 0) {						// Parse the config version
				while (char_is_space(*p)) p++;		// Skip empty prefix
				const int n_args_read = sscanf(p, "version=%d", &version);
				if (n_args_read != 1) return -__LINE__;	// Missing version
				continue;
			}
			int argc = 0;
			char *argv[8];							// Split line into arguments, separated by invisible characters (' ', '\t', ...)
			for (; argc < 8; *p++ = 0) {
				while (char_is_space(*p)) p++;		// Skip empty characters between arguments
				if (*p == 0) break;
				argv[argc++] = p;
				while (char_is_visible(*p)) p++;	// Skip the argument itself
				if (*p == 0) break;
			}
			const int bdev_parse_rv = arr.emplace_back().conf.init_parse(version, argv, argc);
			if (bdev_parse_rv != 0)
				return bdev_parse_rv;
		}
		return 0;
	}
	int n_devices(void) const { return (int)arr.size(); }
	void clear(void) { arr.clear(); }
};

class global_clnt_context_imp : no_implicit_constructors, public base_library { // Singletone: Library context
	global_clnt_context::init_params par;
	global_clnt_context_imp();
	~global_clnt_context_imp();
	void on_event_server_down(void);		// Start accumulating IO's / Possibly failing with time out. Server is inaccessible due to being hot upgraded / missing nvme disk / etc.
	void on_event_server_up(void);
	int parse_conf(void);
 public:
	bdevs_hash bdevs;
	shm_io_bufs_global_t *shm_io_bufs;

	static global_clnt_context_imp& get(void) noexcept;				// Get singletone
	int init(const global_clnt_context::init_params& _par, const char* metadata_json_format) noexcept;
	const char *get_metadata_json(void) const noexcept { return lib_info_json; }
	int destroy(void) noexcept;

	enum connect_rv bdev_connect(      const backend_bdev_id&) noexcept;
	enum connect_rv bdev_bufs_register(const backend_bdev_id&, const std::vector<io_buffer_t>& bufs) noexcept;
	enum connect_rv bdev_bufs_unregist(const backend_bdev_id&, const std::vector<io_buffer_t>& bufs) noexcept;
	enum connect_rv bdev_disconnect(   const backend_bdev_id&) noexcept;
	enum connect_rv bdev_get_info(     const backend_bdev_id&, bdev_info *ret_val) noexcept;
	void bdev_ctl_report_di(           const backend_bdev_id&, uint64_t offset_lba_bytes) noexcept;
};

} // namespace gusli
