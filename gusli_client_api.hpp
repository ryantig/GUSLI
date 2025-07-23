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
/* Gusli client: Block IO submition api */
#include <stdio.h>			// FILE, printf
#include <string.h>			// memset, memcmp
#include <stdint.h>			// uint64_t, uint32_t
#include <vector>
#include <stdexcept>

#define SYMBOL_EXPORT __attribute__((__visibility__("default")))
namespace gusli {

/******************************** block device reflection ***********************/
// Note: Library does not manage local block devices, it assumes they exist and library can connect to them. Like local disks
struct backend_bdev_id {					// Unique ID of volume
	char uuid[16];							// Deliberatly no dependency on uuid.lib
	friend bool operator==(const backend_bdev_id& a, const backend_bdev_id& b) noexcept {
		return (memcmp(a.uuid, b.uuid, sizeof(a.uuid)) == 0);
	}
	struct backend_bdev_id *set_invalid(void) { memset(uuid, 0, sizeof(uuid)); return this; }
	void set_from(const char* str) { set_invalid(); const size_t nc = std::min(strlen(str) + 1, sizeof(uuid)); memcpy(uuid, str, nc); }
	void set_from(const uint64_t uid) { set_invalid(); snprintf(uuid, sizeof(uuid), "%lu", uid); }
} __attribute__((aligned(sizeof(long))));

struct bdev_info {							// After connection to bdev established, this info can be retrieved
	char name[32];							// Server self reported name, used for logging
	int32_t  bdev_descriptor;				// Much like file descriptor, Used to access this bdev in datapath
	uint32_t block_size;					// bytes units. Minimal unit for IO (size and alignment). Typically 4[KB]..16[MB]
	uint64_t num_total_blocks;				// Number of blocks accessible for IO. Can be extended in runtime, never shrinked.
	uint32_t num_max_inflight_io;			// QOS. More than this amount will be throttled by the client side
	uint32_t reserved;
	void clear(void) { memset(this, 0, sizeof(*this)); bdev_descriptor = -1; }
	bool is_valid(void) const { return (block_size > 0) && (bdev_descriptor > 0); }
} __attribute__((aligned(sizeof(long))));

/******************************** io request context ***********************/
enum io_type {									// Request type
	G_NOP = '0',								// Dummy op, for testing and integration. Never passed to backend
	G_READ = 'R',
	G_WRITE = 'W',
};

enum io_error_codes {							// Exhaustive list of error codes
	E_OK = 0, 									// Op success
	E_IN_TRANSFER = 			 -1,			// IO is still in execution. Returned when error code is tested too early (before IO completed).
	E_INTERNAL_FAULT = 			-11,			// Consult with developers. Internal library problem
	E_BACKEND_FAULT = 			-15,			// Underlying block device could not execute the io
	E_CANCELED_BY_CALLER = 		-10,			// IO Operation was explicitly canceled by caller.
	E_THROTTLE_RETRY_LATER = 	-12,			// IO Operation cannot be submittedd
	E_PERM_FAIL_NO_RETRY = 		-13,			// IO Operation, Probably data loss, cannot execute request and no point in retrying it
	E_INVAL_PARAMS = 			-14,			// Invalid parameters/op/block-device for execution
};

struct io_buffer_t {							// For fast datapath, pre-registered io_buffers, same as 'struct iovec'
	/*volatile*/ void *ptr;						// Pointer to memory accessible by application and backend bdev, Source buffer for write, destination for read.
	uint64_t byte_len;							// Length of the buffer
	bool is_valid_for(uint64_t block_size) const {
		const uint64_t align = (byte_len | (uint64_t)ptr);
		return !((align % block_size) || (align % 4096) || (ptr == nullptr) || (byte_len == 0));	// Kernel pages/bdev-blocks can be larger. 512[B] not supported
	}
} __attribute__((aligned(sizeof(long))));

struct io_map_t {								// IO mapping of data buffer to block device lba
	io_buffer_t data;
	uint64_t offset_lba_bytes;					// Starting offset of the io on blockdevice, all address must be aligned to blocks.
	void init(void* ptr, uint64_t len, uint64_t offset) { data.ptr = ptr; data.byte_len = len; offset_lba_bytes = offset; }
	bool is_valid_for(uint64_t block_size) const { return data.is_valid_for(block_size) && ((offset_lba_bytes % block_size) == 0); }
	uint64_t get_offset_end_lba(void) const { return (offset_lba_bytes + data.byte_len); }
} __attribute__((aligned(sizeof(long))));

struct io_multi_map_t {							// Scatter gather list of multi range io, 8[b] header followed by array of entries.
	uint32_t n_entries;							// Number of elements in the array > 1.
	uint32_t reserved;
	io_map_t entries[0];
	uint64_t  my_size(void) const { return sizeof(*this) + n_entries*sizeof(entries[0]); }
	uint64_t buf_size(void) const { uint64_t rv = 0; for (uint32_t i = 0; i < n_entries; i++) rv += entries[i].data.byte_len; return rv; }
	bool is_valid(void) const { return (n_entries > 1); } // For 0,1 range, multi map is not needed
} __attribute__((aligned(sizeof(long))));

class io_request {								// Data structure for issuing IO
 public:
	struct params_t {							// Parameters for IO, treated as const by the library
		io_map_t map;							// Holds a mapping for IO buffer or a mapping to scatter-gather list of mappings
		int32_t bdev_descriptor;				// Take from bdev_info::bdev_descriptor
		//uint32_t timeout_msec;				// Optional timeout for IO in [msec].  0 or ~0 mean infinity. Not supported, Use async io mode and cancel the io if needed
		enum io_type op : 8;					// Operation to be performed
		uint8_t priority: 8;					// [0..100] priority. 100 is highest, 0 lowest
		uint8_t is_mutable_data : 1;			// Set to true if content of io buffer might be changed by caller while IO is in air. If cannot guarantee immutability io will suffer a penalty of internal copy of the buffer
		uint8_t assume_safe_io : 1;				// Set to true if caller verifies correctness of io (fully inside mapped area, etc...), Skips internal checks so IO uses less client side CPU
		uint8_t try_using_uring_api : 1;		// If possible use uring api, more efficient for io's with large amount of ranges
	 // private:								// Fields below are set implicitly, dont touch them directly
		uint8_t _has_mm : 1;					// First 4K of io buffer contains io_multi_map_t (scatter gather) description for multi-io
		uint8_t _async_no_comp : 1;				// Internal flag, IO is async but caller will poll it instead of completion
		void (*_comp_cb)(void* ctx);		// Completion callback, Called from library internal thread, dont do processing/wait-for-locks in this context!
		void *_comp_ctx;				// Callers Completion context passed to the function above
	 public:
		template<class C, typename F> void set_completion(C ctx, F cb) { _comp_ctx = (void*)ctx, _comp_cb = (void (*)(void*))cb; _async_no_comp = false; }
									  void set_blocking(void) {          _comp_ctx = NULL;       _comp_cb = NULL;                _async_no_comp = false; }
									  void set_async_pollable(void) {    _comp_ctx = NULL;       _comp_cb = NULL;                _async_no_comp = true; }
		void init_1_rng(enum io_type _op, int id, uint64_t lba, uint64_t len, void *buf) { _has_mm = 0; op = _op; bdev_descriptor = id; map.init(buf, len, lba); }
		void init_multi(enum io_type _op, int id,  const io_multi_map_t& mm) {             _has_mm = 1; op = _op; bdev_descriptor = id; map.init((void*)&mm, mm.my_size(), 0); }
		uint64_t buf_size(void) const {   return (_has_mm ? ((const io_multi_map_t*)map.data.ptr)->buf_size() : map.data.byte_len); }
		uint32_t num_ranges(void) const { return (_has_mm ? ((const io_multi_map_t*)map.data.ptr)->n_entries  : 1); }
		const class io_request *my_io_req(void) const { return (io_request*)this; }
	} params;
	io_request() { memset(this, 0, sizeof(*this)); }
	bool has_callback(void) const { return (params._comp_cb != NULL); }
	SYMBOL_EXPORT void submit_io(void) noexcept;						// Execute io. May Call again to retry failed io. All errors/success should be checked with function below
	SYMBOL_EXPORT enum io_error_codes get_error(void) noexcept;			// Query io completion status for blocking IO, poll on pollable io. Runnyng on async callback io may yield racy results
	enum cancel_rv { G_CANCELED = 'V', G_ALLREADY_DONE = 'D' };			// DONE = IO finished error/success. CANCELED = Successfully canceled (Async IO, completion will not be executed)
	SYMBOL_EXPORT [[nodiscard]] enum cancel_rv try_cancel(void) noexcept;	// Cancel asynchronous I/O request. For Async IO, completion will not arrive after call to this function, but uncareful user may call it while completion callback is concurently running
 protected:																// Below extra 16[b] for execution state
	class io_request_executor_base* _exec;								// During execution executor attaches to IO, Server side uses it to execute io
 	struct output_t { int64_t rv; } out;								// Negative error code or amount of bytes transferred.
	bool is_blocking_io(void) const { return !has_callback() && !params._async_no_comp; }
private:
	[[nodiscard]] io_request_executor_base* __disconnect_executor_atomic(void) noexcept;	// Internal function, dont touch
};

/******************************** Global library context ***********************/
// API below is more advanced and suitable for more precise manipulation with library singletone
enum connect_rv { C_OK = 0, C_NO_DEVICE = -100,
	C_NO_RESPONSE /* Device exists no response from backend*/,
	C_REMAINS_OPEN /* Already open / Cannot close because in use*/,
	C_WRONG_ARGUMENTS};

struct no_implicit_constructors {	// No copy/move/assign operations to prevent accidental copying of resources, leaks, double free and performance degradation
	no_implicit_constructors(           const no_implicit_constructors& ) = delete;
	no_implicit_constructors(                 no_implicit_constructors&&) = delete;
	no_implicit_constructors& operator=(const no_implicit_constructors& ) = delete;
	no_implicit_constructors& operator=(      no_implicit_constructors&&) = delete;
	no_implicit_constructors() {}
};

class global_clnt_context : no_implicit_constructors {					// Singletone: Library context
 protected: global_clnt_context() = default;
	static constexpr const char* metadata_json_format = "{\"%s\":{\"version\" : \"%s\", \"commit\" : \"%lx\", \"breaking_api_version\" : %u}}";
 public:
	struct init_params {							// All params are optional
		FILE* log = stdout;							// Redirect logs of the library to this file (must be already properly opened)
		const char* config_file = NULL;				// Path to config file or content of config file starting with "# version=".
		const char* client_name = NULL;				// For debug, client identifier
		unsigned int max_num_simultaneous_requests = 256;
	};
	SYMBOL_EXPORT static global_clnt_context& get(void) noexcept; 		// Get library for calling functions below
	SYMBOL_EXPORT [[nodiscard]] int init(const init_params& par) noexcept;	// Must be called first, returns negative on error, 0 or positive on success
	static constexpr const int BREAKING_VERSION = 1;					// Hopefully will always be 1. When braking API change is introduced, this version goes up so apps which link with the library can detect that during compilation
	static constexpr const char* thread_names_prefix = "gusli_";		// All gusli aux threads will have this prefix for easier ps | grep
	SYMBOL_EXPORT const char *get_metadata_json(void) const noexcept;	// Get the version of the library to adapt application dynamically ot library features set.
	SYMBOL_EXPORT [[nodiscard]] int destroy(void) noexcept;				// Must be called last, returns negative on error, 0 or positive on success

