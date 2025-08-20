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
#include "07examples/client/io_submittion_example.hpp"
#include "07examples/client/sample_client.hpp"

#define UNITEST_CLNT_NAME "[_test_]"
/***************************** Base sync IO test ***************************************/
#include <chrono>
class test_timer {	 	// Dont use concurently from multiple threads
	std::chrono::time_point<std::chrono::steady_clock> start;
public:
	void     tic(void) { start = std::chrono::steady_clock::now(); }
	uint64_t toc(bool update_start = false) {
		const auto time_end = std::chrono::steady_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(time_end - start);
		if (update_start) {
			start = time_end;
		}
		return static_cast<uint64_t>(duration.count());	}
	uint64_t toc_tic(void) { return toc(true); }
} timer;
#define log_time(n_micro_sec, fmt, ...) log_unitest(fmt ": time=%5lu.%03u[msec]\n", ##__VA_ARGS__, (n_micro_sec/1000), (unsigned)(n_micro_sec%1000));

static int32_t __get_connected_bdev_descriptor(gusli::global_clnt_context& lib, const gusli::backend_bdev_id bdev) {
	gusli::bdev_info i;
	my_assert(lib.bdev_get_info(bdev, i) == gusli::connect_rv::C_OK);
	log_unitest("\tioable: {bdev uuid=%.16s, fd=%d name=%s, block_size=%u[B], #blocks=0x%lx}\n", bdev.uuid, i.bdev_descriptor, i.name, i.block_size, i.num_total_blocks);
	return i.bdev_descriptor;
}

struct bdev_uuid_cache {
	static constexpr const char* LOCAL_FILE =   "150e8400050e8407";
	static constexpr const char* AUTO_FAIL =    "018867d168867d7";
	static constexpr const char* AUTO_STUCK =   "02_stuck_67d7556";
	static constexpr const char* DEV_ZERO =     "2b3f28dc2b3f28d7";
	static constexpr const char* DEV_NVME =     "3a1e92b3a1e92b7";
	static constexpr const char* REMOTE[] = { "5bcdefab01234567", "6765432123456789", "7b56fa4c9f3316"};
	static constexpr const char* SRVR_NAME[] = { "Bdev0", "Bdev1", "Bdev2"};
	static constexpr const char* SRVR_ADDR[] = { "/dev/shm/gs472f4b04_uds", "t127.0.0.2" /*tcp*/, "u127.0.0.1" /*udp*/ };
	static constexpr const char* SRVR_FAIL_ADDR[] = { "/dev/shm/gsFail0_uds", "/dev/shm/gsFail1_uds" };
} UUID;

void test_non_existing_bdev(gusli::global_clnt_context& lib) {
	std::vector<gusli::io_buffer_t> mem;
	mem.emplace_back(gusli::io_buffer_t::construct(NULL, (1UL << 30)));
	gusli::backend_bdev_id bdev;
	bdev.set_from("NonExist_bdev");
	log_line("test wrong bdev %s", bdev.uuid);
	gusli::bdev_info bdi;
	my_assert(lib.bdev_connect(bdev) == gusli::connect_rv::C_NO_DEVICE);
	my_assert(lib.bdev_get_info(bdev, bdi) == gusli::connect_rv::C_NO_DEVICE);
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_NO_DEVICE);
	my_assert(lib.bdev_bufs_unregist(bdev, mem) == gusli::connect_rv::C_NO_DEVICE);
	my_assert(lib.bdev_disconnect(bdev) == gusli::connect_rv::C_NO_DEVICE);
	lib.bdev_ctl_report_data_corruption(bdev, (1UL << 13));
	unitest_io my_io;
	my_io.io.params.set_dev(345);					// Failed IO with invalid descriptor
	my_io.expect_success(false);
	for_each_exec_mode(i) {
		my_io.exec(gusli::G_READ, (io_exec_mode)i);
	}
}

int base_lib_mem_registration_bad_path(gusli::global_clnt_context& lib, const gusli::backend_bdev_id bdev) {
	my_assert(lib.bdev_connect(bdev) == gusli::C_OK);
	unitest_io my_io[2];
	std::vector<gusli::io_buffer_t> mem;
	mem.reserve(2);
	mem.emplace_back(my_io[0].get_map());
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_OK);
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_WRONG_ARGUMENTS);	// Register same buffer again
	const uint64_t ofst = (mem[0].byte_len / 2);
	mem[0].ptr = (void*)((char*)mem[0].ptr + ofst);
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_WRONG_ARGUMENTS);	// Overlapping buff
	mem[0].ptr = (void*)((char*)mem[0].ptr - 2*ofst);
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_WRONG_ARGUMENTS);	// Overlapping buff
	mem[0].byte_len = 0;
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_WRONG_ARGUMENTS);	// zero length buffer
	mem.clear();
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_WRONG_ARGUMENTS);	// Empty vector of ranges
	my_assert(lib.bdev_bufs_unregist(bdev, mem) == gusli::connect_rv::C_WRONG_ARGUMENTS);	// Empty vector of ranges
	my_assert(lib.bdev_disconnect(bdev) == gusli::connect_rv::C_REMAINS_OPEN);				// Cannot disconnect with mapped buffers
	mem.emplace_back(my_io[1].get_map());
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_OK);				// 2 ranges io[0] and io[1]
	mem.emplace_back(my_io[0].get_map());
	my_assert(lib.bdev_bufs_unregist(bdev, mem) == gusli::connect_rv::C_OK);			// Unregister 2 buffers in reverse order io[1] and io[0]
	my_assert(lib.bdev_bufs_unregist(bdev, mem) == gusli::connect_rv::C_WRONG_ARGUMENTS);
	my_assert(lib.bdev_disconnect(bdev) == gusli::C_OK);
	my_assert(lib.bdev_disconnect(bdev) != gusli::C_OK);			// Double disconnect
	my_assert(lib.bdev_connect(   bdev) == gusli::C_OK);			// Verify can connect and disconnect again
	my_assert(lib.bdev_disconnect(bdev) == gusli::C_OK);
	return 0;
}

