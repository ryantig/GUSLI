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
#ifdef SUPPORT_SPDK
#include "spdk_server_ram.hpp"

static volatile bool ctrl_c_pressed = false;
static volatile bool shutdown_in_progress = false;

class server_spdk_app_1_gusli_server {
 	std::unique_ptr<server_spdk_ram> srvrs[spdk_test::num_bdevs];
	struct spdk_app_opts opts = {};
	struct spdk_poller *gs_poller = NULL;
	int app_start_rv = -ENOEXEC;
	int srvrs_rv[spdk_test::num_bdevs];
	int run_rv = -ENOEXEC;
	int parse_opts(const int orig_argc, const char *orig_argv[]) {
		int argc = orig_argc;
		const char *argv[16];
		my_assert(argc <= 16);
		memcpy(argv, orig_argv, sizeof(char*)*orig_argc);
		argv[argc++] = "-c";
		argv[argc++] = "./07examples/server/spdk_bdev.conf";
		//argv[argc++] = "-b";
		//argv[argc++] = .....;
		spdk_app_opts_init(&opts, sizeof(opts));
		opts.name = "sample_app";
		opts.rpc_addr = NULL;
		const int rv = spdk_app_parse_args(argc, (char**)argv, &opts, "b:", NULL, backend_dev_t::parse_arg, backend_dev_t::usage_help);
		return (rv == SPDK_APP_PARSE_ARGS_SUCCESS) ? 0 : -1;
	}
	static void signal_handler(int signal) {
		if (signal == SIGINT) {
			SPDK_NOTICELOG("Ctrl+C received, initiating shutdown...\n");
			ctrl_c_pressed = true;
			if (!shutdown_in_progress) {
				shutdown_in_progress = true;
				SPDK_NOTICELOG("Forcing SPDK application stop...\n");
				spdk_app_stop(0);			// This ensures SPDK will exit even if the event loop is stuck
			} else {
				SPDK_ERRLOG("Second Ctrl+C received, forcing immediate exit!\n");	// If we're already shutting down and get another Ctrl+C,force exit immediately
				exit(1);
			}
		}
	}
	inline int run_g_server_once(void) {
		run_rv = ~0U;
		for (int i = 0; i < spdk_test::num_bdevs; i++) {
			srvrs_rv[i] = srvrs[i]->run_once();
			run_rv &= srvrs_rv[i];					// Continue running as long as at least 1 server needs
		}
		// SPDK_NOTICELOG("1-run ctr+c=%u run_rv={%d:%d/%d}\n", ctrl_c_pressed, run_rv, srvrs_rv[0], srvrs_rv[1]);
		return SPDK_POLLER_BUSY;					// Run at full rate to see the output clearly
	}
	static int run_g_server_once_static(void *arg) { return ((server_spdk_app_1_gusli_server*)arg)->run_g_server_once(); }
	void app_source_code(void) {
		const uint64_t period_us = 0;				// Fastest polling frequency
		signal(SIGINT, signal_handler);
		signal(SIGTERM, signal_handler);			// Also handle SIGTERM for graceful shutdown
		gs_poller = spdk_poller_register(run_g_server_once_static, (void*)this, period_us);
		if (gs_poller == NULL) {
			SPDK_ERRLOG("Failed to register background poller at %lu[usec]\n", period_us);
			return;
		}
		for (run_rv = 0; (!ctrl_c_pressed) && (run_rv == 0); ) {
			// Here you can have multiple servers/bdevs run your own code
			spdk_thread_poll(spdk_get_thread(), 0, 0);		// Allow server poller to run
		}
		// SPDK_NOTICELOG("Main loop exited, cleaning up...\n");
		spdk_poller_unregister(&gs_poller);
		if (!shutdown_in_progress) {				// If we're here due to normal exit (not Ctrl+C), stop the app
			spdk_app_stop((run_rv < 0) ? -1 : 0);
		}
	}
	static void app_source_code_static(void *arg1) { ((server_spdk_app_1_gusli_server*)arg1)->app_source_code(); }
 public:
	server_spdk_app_1_gusli_server(const int argc, const char *argv[]) {
		for (int i = 0; i < spdk_test::num_bdevs; i++)
			srvrs[i] = std::make_unique<server_spdk_ram>(spdk_test::bdev_name[i], spdk_test::srvr_listen_address[i]);
		const int parse_rv = parse_opts(argc, argv);
		if (parse_rv != 0) {
			SPDK_ERRLOG("Error parsing args\n");
			return;
		}
		app_start_rv = spdk_app_start(&opts, app_source_code_static, this);
		if (app_start_rv) {
			SPDK_ERRLOG("ERROR starting application rv=%d\n", app_start_rv);
			return;
		} else {
			SPDK_NOTICELOG("app finished, gs_rv=%d %s\n", run_rv, (ctrl_c_pressed ? ",Ctr+C pressed" : ""));
			spdk_app_fini();
			log_uni_success("Server: Test OK\n");
		}
	}
	int get_rv(void) const { return run_rv; }
};

#else	// spdk support
	#include "gusli_server_api.hpp"
	#include "../common.hpp"
	class server_spdk_app_1_gusli_server {
	 public:
		server_spdk_app_1_gusli_server(const int argc, const char *argv[]) { (void)argc; (void)argv; }
		int get_rv(void) const {
			log_uni_failure("Server: Could not find spdk framework, spdk server unitest abort, rv=%d\n", -ENOEXEC);
			return ENOEXEC;
		}
	};
#endif
