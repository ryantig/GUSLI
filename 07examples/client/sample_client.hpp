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
#include "07examples/client/io_submittion_example.hpp"

/*****************************************************************************/
/* Simple test of clinet process: Connect to server, Write IO and verify it
   with read. Do 1 range and multi-range io. Stop the server at the end */
int client_simple_test_of_server(const char* clnt_name, const int n_devs, const char* const bdev_uuid[], const char* const srvr_addr[]) {
	gusli::global_clnt_context::init_params p;
	p.client_name = clnt_name;
	p.max_num_simultaneous_requests = MAX_SERVER_IN_FLIGHT_IO;
	char conf[512];
	{	// Generate config
		int i = sprintf(conf,
			"# version=1, Config file for gusli client lib\n"
			"# bdevs: UUID-16b, type, attach_op, direct, path, security_cookie\n");
		for (int b = 0; b < n_devs; b++)
			i += sprintf(&conf[i], "%s N W D %s sec=0x04\n", bdev_uuid[b], srvr_addr[b]);
		p.config_file = &conf[0];
	}
	gusli::global_clnt_raii gc(p);
	log_unitest("Client metadata= %s\n", gc.get_metadata_json());
	// Create io buffers
	unitest_io my_io;
	std::vector<gusli::io_buffer_t> io_bufs;
	io_bufs.emplace_back(my_io.get_map());

	// Map io buffers for read/write operations and auto open block devices
	for (int b = 0; b < n_devs; b++) {
		log_line("Remote server %s, connecting...", srvr_addr[b]);
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		my_assert(gc.bufs_register(bdev, io_bufs) == gusli::connect_rv::C_OK);
		gusli::bdev_info info;
		my_assert(gc.get_bdev_info(bdev, info) == gusli::connect_rv::C_OK);
		my_assert(info.num_total_blocks > 0x100);			// We write to first few blocks
		log_line("Remote server %s oppened and mapped bufs", info.name);
	}

	for (int b = 0; b < n_devs; b++) {
		log_line("%s: Write->Read test of 1[blk]", srvr_addr[b]);
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		gusli::bdev_info info;
		my_assert(gc.get_bdev_info(bdev, info) == gusli::connect_rv::C_OK);
		static constexpr const char *data = "Hello world";
		const uint64_t lba = b * info.block_size;
		my_io.io.params.init_1_rng(gusli::G_NOP, info.bdev_descriptor, lba, info.block_size, my_io.io_buf);
		strcpy(my_io.io_buf, data);
		my_io.exec(gusli::G_WRITE, io_exec_mode::ASYNC_CB);
		my_io.clean_buf();
		my_io.exec(gusli::G_READ, io_exec_mode::ASYNC_CB);
		my_assert(strcmp(data, my_io.io_buf) == 0);
	}
	for (int b = 0; b < n_devs; b++) {
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		gusli::bdev_info info;
		my_assert(gc.get_bdev_info(bdev, info) == gusli::connect_rv::C_OK);

		const gusli::io_buffer_t& map = io_bufs[0];
		static constexpr const int n_blocks = 6;
		my_assert(map.byte_len >= info.block_size * (n_blocks + 1));
		#define n_block(i) (info.block_size * (i))
		#define mappend_block(i) ((void*)((uint64_t)map.ptr + n_block(i)))
		const uint64_t lbas[3] = {n_block(0x0B), n_block(0x11), n_block(0x63)};
		/* IO of 7 ram blocks: {range1=2[blk], sgl=1[blk], range2=3[blk], range3=1[blk]}
		   Block   |   0   |   1   |   2   |   3   |   4   |   5   |   6    |
		   Content |     Range1    |  sgl  |         Range3        | Range1 |
		   LBA     |     lbas[0]   |   2   |         lbas[1]       | lbas[2]|
		*/
		log_line("%s: IO-to-srvr-multi-range %u[blks] + 1sg block", srvr_addr[b], n_blocks);
		gusli::io_multi_map_t* mio = (gusli::io_multi_map_t*)mappend_block(2);			// Scatter gather in third block
		mio->init_num_entries(3);
		mio->entries[0] = (gusli::io_map_t){.data = {.ptr = mappend_block(0), .byte_len = n_block(2), }, .offset_lba_bytes = lbas[0]};
		mio->entries[1] = (gusli::io_map_t){.data = {.ptr = mappend_block(3), .byte_len = n_block(3), }, .offset_lba_bytes = lbas[1]};
		mio->entries[2] = (gusli::io_map_t){.data = {.ptr = mappend_block(6), .byte_len = n_block(1), }, .offset_lba_bytes = lbas[2]};
		test_lba::mmio_fill(mio, info.block_size);
		my_io.io.params.init_multi(gusli::G_READ, info.bdev_descriptor, *mio);
		my_io.exec(gusli::G_WRITE, io_exec_mode::ASYNC_CB);
		test_lba::mmio_verify_and_clean(mio, info.block_size);
		my_io.exec(gusli::G_READ , io_exec_mode::ASYNC_CB);
		test_lba::mmio_verify_and_clean(mio, info.block_size);
	}
	const bool kill_server = true;
	for (int b = 0; b < n_devs; b++) {
		log_line("%s: Disconnect & Kill", srvr_addr[b]);
		struct gusli::backend_bdev_id bdev; bdev.set_from(bdev_uuid[b]);
		my_assert(gc.bufs_unregist(bdev, io_bufs, kill_server) == gusli::connect_rv::C_OK);
	}
	log_uni_success("Client: Test OK\n");
	return 0;
}

