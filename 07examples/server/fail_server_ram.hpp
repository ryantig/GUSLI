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
class fail_server_ram : private gusli::srvr_backend_bdev_api {
	#define dslog(fmt, ...) ({ _unitest_log_fn("\x1b[16;34m%s: " fmt "\x1b[0;0m", binfo.name, ##__VA_ARGS__); })
	gusli::bdev_info binfo;
	bool should_stuck;						// Never return completion on io?
	gusli::bdev_info open1(const char* who) noexcept override { (void)who; return binfo; }
	int             close1(const char* who) noexcept override { (void)who; return 0; }
	void exec_io(gusli::backend_io_req& io) noexcept override {
		if (should_stuck) {
			dslog("Stuck IO[%c].ptr=%p\n", io.params.op(), &io);
		} else {
			dslog("Fail_ IO[%c].ptr=%p\n", io.params.op(), &io);
			io.set_error(gusli::io_error_codes::E_PERM_FAIL_NO_RETRY);
		}
	}
 public:
	fail_server_ram(const char* _name, const char* listen_addr, bool all_ios_stuck = false) {
		strncpy(par.listen_address, listen_addr, sizeof(par.listen_address)-1);
		par.server_name = (all_ios_stuck ? "SrvStuck" : "SrvFail_");
		should_stuck = all_ios_stuck;
		binfo.clear();
		my_assert(set_thread_name(binfo.name, _name) == 0); // For debug, set its thread to block device name
		binfo.bdev_descriptor = 202020202;
		binfo.block_size = UNITEST_SERVER_BLOCK_SIZE;
		binfo.num_total_blocks = (1 << 20); // 4[GB]
		binfo.num_max_inflight_io = 4;		// Very small amount. Only 4 ios in air
		dslog("metadata=|%s|\n", create_and_get_metadata_json());
	}
	void run(void) { my_assert(gusli::srvr_backend_bdev_api::run() > 0); }	// Successful exit
	virtual ~fail_server_ram() = default;
	#undef dslog
};