int test_io_no_result_check_by_user(gusli::global_clnt_context& lib, const gusli::backend_bdev_id bdev) {
	log_line("Submit io, dont check result");
	static constexpr const auto _ok = gusli::connect_rv::C_OK;
	gusli::bdev_info bdi;
	std::vector<gusli::io_buffer_t> mem;
	mem.emplace_back(alloc_io_buffer(UNITEST_SERVER_BLOCK_SIZE, 4));
	my_assert(lib.bdev_bufs_register(bdev, mem) == _ok);
	my_assert(lib.bdev_get_info(bdev, bdi) == _ok);
	const int32_t fd = bdi.bdev_descriptor;
	sem_t wait;
	const auto __cb = [](void* c) -> void { my_assert(sem_post((sem_t*)c) == 0); };

	for_each_exec_mode(i) {
		io_exec_mode mode = (io_exec_mode)i;
		gusli::io_request io;
		io.params.init_1_rng(gusli::G_READ, fd, bdi.block_size, bdi.block_size, mem[0].ptr);
		io.params.set_try_use_uring((mode == URING_POLLABLE) || (mode == URING_BLOCKING));
		if (mode == io_exec_mode::ASYNC_CB) {
			io.params.set_completion(&wait, __cb);
			my_assert(sem_init(&wait, 0, 0) == 0);
		} else if ((mode == POLLABLE) || (mode == URING_POLLABLE)) {
			io.params.set_async_pollable();
		} else if ((mode == SYNC_BLOCKING_1_BY_1) || (mode == URING_BLOCKING)) {
			io.params.set_blocking();
		}
		io.submit_io();
		if (mode == io_exec_mode::ASYNC_CB)
			my_assert(sem_wait(&wait) == 0);
	}	// Here io destructor is called while io is potentially in air and not checked by user
	my_assert(lib.bdev_bufs_unregist(bdev, mem) == gusli::connect_rv::C_OK);
	free(mem[0].ptr);
	return 0;
}

int base_lib_empty_io_unitest(void) {
	log_line("%s",__FUNCTION__);
	{ gusli::io_request io; }		// Constructor / destructor
	{ gusli::io_request io; (void)io.get_error(); }
	{ gusli::io_request io; (void)io.try_cancel(); }
	{ gusli::io_request io; io.submit_io(); }
	{ gusli::io_request io; io.get_error(); (void)io.try_cancel(); }
	{ gusli::io_request io; io.submit_io(); (void)io.try_cancel(); }
	{ gusli::io_request_base io; io.done(); }
	{ gusli::io_request_base io; io.get_error(); io.get_error(); (void)io.try_cancel(); }
	return 0;
}

int io_race_conditions_unittest(const gusli::bdev_info& binfo, unitest_io& my_io, const char *data, int n_iter_race_tests) {
	if (1) {
		const int n_iters = n_iter_race_tests;
		log_line("%s: Race-Pollable in-air-io test %d[iters]", binfo.name, n_iters);
		my_io.enable_prints(false).expect_success(true).clear_stats();
		timer.tic();
		for (int n = 0; n < n_iters; n++) {
			my_io.clean_buf();
			my_io.exec(gusli::G_READ, POLLABLE);
			my_assert(my_io.io.get_error() == gusli::io_error_codes::E_OK);
			my_assert(strcmp(data, my_io.io_buf) == 0);
		}
		const uint64_t n_micro_sec = timer.toc();
		log_time(n_micro_sec, "Test summary[%s]", io_exec_mode_str(POLLABLE));
		my_io.enable_prints(true).clear_stats();
	}
	if (1) {
		const int n_iters = n_iter_race_tests;
		log_line("%s: Race-Cancel in-air-io test %d[iters]", binfo.name, n_iters);
		my_io.enable_prints(false).clear_stats();
		for_each_exec_async_mode(i) {
			for (int do_blocking_cancel = 0; do_blocking_cancel < 2; do_blocking_cancel++) {
				timer.tic();
				for (int n = 0; n < n_iters; n++) {
					my_io.clean_buf();
					my_io.exec_cancel(gusli::G_READ, (io_exec_mode)i, do_blocking_cancel);
					if (my_io.io.get_error() == gusli::io_error_codes::E_OK)
						my_assert(strcmp(data, my_io.io_buf) == 0);
				}
				const uint64_t n_micro_sec = timer.toc();
				log_time(n_micro_sec, "Test summary[%s]: Blocking=%d.canceled %6u/%6u", io_exec_mode_str((io_exec_mode)i), do_blocking_cancel, my_io.n_cancl, my_io.n_ios);
				my_io.clear_stats();
			}
		}
		my_io.enable_prints(true).clear_stats();
		fflush(stderr);
	}
	return 0;
}

