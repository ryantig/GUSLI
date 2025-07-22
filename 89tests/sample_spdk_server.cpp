#ifdef SUPPORT_SPDK
#include "../07examples/server/spdk_app_1server.hpp"
#else
	#include "07examples/client/io_submittion_example.hpp"
	class empty_app {
	 public:
		empty_app(const int argc, const char *argv[]) { (void)argc; (void)argv; }
		int get_rv(void) const {
			log("\x1b[1;31mCould not find spdk framework, spdk server unitest abort, rv=%d\x1b[0;0m\n", -ENOEXEC);
			return -ENOEXEC;
		}
	};
	typedef class empty_app server_spdk_app_1_gusli_server;
#endif

int main(int argc, const char **argv) {
	server_spdk_app_1_gusli_server app(argc, argv);
	return app.get_rv();
}
