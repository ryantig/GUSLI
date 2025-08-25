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
#include <unistd.h>  // for fork()
#include <sys/wait.h>

/*****************************************************************************/
static inline int32_t get_connected_bdev_descriptor(gusli::global_clnt_context& lib, const gusli::backend_bdev_id bdev) {
	gusli::bdev_info i;
	my_assert(lib.bdev_get_info(bdev, i) == gusli::connect_rv::C_OK);
	log_unitest("\tioable: {bdev uuid=%.16s, fd=%d name=%s, block_size=%u[B], n_blocks=0x%lx}\n", bdev.uuid, i.bdev_descriptor, i.name, i.block_size, i.num_total_blocks);
	return i.bdev_descriptor;
}

class servers_list {
	union server_process {
		pthread_t tid;								// Thread  id when server is lauched as thread
		__pid_t   pid;								// Process id when server is lauched as process via fork()
	};
	static constexpr bool launch_server_as_process = true;
	const char* const *names;						// Names of servers, for debug prints
	std::vector<server_process> arr;

	void clnt_connect_to_servers(gusli::global_clnt_context& lib, const char* const uuids[]) {
		gusli::backend_bdev_id bdev; // Connect to all servers, important not to do this 1 by 1, to test multiple bdevs
		using gc = gusli::connect_rv;
		for (size_t s = 0; s < arr.size(); ++s) {
			bdev.set_from(uuids[s]);
			gusli::bdev_info info;
			int n_attempts = 0;
			auto con_rv = gc::C_NO_RESPONSE;
			for (; ((con_rv == gc::C_NO_RESPONSE) && (n_attempts < 10)); n_attempts++ ) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));	// Wait for servers to be up
				con_rv = lib.bdev_connect(bdev);
			}
			my_assert(con_rv == gc::C_OK);
			get_connected_bdev_descriptor(lib, bdev);
			my_assert(lib.bdev_get_info(bdev, info) == gc::C_OK);
			my_assert(strstr(info.name, names[s]) != NULL);				// Server name is correct
		}
		bdev.set_from(uuids[0]);
		std::string msg = "  \t  print_clnt_msg";				// With non visible prefix that should be removed
		my_assert(lib.bdev_ctl_log_msg(bdev, msg) == gc::C_OK);	// Send this debug message to server log
	}

 public:
	servers_list(gusli::global_clnt_context& lib, const char* const uuids[], const char* const _names[], const char* reason, int n_servers, void (*fn)(int idx)) :
		names(_names) {
		log_line("Remote %d server[%s] launch", n_servers, reason);
		arr.resize(n_servers);
		for (int i = 0; i < n_servers; i++) {
			arr[i].pid = fork();
			my_assert(arr[i].pid >= 0);
			if (arr[i].pid == 0) {	// Child process (Server)
				fn(i);
				exit(0);
			}
		}
		clnt_connect_to_servers(lib, uuids);
	}

	void wait_for_all(void) {
		if (launch_server_as_process) {
			for (size_t i = 0; i < arr.size(); ++i)
				wait_for_process(arr[i].pid, names[i]);
		} else {
			for (size_t i = 0; i < arr.size(); ++i) {
				const int err = pthread_join(arr[i].tid, NULL);
				my_assert(err == 0);
			}
		}
	}
};