int base_lib_unitests(gusli::global_clnt_context& lib, int n_iter_race_tests = 10000) {
	unitest_io my_io;
	static constexpr const char *data = "Hello world";
	static constexpr const uint64_t data_len = __builtin_strlen(data);
	gusli::backend_bdev_id bdev; bdev.set_from(UUID.LOCAL_FILE);
	my_assert(lib.bdev_connect(bdev) == gusli::connect_rv::C_OK);
	my_assert(lib.bdev_connect(bdev) == gusli::connect_rv::C_REMAINS_OPEN);
	gusli::bdev_info bdi;
	my_assert(lib.bdev_get_info(bdev, bdi) == gusli::connect_rv::C_OK);
	const int32_t fd = bdi.bdev_descriptor;
	std::vector<gusli::io_buffer_t> mem; mem.emplace_back(my_io.get_map());
	my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_OK);
	my_io.io.params.init_1_rng(gusli::G_NOP, fd, 0, data_len, my_io.io_buf);
	my_assert(my_io.io.try_cancel() == gusli::io_request::cancel_rv::G_ALLREADY_DONE);	// IO not launched so as if already done
	if (1) {
		log_line("Submit async/sync/pollable write");
		for_each_exec_mode(i) {
			strcpy(my_io.io_buf, data);
			my_io.exec(gusli::G_WRITE, (io_exec_mode)i);
			my_io.clean_buf();
			my_io.exec(gusli::G_READ, (io_exec_mode)i);
			my_assert(strcmp(data, my_io.io_buf) == 0);
			my_assert(my_io.io.try_cancel() == gusli::io_request::cancel_rv::G_ALLREADY_DONE);	// Blocking io already finished/succeeded
		}
		test_io_no_result_check_by_user(lib, bdev);
		std::string msg = "print_clnt_msg";
		my_assert(gusli::connect_rv::C_OK == lib.bdev_ctl_log_msg(bdev, msg));
		my_assert(gusli::connect_rv::C_OK == lib.bdev_ctl_reboot(bdev, msg));
	}
	if (1) {
		log_line("Cancel while io is in air all modes");
		my_io.clear_stats();
		for_each_exec_mode(i) {
			my_io.clean_buf();
			my_io.exec_cancel(gusli::G_READ, (io_exec_mode)i);
			if (my_io.io.get_error() == gusli::io_error_codes::E_OK)
				my_assert(strcmp(data, my_io.io_buf) == 0);
		}
	}
	io_race_conditions_unittest(bdi, my_io, data, n_iter_race_tests);

	if (1) {
		log_line("Multi-Range-Read");
		static constexpr const char *multi_io_read_result = "orelloHew";		// Expected permutation of 'data' buffer
		static constexpr const int multi_io_read_length = __builtin_strlen(multi_io_read_result);
		static constexpr const int n_ranges = 4;
		static constexpr const size_t multi_io_size = sizeof(gusli::io_multi_map_t) + n_ranges * sizeof(gusli::io_map_t);
		gusli::io_multi_map_t* mio = (gusli::io_multi_map_t*)&my_io.io_buf[1<<14];	// multi-io, sg at offset of 8K
		char *p = my_io.io_buf;
		mio->init_num_entries(n_ranges);
		mio->entries[0].init(&p[0], 2, 7);	// "Hello world" -> "or"
		mio->entries[1].init(&p[2], 4, 1);	// "Hello world" -> "ello"
		mio->entries[2].init(&p[6], 2, 0);	// "Hello world" -> "He"
		mio->entries[3].init(&p[8], 1, 6);	// "Hello world" -> "w"
		my_io.io.params.init_multi(gusli::G_READ, fd, *mio);
		my_assert(multi_io_size        == mio->my_size());
		my_assert(multi_io_size        == my_io.io.params.map().data.byte_len);
		my_assert(multi_io_read_length == mio->buf_size());
		my_assert(multi_io_read_length == my_io.io.params.buf_size());
		for_each_exec_mode(i) {
			my_io.clean_buf();
			my_io.exec(gusli::G_READ, (io_exec_mode)i);
			my_assert(strcmp(multi_io_read_result, p) == 0);
		}
		my_io.clean_buf();
	}
	my_assert(lib.bdev_bufs_unregist(bdev, mem) == gusli::connect_rv::C_OK);
	my_assert(lib.bdev_disconnect(bdev) == gusli::C_OK);

	if (1) {
		log_line("Failed read");
		bdev.set_from(UUID.AUTO_FAIL);
		my_assert(lib.bdev_connect(bdev) == gusli::connect_rv::C_OK);
		my_io.io.params.init_1_rng(gusli::G_NOP, __get_connected_bdev_descriptor(lib, bdev), 0, 11, my_io.io_buf);
		my_io.expect_success(false);
		for_each_exec_mode(i) {
			my_io.exec(gusli::G_NOP,   (io_exec_mode)i);
			my_io.exec(gusli::G_READ,  (io_exec_mode)i);
			my_io.exec(gusli::G_WRITE, (io_exec_mode)i);
			my_assert(my_io.io.try_cancel() == gusli::io_request::cancel_rv::G_ALLREADY_DONE);	// Already Failed, cannot cancel
		}
		lib.~global_clnt_context();							// failed destroy, bdev is still open
		my_assert(lib.bdev_disconnect(bdev) == gusli::C_OK);
	}
	if (1) test_non_existing_bdev(lib);
	if (1) {
		log_line("Legacy /dev/zero tests");
		bdev.set_from(UUID.DEV_ZERO);
		my_assert(lib.bdev_connect(bdev) == gusli::connect_rv::C_OK);
		my_assert(lib.bdev_get_info(bdev, bdi) == gusli::connect_rv::C_OK);
		my_assert(lib.bdev_bufs_register(bdev, mem) == gusli::connect_rv::C_OK);
		my_io.io.params.init_1_rng(gusli::G_NOP, __get_connected_bdev_descriptor(lib, bdev), 0, 1 * bdi.block_size, my_io.io_buf);
		my_assert(bdi.block_size == 4096);
		my_io.expect_success(true);
		for_each_exec_mode(i) {
			my_io.exec(gusli::G_READ, (io_exec_mode)i);
			my_assert(strcmp("", my_io.io_buf) == 0);
			my_io.exec(gusli::G_WRITE,(io_exec_mode)i);
		}

		if (1) {// Test IO beyond max LBA, should fail
			my_io.io.params.change_map().offset_lba_bytes += (1UL << 62);
			my_io.expect_success(false);
			for_each_exec_mode(i) {
				my_io.exec(gusli::G_WRITE,(io_exec_mode)i);
			}
		}

		my_assert(lib.bdev_bufs_unregist(bdev, mem) == gusli::connect_rv::C_OK);
		my_assert(lib.bdev_disconnect(bdev) == gusli::connect_rv::C_OK);
		base_lib_mem_registration_bad_path(lib, bdev);
	}
	return 0;
}

