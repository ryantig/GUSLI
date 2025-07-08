#pragma once
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

#include <stdio.h>			// FILE, printf
#include <string.h>			// memset, memcmp
#include <stdint.h>			// uint64_t, uint32_t and such
#include "gusli_client_api.hpp"	// for struct bdev_info

namespace gusli {
class global_srvr_context : no_implicit_constructors {	// Singletone: Library context
 protected: global_srvr_context() = default;
 public:
	struct init_params {							// Most params are optional
		char listen_address[32];					// Mandatory
		FILE* log = stderr;							// Redirect logs of the library to this file (must be already properly opened)
		struct bdev_info binfo;
		struct srvr_backend_callbacks {
			void *caller_context;
			int  (*open)(   void *caller_context, const char* who) = 0;
			int  (*close)(  void *caller_context, const char* who) = 0;
			int (*exec_io)( void *caller_context, class io_request& io);		// negative on failure, 0 or positive on success
		} vfuncs;
	};
	SYMBOL_EXPORT static global_srvr_context& get(void); 						// Singletone
	SYMBOL_EXPORT [[nodiscard]] int run(const struct init_params& par);			// Must be called first
};

/******************************** io zero copy ***********************/

} // namespace gusli