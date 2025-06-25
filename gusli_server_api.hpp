#pragma once
#include <stdio.h>			// FILE, printf
#include <string.h>			// memset, memcmp
#include <stdint.h>			// uint64_t, uint32_t and such
#include "gusli_client_api.hpp"	// for struct bdev_info

namespace gusli {
class global_srvr_context : no_implicit_constructors {	// Singletone: Library context
 protected: global_srvr_context() = default;
 public:
	struct init_params {							// All params are optional
		FILE* log = stderr;							// Redirect logs of the library to this file (must be already properly opened)
		struct bdev_info binfo;
		struct srvr_backend_callbacks {
			void *caller_context;
			int  (*open)(   void *caller_context, const char* who) = 0;
			int  (*close)(  void *caller_context, const char* who) = 0;
			void (*exec_io)(void *caller_context, class io_request& io);
		} vfuncs;
	};
	SYMBOL_EXPORT static global_srvr_context& get(void); 			// Singletone
	SYMBOL_EXPORT int  run(const struct init_params& par);				// Must be called first
};

/******************************** io zero copy ***********************/

} // namespace gusli