int client_stuck_io_and_throttle_tests(gusli::global_clnt_context& lib, const char* bdev_uuid) {
	log_line("Stuck IO tests, uuid=%s", bdev_uuid);
	static constexpr const auto _ok = gusli::connect_rv::C_OK;
	gusli::backend_bdev_id bdev;
	bdev.set_from(bdev_uuid);
	static constexpr const uint32_t n_ios = 6;
	unitest_io my_io[n_ios];
	std::vector<gusli::io_buffer_t> mem; mem.reserve(n_ios);
	for (uint32_t i = 0; i < n_ios; i++)
		mem.emplace_back(my_io[i].get_map());
	my_assert(lib.open__bufs_register(bdev, mem) == _ok);
	gusli::bdev_info bdi;
	my_assert(lib.bdev_get_info(bdev, bdi) == _ok);
	my_assert(n_ios > bdi.num_max_inflight_io);				// We test throttling as well
	const int32_t fd = bdi.bdev_descriptor;
	const io_exec_mode tested_modes[3] = {io_exec_mode::POLLABLE, io_exec_mode::ASYNC_CB, io_exec_mode::URING_POLLABLE };	//for_each_exec_async_mode(i)

	// Submit A few reads to overlapping lba's
	for (uint32_t i = 0; i < n_ios; i++) {
		my_io[i].expect_success(false).io.params.init_1_rng(gusli::G_NOP, fd, bdi.block_size, (i+1) * bdi.block_size, my_io[i].io_buf);
		my_assert(my_io[i].io.params.map().data.byte_len <= my_io->get_map().byte_len);	// Sanity that we dont do mem corrupt
		my_io[i].exec_dont_block(gusli::G_READ, tested_modes[i%3]);	// Dont wait for io completion
	}
	// Verify IO's state (while it is in air)
	for (uint32_t i = 0; i < bdi.num_max_inflight_io; i++) {	// Ios are stuck
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_IN_TRANSFER);
	}
	for (uint32_t i = bdi.num_max_inflight_io; i < n_ios; i++) {	// Those io's were throttled
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_THROTTLE_RETRY_LATER);
		my_assert(my_io[i].io.try_cancel() == gusli::io_request::cancel_rv::G_ALLREADY_DONE);	// IO not launched so as if already done
	}
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == bdi.num_max_inflight_io);

	// Stop all IO's
	my_assert(lib.close_bufs_unregist(bdev, mem) != _ok);			// IOs are still in air cant unregister/close bdev
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == bdi.num_max_inflight_io);
	my_assert(lib.bdev_force_refresh(bdev) == _ok);					// Disconnect reconnect and drain ios
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == 0);
	my_assert(lib.bdev_force_refresh(bdev) == _ok);					// Second stop is meaningless

	// Verify all in-air ios terminated
	for (uint32_t i = 0; i < bdi.num_max_inflight_io; i++) {	// Stuck Ios were canceled
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_CANCELED_BY_CALLER);
		my_io[i].exec_dont_block_finish();
	}
	for (uint32_t i = bdi.num_max_inflight_io; i < n_ios; i++) {	// Throttled ios remain untouched
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_THROTTLE_RETRY_LATER);
		my_io[i].exec_dont_block_finish();
	}
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == 0);

	if (1) {
		log_unitest("After drain, Retry the same ios, server disconnects client\n");
		std::string dbg_msg = "FORCE disconenct";
		for (uint32_t i = 0; i < bdi.num_max_inflight_io; i++) {
			my_io[i].exec_dont_block(gusli::G_READ, tested_modes[i%3]);	// Dont wait for io completion
			my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_IN_TRANSFER);
		}
		my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == bdi.num_max_inflight_io);
		my_assert(lib.bdev_ctl_reboot(bdev, dbg_msg) == _ok);					// Disconnect reconnect and drain ios
		my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == 0);
		for (uint32_t i = 0; i < bdi.num_max_inflight_io; i++) {	// Stuck Ios were canceled
			my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_CANCELED_BY_CALLER);
			my_io[i].exec_dont_block_finish();
		}
	}

	log_unitest("After drain, Retry the same ios, client cancels them 1by1\n");
	for (uint32_t i = 0; i < bdi.num_max_inflight_io; i++) {
		my_io[i].exec_dont_block(gusli::G_READ, tested_modes[i%3]);
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_IN_TRANSFER);
	}
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == bdi.num_max_inflight_io);

	// Cancel all except 1 directly by IO pointers
	for (uint32_t i = 1; i < bdi.num_max_inflight_io; i++) {
		my_assert(my_io[i].io.try_cancel() ==  gusli::io_request::cancel_rv::G_CANCELED);
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_CANCELED_BY_CALLER);
		my_io[i].exec_dont_block_finish();
	}
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == 1);		// Only my_io[0] is in air

	// Resubmit the io's that were previously throtteled
	for (uint32_t i = bdi.num_max_inflight_io; i < n_ios; i++) {
		my_io[i].exec_dont_block(gusli::G_READ, tested_modes[i%3]);
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_IN_TRANSFER);
	}
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == (n_ios-bdi.num_max_inflight_io+1));

	// Stop the remaining io's, close the server and verify state
	my_assert(lib.bdev_force_close(bdev) == _ok);
	for (uint32_t i = bdi.num_max_inflight_io; i < n_ios; i++) {
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_CANCELED_BY_CALLER);
		my_io[i].exec_dont_block_finish();
	}
	for (uint32_t i = 0; i < 1; i++) {
		my_assert(my_io[i].io.get_error() == gusli::io_error_codes::E_CANCELED_BY_CALLER);
		my_io[i].exec_dont_block_finish();
	}
	my_assert(lib.bdev_ctl_get_num_in_air_ios(bdev) == 0);			// Blockdevice is not connected so 0 is returned

	// Block device already closed, Cannot close again
	my_assert(lib.close_bufs_unregist(bdev, mem, true) == gusli::connect_rv::C_NO_RESPONSE);
	my_assert(lib.bdev_force_close(bdev) == _ok);	// Already closed, do nothing

	// Refresh will actually connect but without any registered mem
	my_assert(lib.bdev_force_refresh(bdev) == _ok);
	mem.clear();
	my_assert(lib.close_bufs_unregist(bdev, mem, true) == _ok);		// At last disconnect + kill server
	return 0;
}

/***************************** Clnt Server test ***************************************/
#include "07examples/server/read_only_ram.hpp"
#include "07examples/server/fail_server_ram.hpp"

template<class T> class atomic {
	T v;
 public:
	atomic() : v(0) {}
	atomic(T new_val) : v(new_val) {}
	inline void set(T new_val) { __atomic_store_n(&v, new_val, __ATOMIC_SEQ_CST); }
	inline T read(void) const { return __atomic_load_n(&v, __ATOMIC_SEQ_CST); }
	inline T inc(void) { /* (++(*v)); */ return __atomic_add_fetch(&v,  1, __ATOMIC_SEQ_CST); }
	inline T dec(void) { /* (--(*v)); */ return __atomic_add_fetch(&v, -1, __ATOMIC_SEQ_CST); }
};
typedef atomic<uint64_t> atomic_uint64_t;

