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
#include "../07examples/server/spdk_app_1server.hpp"
#include "../07examples/client/sample_client.hpp"
#include <unistd.h>  // for fork()
#include <sys/wait.h>
static int run_server(int argc, const char **argv) {
	server_spdk_app_1_gusli_server app(argc, argv);
	return app.get_rv();
}

static int run_client(__pid_t pid) {
	#ifdef SUPPORT_SPDK
		std::this_thread::sleep_for(std::chrono::milliseconds(100));	// Wait for servers to be up and ready to acept client
		const int rv = client_simple_test_of_server("[GS_Clnt_]", spdk_srvr_bdev0_uuid, spdk_srvr_listen_address);
		if (pid)
			wait_for_process(pid, "client_wait_for_server_done");
		return rv;
	#else
		log_uni_failure("Client: Could not find spdk framework, spdk client unitest abort, rv=%d\n", -ENOEXEC);
		return ENOEXEC;
	#endif
}

int main(int argc, const char **argv) {
	if (strcmp(argv[argc-1], "@c") == 0) {			// Last param says launch only client
		log_line("!!! Important: Launch only client !!!");
		return run_client(0);						// Run client, dont wait for server
	} else if (strcmp(argv[argc-1], "@s") == 0) {	// Last param says launch only server
		log_line("!!! Important: Launch only Server !!!");
		return run_server(argc - 1, argv);			// Remove last param to not mess with spdk/dpdk params
	}
	const __pid_t pid = fork();						// Launch both via fork
	my_assert(pid >= 0);
	if (pid == 0) {	// Child process
		run_server(argc, argv);
	} else {
		return run_client(pid);						// Wait for server to finish
	}
}
