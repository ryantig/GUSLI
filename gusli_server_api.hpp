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

class server_io_req : public io_request {			// Data structure for processing incomming IO from gusli client
public:											// Note this is the same as base class, just add functions for the executor of the io
	server_io_req() {}
	SYMBOL_EXPORT bool is_valid(void) const { return (_exec == nullptr); }
	SYMBOL_EXPORT void start_execution(void) { out.rv = io_error_codes::E_IN_TRANSFER; }
	SYMBOL_EXPORT int64_t get_raw_rv(void) const { return out.rv; }
	SYMBOL_EXPORT const io_multi_map_t* get_multi_map(void) const { return (const io_multi_map_t*)params.map.data.ptr; }
	SYMBOL_EXPORT void** get_private_exec_u64_addr(void) { return (void**)&this->_exec; }
	SYMBOL_EXPORT void set_error(enum io_error_codes err) {
		const bool do_cb = has_callback();
		out.rv = (int64_t)err;							// After this line 'this' may get free, if no callback
		if (do_cb) params._comp_cb(params._comp_ctx);	// Here consider 'this' as destroyed
	}
	SYMBOL_EXPORT void set_success( uint64_t n_done_bytes) {
		const bool do_cb = has_callback();
		out.rv = (n_done_bytes == params.buf_size()) ? n_done_bytes : (int64_t)io_error_codes::E_BACKEND_FAULT;	// Partial io not supported
		if (do_cb) params._comp_cb(params._comp_ctx);
	}
};

class global_srvr_context : no_implicit_constructors {	// Singletone: Library context
 protected: global_srvr_context() = default;
 public:
	struct init_params {							// Most params are optional
		char listen_address[32];					// Mandatory
		FILE* log = stderr;							// Redirect logs of the library to this file (must be already properly opened)
		const char* server_name = NULL;				// For debug, server identifier
		bool has_external_polling_loop = false;		// If 'true' like s*pdk framework, run() will do only 1 iteration and user is responsible to call it in a loop
		bool use_blocking_client_accept = true;		// If client is not connected, server has nothing to do so it may block. If you implement external polling loop, consider setting this to false
		struct srvr_backend_callbacks {
			void *caller_context;
			bdev_info (*open1)(void *caller_context, const char* who) = 0;
			int      (*close1)(void *caller_context, const char* who) = 0;
			void    (*exec_io)(void *caller_context, server_io_req& io) = 0;		// use server_io_req methods to execute the io and set result
		} vfuncs;
	};
	static constexpr const int BREAKING_VERSION = 1;					// Hopefully will always be 1. When braking API change is introduced, this version goes up so apps which link with the library can detect that during compilation
	SYMBOL_EXPORT static global_srvr_context& get(void) noexcept; 				// Singletone
	SYMBOL_EXPORT [[nodiscard]] int init(const init_params& par) noexcept;		// Must be called first, returns negative on error, 0 or positive on success
	SYMBOL_EXPORT [[nodiscard]] int run(void) noexcept;							// Main server loop. Returns < 0 upon error, 0 - may continue to run the loop, >0 - successfull server exit
	SYMBOL_EXPORT [[nodiscard]] int destroy(void) noexcept;						// Must be called last,  returns negative on error, 0 or positive on success
};

/******************************** RAII API ***********************/
// High level more C++ API, Construct which throws exception
class global_srvr_raii : no_implicit_constructors {
public:
	static constexpr const int BREAKING_VERSION = 1;					// Hopefully will always be 1. When braking API change is introduced, this version goes up so apps which link with the library can detect that during compilation
	SYMBOL_EXPORT global_srvr_raii(const global_srvr_context::init_params& par) {
		auto& instance = global_srvr_context::get();
		if (instance.init(par) < 0)
			throw std::runtime_error("Failed to initialize gusli server");
	}
	SYMBOL_EXPORT ~global_srvr_raii() noexcept { (void)global_srvr_context::get().destroy(); }
	SYMBOL_EXPORT [[nodiscard]] int run(void) noexcept { return global_srvr_context::get().run(); }
};

/******************************** io zero copy ***********************/

} // namespace gusli