class all_ios_t *glbal_all_ios = NULL;
class all_ios_t {
	gusli::io_request ios[512];
	atomic_uint64_t n_completed_ios;
	atomic_uint64_t n_in_air_ios;
	uint64_t n_ios_todo;
	int n_max_ios_in_air;
	uint32_t block_size;
	sem_t wait;						// Block test until completes
	static void __comp_cb(gusli::io_request *c) {
		my_assert(c->get_error() == 0);
		const uint64_t n_completed_ios = glbal_all_ios->n_completed_ios.inc();
		// log_unitest("Submit n_comp=%lu, %lu\n", n_completed_ios, glbal_all_ios->n_ios_todo);
		if (n_completed_ios < glbal_all_ios->n_ios_todo) {
			c->params.change_map().offset_lba_bytes += 7*glbal_all_ios->block_size;		// Read from a different place
			c->done();
			c->submit_io();
			return;
		}
		const uint64_t still_in_air = glbal_all_ios->n_in_air_ios.dec();
		if (still_in_air == 0)
			my_assert(sem_post(&glbal_all_ios->wait) == 0);				// Unblock waiter
	}
 public:
	all_ios_t(const gusli::io_buffer_t io_buf, const gusli::bdev_info& info) {
		block_size = info.block_size;
		n_max_ios_in_air = info.num_max_inflight_io;
		my_assert((int)(sizeof(ios)/sizeof(ios[0])) >= n_max_ios_in_air);		// Arrays is large enough
		for (int i=0; i < n_max_ios_in_air; i++) {
			auto *p = &ios[i].params;
			p->init_1_rng(gusli::G_READ, info.bdev_descriptor, (i * block_size) + 0x100000, 1 * block_size, (char*)io_buf.ptr + (i * block_size)); // Destination buffer for read
			p->set_priority(100).set_safe_io(true).set_mutalbe_data(false).set_completion(&ios[i], __comp_cb);
		}
		glbal_all_ios = this;
		n_completed_ios.set(0);
	}
	template<class T> static inline T min(         T x, T y) { return x < y ? x : y; }
	void launch_perf_reads(uint64_t _n_ios_todo) {
		n_ios_todo = _n_ios_todo;
		const int io_depth = (int)min(_n_ios_todo, (uint64_t)n_max_ios_in_air);
		n_in_air_ios.set(io_depth);
		my_assert(sem_init(&wait, 0, 0) == 0);
		if (n_completed_ios.read() == 0)
			log_unitest("\tperfTest %lu[op=%c], io_size=%lu[b], io_depth=%u\n", _n_ios_todo, ios[0].params.op(), ios[0].params.buf_size(), io_depth);
		n_completed_ios.set(0);
		timer.tic();
		for (int i = 0; i < io_depth; i++) {
			ios[i].submit_io();
		}
		my_assert(sem_wait(&wait) == 0);
		const uint64_t n_micro_sec = timer.toc();
		const uint64_t n_done_ios = n_completed_ios.read();
		log_time(n_micro_sec, "\tperfTest %lu[Kios], %lu[Kio/s]", n_done_ios/1000, ((n_completed_ios.read()*1000)/ n_micro_sec));
		for (int i = 0; i < io_depth; i++) {
			ios[i].done();
		}
	}
	~all_ios_t() { }
};

static void _remote_server_bad_path_io_unitests(const gusli::bdev_info& info, const gusli::io_buffer_t& map) {
	#define dst_block(i) mappend_block(map.ptr, i)
	gusli::io_request io;
	io.params.set_dev(info.bdev_descriptor);
	io.submit_io(); my_assert(io.get_error() != 0);			// No completion function
	const auto __io_invalid_arg_comp_cb = [] (void* ctx) -> void {
		gusli::io_request *_io = (gusli::io_request *)ctx;
		my_assert(_io->get_error() == gusli::io_error_codes::E_INVAL_PARAMS);
	};
	io.params.set_completion(&io, __io_invalid_arg_comp_cb);
	io.done(); io.submit_io(); 										// No mapped buffers
	gusli::io_multi_map_t* mio = (gusli::io_multi_map_t*)calloc(1, 4096);
	io.params.init_multi(gusli::G_READ, info.bdev_descriptor, *mio);
	mio->init_num_entries(1);
	io.done(); io.submit_io(); 										// < 2 ranges are not allowed
	mio->init_num_entries(2);
	io.done(); io.submit_io(); 										// Wrong mapping of first range, it is zeroed
	mio->entries[1] = mio->entries[0].init((void*)(1 << 20), (1 << 20), (1 << 20));
	io.done(); io.submit_io(); 										// Wrong mapping of first range, it is not inside shared memory area
	mio->entries[1] = mio->entries[0].init(dst_block(2), n_block(1), n_block(3));
	io.done(); io.submit_io(); 										// Correct mapping, but scatter gather itself is not inside shared memory area
	free(mio);
	mio = (gusli::io_multi_map_t*)dst_block(0);
	mio->init_num_entries(2);
	mio->entries[1] = mio->entries[0].init(dst_block(3), n_block(2), n_block(1) / 3);
	io.params.init_multi(gusli::G_READ, info.bdev_descriptor, *mio);
	io.done(); io.submit_io(); 										// Fractional block offset
	mio->entries[1].offset_lba_bytes = mio->entries[0].offset_lba_bytes = (1UL << 62);
	io.done(); io.submit_io(); 										// LBA outside of block device range
	mio->entries[1].offset_lba_bytes = mio->entries[0].offset_lba_bytes = 0;
	io.params.init_1_rng(gusli::G_READ, info.bdev_descriptor, (1UL << 62), n_block(2), dst_block(0));
	io.done(); io.submit_io(); 										// LBA outside of block device range
}

static void __verify_mapped_properly(const std::vector<gusli::io_buffer_t>& io_bufs) {
	static constexpr const size_t test_phrase_len = 32;
	char test_phrase[test_phrase_len + 1] __attribute__((aligned(sizeof(long))));
	strcpy(test_phrase, "|Unit-test 32[b] dummy pattern |");
	for (const gusli::io_buffer_t& b : io_bufs) {
		char* p = (char*)b.ptr;
		p[b.byte_len/2] = p[b.byte_len-1] = p[0] = 'a';						// Write to  start/middle/end of buffer
		my_assert('a' == (p[b.byte_len/2] ^ p[b.byte_len-1] ^ p[0]));		// Read from start/middle/end of buffer
		// Time consuming for large buffers, write to each and every page to verify it is properly mapped
		for (size_t i = 0; i < b.byte_len; i += 4096) // += test_phrase_len to write to each and every byte
			memcpy(&p[i], test_phrase, test_phrase_len);
	}
}

void client_no_server_reply_test(gusli::global_clnt_context& lib) {
	static constexpr const int si = 0;		// Server index
	log_line("Remote server %s(%s): no reply test", UUID.SRVR_NAME[si], UUID.SRVR_ADDR[si]);
	gusli::backend_bdev_id bdev; bdev.set_from(UUID.REMOTE[si]);
	my_assert(UUID.SRVR_ADDR[si][0] != 'u');			// udp will just get stuck waiting for server, this test should run on uds or tcp
	const auto con_rv = lib.bdev_connect(bdev);
	if (con_rv == gusli::connect_rv::C_OK)
		log_uni_failure("There is another server process running in parallel to unitest. Kill it and rerun!\n\n");
	my_assert(con_rv == gusli::connect_rv::C_NO_RESPONSE);
	my_assert(lib.bdev_connect(bdev) == gusli::connect_rv::C_NO_RESPONSE);
}

#include <unistd.h>  // for fork()
#include <sys/wait.h>

