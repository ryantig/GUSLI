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
#include <unistd.h>		// close()
#include <fcntl.h>		// open()
#include "gusli_server_api.hpp"
#include "../common.hpp"

/* Example of a backend io execution server which is
	1. read-only (does not support writes)
	2. Backed up by an empty file, in local dir of the executable
	3. First 8 bytes of each block store the address of that block
		so unit-tests can verify the correctness of reads
	4. Completes io's immediately (syncronously)
*/
class server_ro_lba : private gusli::srvr_backend_bdev_api {
	#define dslog(fmt, ...) ({ _unitest_log_fn("\x1b[16;34m%s: " fmt "\x1b[0;0m", binfo.name, ##__VA_ARGS__); })
	gusli::bdev_info binfo;
	gusli::bdev_info open1(const char* who) noexcept override {
		binfo.bdev_descriptor = open(binfo.name, O_RDWR | O_CREAT | O_LARGEFILE, (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
		my_assert(binfo.bdev_descriptor > 0);
		binfo.block_size = UNITEST_SERVER_BLOCK_SIZE;
		binfo.num_total_blocks = (1 << 20); // 4[GB]
		binfo.num_max_inflight_io = MAX_SERVER_IN_FLIGHT_IO;
		dslog("open: fd=%d, remote client=%s\n", binfo.bdev_descriptor, who);
		my_assert(strcmp(who, UNITEST_CLNT_NAME) == 0);
		return binfo;
	}
	int close1(const char* who) noexcept override {
		const int prev_fd = binfo.bdev_descriptor;
		if (binfo.is_valid()) {
			close(binfo.bdev_descriptor);
			binfo.bdev_descriptor = 0;
		}
		const int rv = remove(binfo.name);
		my_assert(rv >= 0);
		dslog("close: fd=%d, rv=%d, who=%s\n", prev_fd, rv, who);
		return 0;
	}
	void exec_io(gusli::server_io_req& io) noexcept override {
		my_assert(io.params.has_callback());			// Do not support io without callback for now
		io.start_execution();
		/*if (io.params.op() == gusli::io_type::G_WRITE) {	// Do not fail writes, just verify their content is correct
			io.set_error(gusli::E_BACKEND_FAULT);
		} else */ if (io.params.num_ranges() == 1) {
			const gusli::io_map_t &map = io.params.map();
			// dslog("Serving IO[%c]: 1rng ", io.params.op());
			// test_lba::map1_print(map, par.server_name);
			if (io.params.op() == gusli::io_type::G_WRITE)
				test_lba::map1_verify(map, binfo.block_size);
			else
				test_lba::map1_fill(  map, binfo.block_size);
		} else {
			const gusli::io_multi_map_t* mio = io.get_multi_map();
			dslog("Serving IO[%c]: #rng=%u, buf_size=%lu[b]\n", io.params.op(), io.params.num_ranges(), io.params.buf_size());
			test_lba::mmio_print(mio, par.server_name);
			if (io.params.op() == gusli::io_type::G_WRITE)
				test_lba::mmio_verify(mio, binfo.block_size);
			else
				test_lba::mmio_fill(  mio, binfo.block_size);
		}
		io.set_success(io.params.buf_size());
	}
 public:
	server_ro_lba(const char* _name, const char* listen_addr, bool use_extenral_loop = false) {
		my_assert(BREAKING_VERSION == 1);
		strncpy(par.listen_address, listen_addr, sizeof(par.listen_address)-1);
		par.log = stderr;
		par.server_name = (use_extenral_loop ? "RoSrvEL" : "RoSrv");
		par.has_external_polling_loop = use_extenral_loop;
		binfo.clear();
		my_assert(set_thread_name(binfo.name, _name) == 0); // For debug, set its thread to block device name
		dslog("metadata=|%s|\n", create_and_get_metadata_json());
	}
	void run(void) {
		int run_rv;
		if (par.has_external_polling_loop) {
			for (run_rv = 0; run_rv == 0; run_rv = gusli::srvr_backend_bdev_api::run()) {
				dslog("User is doing interesting stuff here, throttle how much cpu server uses\n");
			}
		} else {
			run_rv = gusli::srvr_backend_bdev_api::run();
		}
		my_assert(run_rv > 0);		// Successful exit
	}
	virtual ~server_ro_lba() = default;	// Just avoid clang warning of missing default virtual destructor
	#undef dslog
};
