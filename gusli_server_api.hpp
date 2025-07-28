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
#include "gusli_client_api.hpp"	// for struct bdev_info / io_request

namespace gusli {

class server_io_req : public io_request {			// Data structure for processing incomming IO from gusli client
 public:											// Note this is the same as base class, just add functions for the executor of the io
	server_io_req() {}
	SYMBOL_EXPORT bool is_valid(void) const { return (_exec == nullptr) && params.is_valid(); }		// Server use this to verify incomming io from client is in valid state
	SYMBOL_EXPORT void start_execution(void) { out.rv = io_error_codes::E_IN_TRANSFER; }
	SYMBOL_EXPORT int64_t get_raw_rv(void) const { return out.rv; }
	SYMBOL_EXPORT const io_multi_map_t* get_multi_map(void) const { return (const io_multi_map_t*)params.map.data.ptr; }
	SYMBOL_EXPORT void** get_private_exec_u64_addr(void) { return (void**)&this->_exec; }
	SYMBOL_EXPORT void set_error(enum io_error_codes err) {
		const bool do_cb = has_callback();
		out.rv = (int64_t)err;							// After this line 'this' may get free, if no callback
		if (do_cb) params._comp_cb(params._comp_ctx);	// Here consider 'this' as destroyed
	}
	SYMBOL_EXPORT void set_success(uint64_t n_done_bytes) {
		const bool do_cb = has_callback();
		out.rv = (n_done_bytes == params.buf_size()) ? n_done_bytes : (int64_t)io_error_codes::E_BACKEND_FAULT;	// Partial io not supported
		if (do_cb) params._comp_cb(params._comp_ctx);
	}
};

class srvr_backend_bdev_api {						// Implement (derive from this class privetly) for backend of block device
 public:
	/* Backend API towards GUSLI, implement functions/params below  */
	static constexpr const int BREAKING_VERSION = 1;	// Hopefully will always be 1. When braking API change is introduced, this version goes up so apps which link with the library can detect that during compilation
	struct init_params {							// Most params are optional, initialize params before calling the below create..() function
		char listen_address[32];					// Mandatory, server will bind to this address, client will connect to it
		FILE* log = stderr;							// Redirect logs of the library to this file (must be already properly opened)
		const char* server_name = "";				// For debug, server identifier
		bool has_external_polling_loop = false;		// If 'true' like s*pdk framework, run() will do only 1 iteration and user is responsible to call it in a loop
		bool use_blocking_client_accept = true;		// If client is not connected, server has nothing to do so it may block. If you implement external polling loop, consider setting this to false
	} par;
	virtual bdev_info open1(const char* debug_reason) = 0;
	virtual int      close1(const char* who) = 0;
	virtual void    exec_io(server_io_req& io) = 0;	// use server_io_req methods to execute the io and set result

 protected:
	SYMBOL_EXPORT ~srvr_backend_bdev_api() noexcept;// Cleans up 'impl'
	/* Gusli API towards your class, USE this API to initialize/User the server */
	SYMBOL_EXPORT const char *create_and_get_metadata_json();	// Callfrom your derived class constructor. Initializes 'impl'. Upon error throws exception. Get the version of the library to adapt application dynamically to library features set.
	SYMBOL_EXPORT_NO_DISCARD int run(void) noexcept; // Main server loop. Returns < 0 upon error, 0 - may continue to run the loop, >0 - successfull server exit
 private:
	static constexpr const char* metadata_json_format = "{\"%s\":{\"version\" : \"%s\", \"commit\" : \"%lx\", \"optimization\" : \"%s\", \"trace_level\" : %u, \"Build\" : \"%s\"}}";
	class global_srvr_context_imp *impl = nullptr;		// Actual implementation of the gusli engine, dont touch
};

} // namespace gusli