static void __connect_to_servers(gusli::global_clnt_context& lib, int n_servers) {
	// Connect to all servers, important not to do this 1 by 1, to test multiple bdevs
	for (int s = 0; s < n_servers; s++) {
		gusli::backend_bdev_id bdev; bdev.set_from(UUID.REMOTE[s]);
		gusli::bdev_info info;
		{
			int n_attempts = 0;
			enum gusli::connect_rv con_rv = gusli::connect_rv::C_NO_RESPONSE;
			for (; ((con_rv == gusli::connect_rv::C_NO_RESPONSE) && (n_attempts < 10)); n_attempts++ ) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));	// Wait for servers to be up
				con_rv = lib.bdev_connect(bdev);
			}
			my_assert(con_rv == gusli::connect_rv::C_OK);
			__get_connected_bdev_descriptor(lib, bdev);
		}
		my_assert(lib.bdev_get_info(bdev, info) == gusli::connect_rv::C_OK);
		my_assert(strstr(info.name, UUID.SRVR_NAME[s]) != NULL);
	}
	{
		gusli::backend_bdev_id bdev; bdev.set_from(UUID.REMOTE[0]);
		std::string msg = "  \t  print_clnt_msg";		// With non visible prefix that should be removed
		enum gusli::connect_rv msg_rv = lib.bdev_ctl_log_msg(bdev, msg);
		my_assert(msg_rv == gusli::connect_rv::C_OK);
	}
}

void client_server_basic_test(gusli::global_clnt_context& lib, int num_ios_preassure) {
	static constexpr const int n_servers = 3;
	log_line("Remote %d server launch", n_servers);
	struct {
		union {
			pthread_t tid;								// Thread  id when server is lauched as thread
			__pid_t   pid;								// Process id when server is lauched as process via fork()
		};
	} child[n_servers];
	static constexpr bool launch_server_as_process = true;
	for (int i = 0; i < n_servers; i++) {
		child[i].pid = fork();
		my_assert(child[i].pid >= 0);
		if (child[i].pid == 0) {	// Child process
			const bool use_extenral_loop = (UUID.SRVR_ADDR[i][0] == 't');
			{
				server_ro_lba ds(UUID.SRVR_NAME[i], UUID.SRVR_ADDR[i], use_extenral_loop);
				ds.run();
			}
			exit(0);
		}
	}
	__connect_to_servers(lib, n_servers);
	unitest_io my_io;
	std::vector<gusli::io_buffer_t> io_bufs;
	io_bufs.reserve(2);
	io_bufs.emplace_back(alloc_io_buffer(UNITEST_SERVER_BLOCK_SIZE, MAX_SERVER_IN_FLIGHT_IO));
	io_bufs.emplace_back(my_io.get_map());										// shared buffer for 1 io test

	for (int s = 0; s < n_servers; s++) {
		gusli::backend_bdev_id bdev; bdev.set_from(UUID.REMOTE[s]);
		gusli::bdev_info info;
		my_assert(lib.bdev_get_info(bdev, info) == gusli::connect_rv::C_OK);
		const bool is_unaligned_block = ((info.block_size % UNITEST_SERVER_BLOCK_SIZE) != 0);
		my_assert(!is_unaligned_block);	// Else: unitest io buffer is not properly alligned
		my_assert(info.num_max_inflight_io >= MAX_SERVER_IN_FLIGHT_IO);	// Else: not enough unitest buffers for all io's

		// Map app buffers for read operations
		log_line("Remote server %s, uuid=%s: map bufs", info.name, UUID.SRVR_NAME[s]);
		my_assert(lib.bdev_bufs_register(bdev, io_bufs) == gusli::connect_rv::C_OK);
	}

	// Simple io test vs each server
	for (int s = 0; s < n_servers; s++) {
		gusli::backend_bdev_id bdev; bdev.set_from(UUID.REMOTE[s]);
		gusli::bdev_info info;
		my_assert(lib.bdev_get_info(bdev, info) == gusli::connect_rv::C_OK);
		if (1) _remote_server_bad_path_io_unitests(info, io_bufs[0]);
		for (int j = 0; j < 2; j++)
			client_test_write_read_verify_1blk(info, my_io, j * 17 * info.block_size);	// Test 1 block write-read on lba's 0 and 17
		if (1) {
			log_line("%s: IO-to-srvr-multi-range", UUID.SRVR_NAME[s]);
			client_test_write_read_verify_multi(info, io_bufs);
		}

		if (s == 0) { // Lauch async perf read test on first server only
			log_line("IO-to-srvr-perf %u[Mio]", (num_ios_preassure >> 20));
			all_ios_t ios(io_bufs[0], info);
			for (int i = 0; i < 4; i++)
				ios.launch_perf_reads(num_ios_preassure);
		}

		log_line("%s: Unmap bufs", UUID.SRVR_NAME[s]);
		// Unmap buffers and disconnect from server
		my_assert(lib.bdev_disconnect(bdev) != gusli::C_OK);			// Cannot disconnect with mapped buffers
		my_assert(lib.bdev_bufs_unregist(bdev, io_bufs) == gusli::connect_rv::C_OK);
		my_assert(lib.bdev_bufs_unregist(bdev, io_bufs) == gusli::connect_rv::C_WRONG_ARGUMENTS);	// Non existent buffers
		for (int j = 0; j < 2; j++) {
			log_line("%s: Rereg-Unreg bufs again (iter=%d)", UUID.SRVR_NAME[s], j);
			my_assert(lib.bdev_bufs_register(bdev, io_bufs) == gusli::connect_rv::C_OK);
			my_assert(lib.bdev_bufs_unregist(bdev, io_bufs) == gusli::connect_rv::C_OK);
		}
		if (1) {		// Verify reg/unreg did not ruin original user buffers virtual mem mapping
			__verify_mapped_properly(io_bufs);
		}
		log_line("%s: Disconnect from server", UUID.SRVR_NAME[s]);
		my_assert(lib.bdev_disconnect(bdev) == gusli::C_OK);
		log_line("%s: Connect again", UUID.SRVR_NAME[s]);
		my_assert(lib.bdev_connect(bdev) == gusli::connect_rv::C_OK);
		__get_connected_bdev_descriptor(lib, bdev);
		my_assert(lib.bdev_get_info(bdev, info) == gusli::connect_rv::C_OK);
		my_assert(strstr(info.name, UUID.SRVR_NAME[s]) != NULL);
		log_line("%s: Disconnect & Kill", UUID.SRVR_NAME[s]);
		lib.bdev_ctl_report_data_corruption(bdev, 0);			// Kill the server
	}
	for (gusli::io_buffer_t& buf : io_bufs)
		free(buf.ptr);
	my_io.io_buf = nullptr;	// Because we already freed it in the line above
	io_bufs.clear();

	// Wait for all servers process to finish
	if (launch_server_as_process) {
		for (int i = 0; i < n_servers; ++i) {
			wait_for_process(child[i].pid, "server_done");
		}
	} else {
		for (int i = 0; i < n_servers; ++i) {
			const int err = pthread_join(child[i].tid, NULL);
			my_assert(err == 0);
		}
	}
}

