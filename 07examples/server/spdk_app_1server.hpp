#pragma once
#include "spdk_server_ram.hpp"

static volatile bool ctrl_c_pressed = false;
class server_spdk_app_1_gusli_server {
 	std::unique_ptr<server_spdk_ram> srvr;
	struct spdk_app_opts opts = {};
	struct spdk_poller *gs_poller = NULL;
	int app_start_rv = -ENOEXEC;
	int run_rv = -ENOEXEC;
	int parse_opts(const int orig_argc, const char *orig_argv[]) {
		int argc = orig_argc;
		const char *argv[16];
		my_assert(argc <= 16);
		memcpy(argv, orig_argv, sizeof(char*)*orig_argc);
		argv[argc++] = "-c";
		argv[argc++] = "./07examples/server/spdk_bdev.conf";
		argv[argc++] = "-b";
		argv[argc++] = backend_dev_t::default_bdev_name;
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
		}
	}
	static int run_g_server_once(void *arg) {
		class server_spdk_app_1_gusli_server* me = (server_spdk_app_1_gusli_server*)arg;
		me->run_rv = me->srvr->run_once();
		return SPDK_POLLER_BUSY;  // Run at full rate to see the output clearly
	}
	void app_source_code(void) {
		const uint64_t period_us = 0;  // Fastest polling frequency
		signal(SIGINT, signal_handler);
		gs_poller = spdk_poller_register(run_g_server_once, (void*)this, period_us);
		if (gs_poller == NULL) {
			SPDK_ERRLOG("Failed to register background poller at %lu[usec]\n", period_us);
			return;
		}
		for (run_rv = 0; (!ctrl_c_pressed) && (run_rv == 0); ) {
			// Here you can have multiple servers/bdevs run your own code
			spdk_thread_poll(spdk_get_thread(), 0, 0);		// Allow server poller to run
		}
		spdk_poller_unregister(&gs_poller);
		spdk_app_stop((run_rv < 0) ? -1 : 0);
	}
	static void app_source_code_static(void *arg1) { ((server_spdk_app_1_gusli_server*)arg1)->app_source_code(); }
 public:
	server_spdk_app_1_gusli_server(const int argc, const char *argv[]) {
		srvr = std::make_unique<server_spdk_ram>(backend_dev_t::default_bdev_name, spdk_srvr_listen_addre);
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
		}
	}
	int get_rv(void) const { return run_rv; }
};
