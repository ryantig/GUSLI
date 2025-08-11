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

#include "gusli_client_api.hpp"						// for struct bdev_info / io_request
#include <atomic>

namespace gusli {

class backend_io_req : public io_request {						// Data structure for processing incomming IO from gusli client
	void set_rv(int64_t result) {
		const bool do_cb = params.has_callback();				// Save param on stack, because this might get free
		std::atomic_thread_fence(std::memory_order_release);	// Ensure all the needed parts of 'this' were copied to stack, before setting the result
		out.rv = result;										// After this line 'this' may get free, if no completion callback is registered
		if (do_cb) params._comp_cb(params._comp_ctx);			// Here consider 'this' as destroyed
	}
 public:											// Note this is the same as base class, just add functions for the executor of the io
	SYMBOL_EXPORT const io_multi_map_t* get_multi_map(void) const { return (const io_multi_map_t*)params.map().data.ptr; }
	SYMBOL_EXPORT void** get_private_exec_u64_addr(void) { return (void**)&this->_exec; }			// Use this to attach your own executor to the io, if needed
	SYMBOL_EXPORT void set_error(enum io_error_codes err) { set_rv((int64_t)err); }
	SYMBOL_EXPORT void set_success(uint64_t n_done_bytes) {
		const int64_t err = (n_done_bytes == params.buf_size()) ? (int64_t)n_done_bytes : (int64_t)io_error_codes::E_BACKEND_FAULT;	// Partial io not supported
		set_rv(err);
	}
};

class srvr_backend_bdev_api : no_implicit_constructors {		// Implement (derive from this class privetly) for backend of block device
	// Note: Copy/Move-Cosntructor/Assignmet-op is disabled. gusli will use your derive class by pointer. Your derive class can be wrapped as unique_ptr<> but putting it into vector is deliberatly disabled
 public:
	/* Backend API towards GUSLI: implement (in your derived class) the functions & params initialization below */
	static constexpr const int BREAKING_VERSION = 1;			// Hopefully will always be 1. When braking API change is introduced, this version goes up so apps which link with the library can detect that during compilation
	struct init_params {										// Most params are optional, initialize params in your constructor before calling the below create..() function
		char listen_address[32];								// Mandatory, server will bind to this address, client will connect to it
		FILE* log = stderr;										// Redirect logs of the library to this file (must be already properly opened)
		const char* server_name = "";							// For debug, server identifier
		bool has_external_polling_loop = false;					// If 'true' like s*pdk framework, run() will do only 1 iteration and user is responsible to call it in a loop
		bool use_blocking_client_accept = true;					// If client is not connected, server has nothing to do so it may block. If you implement external polling loop, consider setting this to false
	} par;
	virtual bdev_info open1(const char* reason) noexcept = 0;	// When client first connect to block device this function is called. Result will be forwarded to the client. On error make result invalid
	virtual int      close1(const char* reason) noexcept = 0;	// When client disconnects from bdev this function is called. Return value is meaningless for now
	virtual void    exec_io(backend_io_req& io) noexcept = 0;	// use io methods to execute the io and set result. Reply must be done in the same thread which called this function

 protected:
	SYMBOL_EXPORT srvr_backend_bdev_api() noexcept : impl(nullptr) {};
	SYMBOL_EXPORT ~srvr_backend_bdev_api() noexcept;// Cleans up 'impl'
	/* Gusli API towards your class, USE this API to initialize/User the server */
	SYMBOL_EXPORT const char *create_and_get_metadata_json(void);// Call from your derived class constructor. Initializes 'impl'. Upon error throws exception. Get the version of the library to adapt application dynamically to library features set.
	SYMBOL_EXPORT int set_thread_name(char (&out_name)[32], const char your_prefix[8]) const noexcept;	// Optional: Build a server name and change current thread to this name
	SYMBOL_EXPORT_NO_DISCARD int run(void) const noexcept;		// Main server loop. Returns < 0 upon error, 0 - may continue to run the loop, >0 - successfull server exit
 private:
	static constexpr const char* metadata_json_format = "{\"%s\":{\"version\" : \"%s\", \"commit\" : \"%lx\", \"optimization\" : \"%s\", \"trace_level\" : %u, \"Build\" : \"%s\"}}";
	class srvr_imp *impl;										// Actual implementation of the gusli engine, dont touch
};

} // namespace gusli