void client_server_stuck_io_and_throttle_test(gusli::global_clnt_context& lib) {
	static constexpr const int n_servers = 1;
	log_line("Remote %d Stuck_io server launch", n_servers);
	struct {
		__pid_t   pid;								// Process id when server is lauched as process via fork()
	} child[n_servers];
	for (int i = 0; i < n_servers; i++) {
		child[i].pid = fork();
		my_assert(child[i].pid >= 0);
		if (child[i].pid == 0) {	// Child process
			fail_server_ram ds(UUID.SRVR_NAME[i], UUID.SRVR_ADDR[i], (i==0));
			ds.run();
			exit(0);
		}
	}
	__connect_to_servers(lib, n_servers);
	client_stuck_io_and_throttle_tests(lib, UUID.REMOTE[0]);

	// Wait for all servers process to finish
	for (int i = 0; i < n_servers; ++i) {
		wait_for_process(child[i].pid, "server_done");
	}
}

void lib_uninitialized_invalid_unitests(gusli::global_clnt_context& lib) {
	log_line("Uninitialized library tests");
	my_assert(lib.get_metadata_json()[0] == (char)0);			// Empty
	lib.~global_clnt_context();									// Does nothing so succeeds
	lib.~global_clnt_context();									// Does nothing so succeeds
	gusli::backend_bdev_id bdev; bdev.set_from("something");
	gusli::bdev_info bdi;
	std::vector<gusli::io_buffer_t> mem;
	const gusli::connect_rv rv = gusli::connect_rv::C_NO_DEVICE;
	my_assert(rv == lib.bdev_connect(bdev));
	my_assert(rv == lib.bdev_get_info(bdev, bdi));
	my_assert(lib.bdev_get_descriptor(bdev) < 0);
	my_assert(rv == lib.bdev_bufs_register(bdev, mem));
	my_assert(rv == lib.bdev_bufs_unregist(bdev, mem));
	my_assert(rv == lib.open__bufs_register(bdev, mem));
	my_assert(rv == lib.close_bufs_unregist(bdev, mem));
	my_assert(rv == lib.bdev_disconnect(bdev));
	lib.bdev_ctl_report_data_corruption(bdev, 0);
	unitest_io my_io;	// Write 100 bytes
	my_io.expect_success(false).enable_prints(false);
	my_io.io.params.init_1_rng(gusli::G_WRITE,  0, 5, 100, NULL);
	my_io.exec(gusli::G_WRITE, ASYNC_CB);
	my_io.io.params.init_1_rng(gusli::G_READ,  -1, 8, 999, NULL);
	my_io.exec(gusli::G_READ,  SYNC_BLOCKING_1_BY_1);
	my_io.io.params.init_1_rng(gusli::G_WRITE,  1, 0, 16, NULL);
	my_io.exec(gusli::G_READ,  POLLABLE);
	std::string msg = "print_clnt_msg";
	my_assert(rv == lib.bdev_ctl_log_msg(bdev, msg));
	my_assert(rv == lib.bdev_ctl_reboot(bdev, msg));
}

gusli::global_clnt_context* lib_initialize_unitests(void) {
	gusli::global_clnt_context::init_params p;
	char clnt_name[32];
	gusli::client_config_file conf(1 /*Version*/);
	strncpy(clnt_name, UNITEST_CLNT_NAME, sizeof(clnt_name));
	p.client_name = clnt_name;
	p.max_num_simultaneous_requests = MAX_SERVER_IN_FLIGHT_IO;
	{	// Generate config
		using gsc = gusli::bdev_config_params;
		conf.bdev_add(gsc(UUID.LOCAL_FILE,    gsc::bdev_type::DEV_FS_FILE,     "./store.bin",  "sec=0x11", 0, gsc::connect_how::EXCLUSIVE_RW));
		conf.bdev_add(gsc(UUID.AUTO_STUCK,    gsc::bdev_type::DUMMY_DEV_STUCK, "__STUCK__",    "sec=0x12", 0, gsc::connect_how::READ_ONLY));
		conf.bdev_add(gsc(UUID.AUTO_FAIL,     gsc::bdev_type::DUMMY_DEV_FAIL,  "___FAIL__",    "sec=0x21", 0, gsc::connect_how::EXCLUSIVE_RW));
		conf.bdev_add(gsc(UUID.DEV_ZERO,      gsc::bdev_type::DEV_BLK_KERNEL,  "/dev/zero",    "sec=0x22", 0, gsc::connect_how::EXCLUSIVE_RW));
		conf.bdev_add(gsc(UUID.DEV_NVME,      gsc::bdev_type::DEV_BLK_KERNEL,  "/dev/nvme0n1", "sec=0x23", 1, gsc::connect_how::SHARED_RW));
		for (int i=0; i < 3; i++)
			conf.bdev_add(gsc(UUID.REMOTE[i], gsc::bdev_type::REMOTE_SRVR,  UUID.SRVR_ADDR[i], "sec=0x35", 1, gsc::connect_how::EXCLUSIVE_RW));
		#if 0
			p.config_file = "./gusli.conf";		// Can use external file
		#else
			p.config_file = conf.get();
		#endif
	}
	log_line("Init/Destroy tests");
	using gc = gusli::global_clnt_context;
	using ge = gusli::clnt_init_exception;
	for (int i = 0; i < 3; i++) {				// Create Library failure (empty config file)
		gc::init_params p0;
		try {
			gc lib(p0);
		} catch (const ge& e) { my_assert(e.code() < 0); }
	}
	if (1) {
		gc lib(p);
		try {
			gc lib2(p);							// Fail to initialize a second time
		} catch (const ge& e) { my_assert(e.code() == EEXIST); }
		gc *lib2 = nullptr;
		try {
			lib2 = new gc(p);					// Fail to initialize a second time via heap
		} catch (const ge& e) { my_assert(e.code() == EEXIST); }
		my_assert(lib2 == nullptr);
		#if 0										// Copy/move/assignment is disabled
			{ gc lib3 = lib; }						// Destroy the library via lib3
			{ gc lib3 = lib; }						// Copy destroyed library
			{ gc lib3(lib); }						// Copy destroyed library
			{ gc lib3 = std::move(lib); }			// Move destroyed library
		#else
			lib.~global_clnt_context();
		#endif
		lib_uninitialized_invalid_unitests(lib);
	}
	log_line("Library up");
	gc *rv = new gc(p);
	memset((void*)&p, 0xCC, sizeof(p));						// Trap usage by gusli library of params memory after initialization
	try { rv = new gc(p);									// Second initialization, even with garbage params is also OK
	} catch (const ge& e) { my_assert(e.code() == EEXIST); }
	log_unitest("\tmetadata= %s\n", rv->get_metadata_json());
	my_assert(rv->BREAKING_VERSION == 1);					// Much like in a real app. Unitests built for specific library version
	memset(clnt_name, 0xCC, sizeof(clnt_name));
	return rv;
}

