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
#include <pthread.h>	// pthread_self()
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
	gusli::bdev_info open1(const char* who) override {
		binfo.bdev_descriptor = open(binfo.name, O_RDWR | O_CREAT | O_LARGEFILE, (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
		my_assert(binfo.bdev_descriptor > 0);
		binfo.block_size = 4096;			// 4[KB]
		binfo.num_total_blocks = (1 << 20); // 4[GB]
		binfo.num_max_inflight_io = MAX_SERVER_IN_FLIGHT_IO;
		dslog("open: fd=%d, remote client=%s\n", binfo.bdev_descriptor, who);
		my_assert(strcmp(who, UNITEST_CLNT_NAME) == 0);
		return binfo;
	}
	int close1(const char* who) override {
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
	void exec_io(class gusli::server_io_req& io) override {
		my_assert(io.has_callback());			// Do not support io without callback for now
		io.start_execution();
		if (io.params.op == gusli::io_type::G_WRITE) {
			io.set_error(gusli::E_BACKEND_FAULT);
		} else if (io.params.num_ranges() <= 1) {
			test_lba::map1_fill(io.params.map, binfo.block_size);
		} else {
			const gusli::io_multi_map_t* mio = io.get_multi_map();
			dslog("Serving IO: #rng = %u, buf_size=%lu[b]\n", io.params.num_ranges(), io.params.buf_size());
			test_lba::mmio_print(mio, par.server_name);
			test_lba::mmio_fill( mio, binfo.block_size);
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
		snprintf(binfo.name, sizeof(binfo.name), "%s%s", gusli::global_clnt_context::thread_names_prefix, _name);
		const int rename_rv = pthread_setname_np(pthread_self(), binfo.name);	// For debug, set its thread to block device name
		my_assert(rename_rv == 0);
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
	#undef dslog
};