	SYMBOL_EXPORT [[nodiscard]] enum connect_rv bdev_connect(      const backend_bdev_id& id) noexcept;	// Open block device, must be done before register buffers or submitting io
	SYMBOL_EXPORT [[nodiscard]] enum connect_rv bdev_get_info(     const backend_bdev_id& id, struct bdev_info *ret_val) noexcept __nonnull ((1));
	SYMBOL_EXPORT [[nodiscard]] enum connect_rv bdev_bufs_register(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept;	// Register shared memory buffers which will store the content of future io
	SYMBOL_EXPORT [[nodiscard]] enum connect_rv bdev_bufs_unregist(const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept;
	SYMBOL_EXPORT [[nodiscard]] enum connect_rv bdev_disconnect(   const backend_bdev_id& id) noexcept;
	SYMBOL_EXPORT void bdev_report_data_corruption(  const backend_bdev_id& id, uint64_t offset_lba_bytes) noexcept;
};

/******************************** RAII API ***********************/
// High level more C++ API, Construct which throws exception
class global_clnt_raii : no_implicit_constructors {		// RAII (Resource Acquisition Is Initialization) to manage the singleton lifecycle
public:
	static constexpr const int BREAKING_VERSION = 1;					// Hopefully will always be 1. When braking API change is introduced, this version goes up so apps which link with the library can detect that during compilation
	SYMBOL_EXPORT global_clnt_raii(const global_clnt_context::init_params& par) {
		auto& instance = global_clnt_context::get();
		if (instance.init(par) < 0)
			throw std::runtime_error("Failed to initialize gusli client");
	}
	SYMBOL_EXPORT ~global_clnt_raii() noexcept { (void)global_clnt_context::get().destroy(); }
	SYMBOL_EXPORT const char *get_metadata_json(void) const noexcept { return global_clnt_context::get().get_metadata_json(); }
	SYMBOL_EXPORT [[nodiscard]] static enum connect_rv bufs_register( const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs) noexcept;	// Register shared memory buffers which will store the content of future io
	SYMBOL_EXPORT [[nodiscard]] static enum connect_rv bufs_unregist( const backend_bdev_id& id, const std::vector<io_buffer_t>& bufs, bool stop_server = false) noexcept;
	SYMBOL_EXPORT [[nodiscard]] static enum connect_rv get_bdev_info( const backend_bdev_id& id, struct bdev_info &rv) noexcept { return global_clnt_context::get().bdev_get_info(id, &rv); }
	SYMBOL_EXPORT [[nodiscard]] static int32_t   get_bdev_descriptor( const backend_bdev_id& id) noexcept;
	SYMBOL_EXPORT               static void   report_data_corruption( const backend_bdev_id& id, uint64_t offset_lba_bytes) noexcept { global_clnt_context::get().bdev_report_data_corruption(id, offset_lba_bytes); }
};

} // namespace gusli