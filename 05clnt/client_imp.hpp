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
enum bdev_type {							// Fast path to supported bdevs
	DUMMY_DEV_INVAL = 0x0,					// Invalid type
	DUMMY_DEV_FAIL =  'X',					// Dummy device which always fails io. For integration testing of bad-path
	FS_FILE =	      'f',					// 1 File or a directory of files storing the data
	KERNEL_BDEV =     'K',					// Backwards compatibility to /dev/... kernel implemented block devices including NVMe drives, /dev/zero, etc
	LINUX_UBLK =      'U',					// Backwards compatibility to Linux user space block device
	NVMESH_UM =       'N',					// Network block device (gusli client communicates wih bdev via server)
	//OTHER =           '?',
};

struct bdev_config {
	enum bdev_type type;
	enum connect_how { SHARED_RW = 'W', READ_ONLY = 'R', EXCLUSIVE_RW = 'X'} how;
	bool is_direct_io;
	union connection_type {
		char local_bdev_path[32];					//	Kernel block device /dev/....
		char local_file_path[32];					//	Local file path like /tmp/my_file.txt
		char remot_sock_addr[32];					//	Remote (server) uds/udp/tcp socket
	} conn;
	bdev_config() { memset(this, 0, sizeof(*this)); }
	void init_dev_fail(void) { type = DUMMY_DEV_FAIL; how = EXCLUSIVE_RW; strcpy(conn.local_file_path, ""); }
	void init_fs_file (const char *file_name) { type = FS_FILE; how = EXCLUSIVE_RW; strncpy(conn.local_file_path, file_name, sizeof(conn.local_file_path)-1); }
	void init_kernel_bdev(const char *path  ) { type = KERNEL_BDEV; how = EXCLUSIVE_RW; strncpy(conn.local_bdev_path, path, sizeof(conn.local_bdev_path)-1); }
};

class bdev_no_backend_api {									// When there is no server, and client emulates it
 public:
	uint32_t n_mapped_bufs;
	bdev_no_backend_api() : n_mapped_bufs(0) { }
};

struct bdev_stats_clnt {
	uint64_t n_doorbels_wakeup_srvr;
	void clear(void) { memset(this, 0, sizeof(*this)); }
	bdev_stats_clnt() { clear(); }
	int print_stats(char* buf, int buf_len) {
		return scnprintf(buf, buf_len, "d={%lu}", n_doorbels_wakeup_srvr);
	}
};

class bdev_backend_api {									// API to server 1 block device
	static constexpr const uint32_t minimal_delay = 100;	// Minimal rate of control path changes propagations X[millisec],
	sock_t sock;											// Socket through which clnt talks to server
	connect_addr ca;										// Connected server address
	time_t last_keepalive;
	const char* srv_addr;
	struct bdev_stats_clnt stats;

	bool is_control_path_ok;								// State of control path
	pthread_t io_listener_tid;
	sem_t wait_control_path;
	bool check_incoming();
	int  send_to(MGMT::msg_content &msg, size_t n_bytes) const __attribute__((warn_unused_result));
	void on_keep_alive_received(void) { time(&last_keepalive); }
 public:
	char security_cookie[16];								// Used for handshake with server, to verify library is authorized to access this bdev
	class datapath_t dp;
	class bdev_no_backend_api f;							// Todo: Properly union with dp and other fields
	bdev_info info;											// block device information visible for user
	bdev_backend_api() { io_listener_tid = 0; info.clear(); memset(security_cookie, 0, 16); }
	int hand_shake(const backend_bdev_id& id, const char* _ip, const char *clnt_name);
	int map_buf(   const backend_bdev_id& id, const io_buffer_t buf);
	int map_buf_un(const backend_bdev_id& id, const io_buffer_t buf);
	int close(     const backend_bdev_id& id, const bool do_kill_server = false);
	int dp_wakeup_server(void);
	static void* io_completions_listener(bdev_backend_api *_self);
	uint32_t suggested_control_past_rate(void) const { return minimal_delay * (is_control_path_ok ? 10 : 1); } // Slower ping to DS in steady state of good path, faster response to disaster recovery
};