void unitest_auto_open_close(const gusli::global_clnt_context* lib) {
	log_line("/dev/zero read with auto open/close");
	unitest_io my_io[2];
	gusli::backend_bdev_id bdev;
	bdev.set_from(UUID.DEV_ZERO);
	// Register buffer with kernel bdev - forces an auto open
	std::vector<gusli::io_buffer_t> mem0, mem1;
	mem0.emplace_back(my_io[0].get_map());
	mem1.emplace_back(my_io[1].get_map());
	my_assert(lib->open__bufs_register(bdev, mem0) == gusli::connect_rv::C_OK);
	my_assert(lib->bdev_disconnect(bdev) == gusli::connect_rv::C_REMAINS_OPEN); // Verify was auto-opened, 1 buffer
	my_assert(lib->open__bufs_register(bdev, mem1) == gusli::connect_rv::C_OK);
	my_assert(lib->bdev_disconnect(bdev) == gusli::connect_rv::C_REMAINS_OPEN); // Verify was auto-opened, 2 registered buffers
	my_io[0].io.params.init_1_rng(gusli::G_NOP, lib->bdev_get_descriptor(bdev), (1 << 14), 4096, my_io[0].io_buf);
	my_io[0].expect_success(true).clean_buf();
	my_io[0].exec(gusli::G_READ, SYNC_BLOCKING_1_BY_1);
	my_assert(strcmp("", my_io[0].io_buf) == 0);
	my_assert(lib->close_bufs_unregist(bdev, mem0) == gusli::connect_rv::C_OK);
	my_assert(lib->bdev_connect(bdev) == gusli::connect_rv::C_REMAINS_OPEN); // Verify still open
	my_assert(lib->close_bufs_unregist(bdev, mem1) == gusli::connect_rv::C_OK);			// Auto closed here
	my_assert(lib->close_bufs_unregist(bdev, mem1) == gusli::connect_rv::C_NO_RESPONSE);	// Verify was autoclosed
	__verify_mapped_properly(mem0);
	__verify_mapped_properly(mem1);
}

void unitest_huge_mem_map_and_io(const gusli::global_clnt_context* lib) {
	const size_t n_bytes = 0x100000000UL, block_size = 4096;	// 4[GB]
	log_line("/dev/zero mmap %lu[GB]", (n_bytes >> 30));
	gusli::backend_bdev_id bdev;
	bdev.set_from(UUID.DEV_ZERO);
	std::vector<gusli::io_buffer_t> io_bufs;
	uint64_t n_micro_sec;
	timer.tic();
	io_bufs.emplace_back(alloc_io_buffer(block_size, n_bytes / block_size));
	const gusli::io_buffer_t &map = io_bufs[0];
	//__verify_mapped_properly(io_bufs); n_micro_sec = timer.toc_tic(); log_time(n_micro_sec, "Verify write");
	my_assert(lib->open__bufs_register(bdev, io_bufs) == gusli::connect_rv::C_OK);
	n_micro_sec = timer.toc_tic(); log_time(n_micro_sec, "register-mem took");
	gusli::io_request io;
	io.params.init_1_rng(gusli::G_READ, lib->bdev_get_descriptor(bdev), 0, n_bytes, map.ptr);
	io.params.set_blocking();
	io.submit_io(); io.done();
	n_micro_sec = timer.toc_tic(); log_time(n_micro_sec, "Read    - op took");
	my_assert(lib->close_bufs_unregist(bdev, io_bufs) == gusli::connect_rv::C_OK);
	n_micro_sec = timer.toc();     log_time(n_micro_sec, "UnRegist-mem took");
	//__verify_mapped_properly(io_bufs);
	free(io_bufs[0].ptr);
}

/*****************************************************************************/
#include <getopt.h>
int main(int argc, char *argv[]) {
	int opt, num_ios_preassure = (1 << 23), n_iter_race_tests = 10000;
	int do_large_io_test = true;
	while ((opt = getopt(argc, argv, "n:c:l:h")) != -1) {
		switch (opt) {
			case 'n': num_ios_preassure = std::stoi(  optarg); break;
			case 'c': n_iter_race_tests = std::stoi(  optarg); break;
			case 'l': do_large_io_test =  std::stoi(  optarg); break;
			case 'h':
			default:
				log_unitest("Usage: %s [-n num_ios_preassure] [-c n_iter_race_tests] [-l do_large_io_test] [-h]\n", argv[0]);
				log_unitest("  -n num_ios_preassure, (default: %d)\n", num_ios_preassure);
				log_unitest("  -c n_iter_race_tests, (default: %d)\n", n_iter_race_tests);
				log_unitest("  -l 1/0, (default: %d)\n", do_large_io_test);
				log_unitest("  -h                    Show this help message\n");
				return (opt == 'h') ? 0 : 1;
		}
	}
	{
		char thread_name[32];
		snprintf(thread_name, sizeof(thread_name), "%sunit", gusli::thread_names_prefix);
		(void)pthread_setname_np(pthread_self(), thread_name);
	}
	gusli::global_clnt_context* lib = lib_initialize_unitests();
	base_lib_empty_io_unitest();
	unitest_auto_open_close(lib);
	client_server_stuck_io_and_throttle_test(*lib);
	client_stuck_io_and_throttle_tests(*lib, UUID.AUTO_STUCK);
	if (do_large_io_test) unitest_huge_mem_map_and_io(lib);
	base_lib_unitests(*lib, n_iter_race_tests);
	client_no_server_reply_test(*lib);
	client_server_basic_test(*lib, num_ios_preassure);
	delete lib;
	log_unitest("Done!!! Success\n\n\n");
}
