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
#include "../common.hpp"

/* Example of a backend io execution server which fails io or never returns completions */
class dummy_server_ram : protected gusli::srvr_backend_bdev_api {
 protected:
	#define dslog(fmt, ...) ({ _unitest_log_fn("\x1b[16;34m%s: " fmt "\x1b[0;0m", binfo.name, ##__VA_ARGS__); })
	gusli::bdev_info binfo;
	gusli::bdev_info open1(const char* who) noexcept override { dslog("Open1 : %s\n", who); return binfo; }
	int             close1(const char* who) noexcept override { dslog("Close1: %s\n", who); return 0; }
 public:
	dummy_server_ram(const char* srvr_name, const char* bdev_name, const char* listen_addr) {
		strncpy(par.listen_address, listen_addr, sizeof(par.listen_address)-1);
		par.server_name = srvr_name;
		binfo.clear();
		my_assert(set_thread_name(binfo.name, bdev_name) == 0); // For debug, set its thread to block device name
		binfo.bdev_descriptor = 202020202;
		binfo.block_size = UNITEST_SERVER_BLOCK_SIZE;
		binfo.num_total_blocks = (1 << 20); // 4[GB]
		binfo.num_max_inflight_io = 4;		// Very small amount. Only 4 ios in air
		dslog("metadata=|%s|\n", create_and_get_metadata_json());
	}
	void run(void) { my_assert(gusli::srvr_backend_bdev_api::run() > 0); }	// Successful exit
	virtual ~dummy_server_ram() = default;
};

class fail_server_ram : public dummy_server_ram {
	bool should_stuck;
	void exec_io(gusli::backend_io_req& io) noexcept override {
		dslog("%s IO[%c].ptr=%p\n", par.server_name, io.params.op(), &io);
		if (!should_stuck)
			io.set_error(gusli::io_error_codes::E_PERM_FAIL_NO_RETRY);
	}
 public:
	fail_server_ram(const char* _name, const char* listen_addr, bool all_ios_stuck = false) :
		dummy_server_ram((all_ios_stuck ? "SrvStuck" : "SrvFail_"), _name, listen_addr) {
		should_stuck = all_ios_stuck;
	}
};

/*****************************************************************************/
class zero_server_ram : public dummy_server_ram {
	void exec_io(gusli::backend_io_req& io) noexcept override {
		if (io.params.op() == gusli::G_READ) {
			if (io.params.num_ranges() <= 1) {
				const gusli::io_map_t &map = io.params.map();
				memset(map.data.ptr, 0, map.data.byte_len);
			} else {
				const gusli::io_multi_map_t* mio = io.get_multi_map();
				for (uint32_t i = 0; i < mio->n_entries; i++) {
					const gusli::io_map_t &map = mio->entries[i];
					memset(map.data.ptr, 0, map.data.byte_len);
				}
			}
		}
		io.set_success(io.params.buf_size());
	}
 public:
	zero_server_ram(const char* _name, const char* listen_addr) :
		dummy_server_ram("SrvZero", _name, listen_addr) {}
	#undef dslog
};