struct server_bdev {					// Reflection of server (how to communicate with it)
	struct backend_bdev_id id;			// Key to find server by it
	struct bdev_config conf;			// Config of how to connect to this block device
	bdev_backend_api b;					// Remote connection
	t_lock_mutex_recursive control_path_lock;
	server_bdev() { control_path_lock.init(); }
	int get_fd(void) const { return b.info.bdev_descriptor; }
	int& get_fd(void) { return b.info.bdev_descriptor; }
	bool is_alive(void) const { return ((conf.type != DUMMY_DEV_INVAL) && (get_fd() > 0)); }
	uint32_t get_num_uses(void) const;
	bool is_still_used(void) const { return get_num_uses() != 0; }
};

struct bdevs_hash { 					// Hash table of connected servers
	static constexpr int N_MAX_BDEVS = 8;
	server_bdev arr[N_MAX_BDEVS];
	int n_devices = 0;
	bdevs_hash() { }
	server_bdev *find_by(int fd) const {
		for (int i = 0; i < N_MAX_BDEVS; i++ ) {
			if (fd == arr[i].get_fd())
				return (server_bdev *)&arr[i];
		}
		return NULL;
	}
	server_bdev *find_by(const struct backend_bdev_id& id) const {
		for (int i = 0; i < N_MAX_BDEVS; i++ ) {
			if (id == arr[i].id)
				return (server_bdev *)&arr[i];
		}
		return NULL;
	}
	bool has_any_bdev_open(void) const {
		for (int i = 0; i < N_MAX_BDEVS; i++ )
			if (arr[i].is_alive()) {
				const auto* bdev = &arr[i];
				pr_info1("Still open: bdev uuid=%.16s, type=%c, path=%s\n", bdev->id.uuid, bdev->conf.type, bdev->conf.conn.local_bdev_path);
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
		int version = 0;
		for (char *p = buf, *line_end; (p < buf_end); p = line_end + 1, line_no++ ) {
			line_end = strchr(p, '\n');
			if (line_end)
				*line_end = 0;						// Split lines
			else
				line_end = buf_end;					// Last line in a file
			if (line_no == 0) {						// Parse the config version
				sscanf(p, "# version=%d", &version);
			}
			if (should_skip_comment(p))
				continue;
			int argc = 0, n_args_read = 0;
			char *argv[8], c;						// Split line into arguments, separated by invisible characters (' ', '\t', ...)
			for (; argc < 8; *p++ = 0) {
				while (char_is_space(*p)) p++;		// Skip empty characters between arguments
				if (*p == 0) break;
				argv[argc++] = p;
				while (char_is_visible(*p)) p++;	// Skip the argument itself
				if (*p == 0) break;
			}
			server_bdev *b = &arr[n_devices];
			if (version == 1) {
				n_args_read += sscanf(argv[0], "%s", b->id.set_invalid()->uuid);
				n_args_read += sscanf(argv[1], "%c", &c); b->conf.type = (enum bdev_type)c;
				n_args_read += sscanf(argv[2], "%c", &c); b->conf.how = (bdev_config::connect_how)c;
				n_args_read += sscanf(argv[3], "%c", &c); b->conf.is_direct_io = (bool)((c == 'D')||(c == 'd'));
				n_args_read += sscanf(argv[4], "%s", (char*)&b->conf.conn);
				n_args_read += sscanf(argv[5], "%s", (char*)&b->b.security_cookie);
				// pr_verb1("+Dev: uuid=%s|%c|%c|%u|con=%s\n", b->id.uuid, b->conf.type,  b->conf.how,  b->conf.is_direct_io,  (char*)&b->conf.conn);
				if (n_args_read != argc)
					return -3;
			}
			n_devices++;
		}
		return 0;
	}
	void clear(void) { n_devices = 0; }
};

class global_clnt_context_imp : public global_clnt_context, public base_library {
 public:
	friend class global_clnt_context;
	struct init_params par;
	bdevs_hash bdevs;
	global_clnt_context_imp() : base_library(LIB_NAME) {
		pr_info1("clnt_ctx[%p] - construct %lu[b]\n", this, sizeof(*this));
	}
	~global_clnt_context_imp() {
		pr_info1("clnt_ctx[%p] - destruct\n", this);
	}
	enum connect_rv bdev_connect(void);
	int server_disconenct(void);
	void on_event_server_down(void);		// Start accumulating IO's / Possibly failing with time out. Server is inaccessible due to being hot upgraded / missing nvme disk / etc.
	void on_event_server_up(void);
	int parse_conf(void);
};

} // namespace gusli
