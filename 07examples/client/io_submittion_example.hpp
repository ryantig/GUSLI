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
#include "gusli_client_api.hpp"
#include "../common.hpp"

enum io_exec_mode { ILLEGAL = 0, POLLABLE, ASYNC_CB, URING_POLLABLE, URING_BLOCKING, SYNC_BLOCKING_1_BY_1, N_MODES };
static const char* io_exec_mode_str(io_exec_mode m) {
	switch(m) {
		case SYNC_BLOCKING_1_BY_1: return "BSync_block";
		case ASYNC_CB:             return "AC_async_cb";
		case POLLABLE:             return "APasyncPoll";
		case URING_BLOCKING:       return "UBringBlock";
		case URING_POLLABLE:       return "UPring_Poll";
		default: my_assert(false); return "";
	}
}
#define for_each_exec_mode(i)       for (int i = ((int)io_exec_mode::ILLEGAL + 1); i < io_exec_mode::N_MODES;        i++)
#define for_each_exec_async_mode(i) for (int i = ((int)io_exec_mode::POLLABLE   ); i < io_exec_mode::URING_BLOCKING; i++)

/***************************** Example of io usage ***************************************/
class unitest_io {
	static constexpr const int buf_len = (1 << 16);	// 64K
	const int buf_align = sysconf(_SC_PAGESIZE);
	sem_t wait;					// Block sender until io returns
	bool is_waiting_for_callback;
	bool _expect_success = true;
	bool _verbose = true;
	bool _should_wait_for_io_finish = true;
	bool _should_cancel = false;
	enum io_exec_mode mode;
 public:
	unsigned int n_ios = 0;		// Number of executed ios
	unsigned int n_cancl = 0;	// Number of canceled ios
 private:
	void print_io_comp(void) { n_ios++; if (_verbose) log_unitest("\t  +cmp[%u] %s-%c rv=%d\n", n_ios, io_exec_mode_str(mode), io.params.op(), io.get_error()); }
	using ge = gusli::io_error_codes;
	static void __comp_cb(unitest_io *c) {
		const ge io_rv = c->io.get_error();
		my_assert(io_rv != ge::E_IN_TRANSFER);							// When callback is is done and cannot be in air
		if (!c->_should_wait_for_io_finish) {
			my_assert((io_rv == ge::E_CANCELED_BY_CALLER) || (io_rv == ge::E_THROTTLE_RETRY_LATER));		// Stuck ios can be resolved only by force cancelation or throttling
		} else
			my_assert(io_rv != ge::E_CANCELED_BY_CALLER);				// Even if canceled by caller, while IO is in air, callback cannot arrive
		c->is_waiting_for_callback = false;
		c->print_io_comp();
		if (c->_should_cancel)
			my_assert(c->io.cancel_wait() == gusli::io_request::cancel_rv::G_ALLREADY_DONE); // If callback was returned, IO is done, cannot cancel it
		my_assert(sem_post(&c->wait) == 0);	// Unblock waiter. Must be last expression to prevent the callback from running while io is retried.
	}
	void blocking_wait_for_io_finish(void) {
		if (mode == io_exec_mode::ASYNC_CB) {
			if (io.get_error() == ge::E_CANCELED_BY_CALLER) {
				print_io_comp();
				my_assert(is_waiting_for_callback == true);	// Callback will not come
				is_waiting_for_callback = false;
			} else {
				my_assert(sem_wait(&wait) == 0);			// Block until Callback arrives
			}
		} else if ((mode == POLLABLE) || (mode == URING_POLLABLE)) {
			while (io.get_error() == ge::E_IN_TRANSFER) {
				//std::this_thread::sleep_for(std::chrono::nanoseconds(100));
			}
			print_io_comp();
		} else { /* Nothing to do for blocking io */
			print_io_comp();
		}
		assert_rv();
	}
	void assert_rv(void) {
		if ((io.params.op() == gusli::G_READ) && (io.get_error() != ge::E_CANCELED_BY_CALLER)) {
			const uint64_t n_bytes = io.params.buf_size();
			io_buf[n_bytes] = 0;		// Null termination for prints of buffer content
		}
		const ge io_rv = io.get_error();
		my_assert(is_waiting_for_callback == false);
		if (io_rv == ge::E_CANCELED_BY_CALLER) {
			n_cancl++;
			if (_should_wait_for_io_finish)
				my_assert(_should_cancel);		// Blocking cancel waits for IO to finish
		} else {
			const bool io_succeeded = (io_rv == ge::E_OK);
			my_assert(io_succeeded == _expect_success);
		}
		io.done();
		_should_cancel = false;	// Reset
	}
 public:
	gusli::io_request io;
	char* io_buf = NULL;		// Source for write, destination buffer for read.
	unitest_io() { my_assert(posix_memalign((void**)&io_buf, buf_align, buf_len) == 0);	}
	~unitest_io() { if (io_buf) free(io_buf); }
	gusli::io_buffer_t get_map(void) const { return gusli::io_buffer_t::construct(io_buf, buf_len); }
	const unitest_io& exec(gusli::io_type _op, io_exec_mode _mode) {
		mode = _mode;
		const int n_bytes = (int)io.params.buf_size();
		my_assert(n_bytes < buf_len);
		if (_verbose) log_unitest("\tSubmit[%u] %s-%c %u[b], n_ranges=%u\n", n_ios, io_exec_mode_str(mode), _op, n_bytes, io.params.num_ranges());
		io.params.set(_op);
		if (mode == io_exec_mode::ASYNC_CB) {
			io.params.set_completion(this, __comp_cb);
			my_assert(sem_init(&wait, 0, 0) == 0);
		} else if ((mode == POLLABLE) || (mode == URING_POLLABLE)) {
			io.params.set_async_pollable();
		} else if ((mode == SYNC_BLOCKING_1_BY_1) || (mode == URING_BLOCKING)) {
			io.params.set_blocking();
		} else {my_assert(false); }
		io.params.set_try_use_uring((mode == URING_POLLABLE) || (mode == URING_BLOCKING));
		is_waiting_for_callback = io.params.has_callback();
		io.submit_io();
		if (_should_cancel) {
			if (!io.params.may_use_uring())
				std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
			(void)io.cancel_wait();
		}
		if (_should_wait_for_io_finish)
			blocking_wait_for_io_finish();
		return *this;
	}
	void exec_cancel(gusli::io_type _op, io_exec_mode _mode) {
		_should_cancel = true;
		exec(_op, _mode);
	}
	void exec_dont_block(gusli::io_type _op, io_exec_mode _mode) {
		my_assert(_should_wait_for_io_finish == true);
		_should_wait_for_io_finish = false;
		exec(_op, _mode);
	}
	void exec_dont_block_finish(void) {
		my_assert(_should_wait_for_io_finish == false);
		blocking_wait_for_io_finish();
		_should_wait_for_io_finish = true;
	}

	unitest_io& expect_success(bool val) { _expect_success = val; return *this; }
	unitest_io& clear_stats(void) { n_ios = n_cancl = 0; return *this; }
	unitest_io& enable_prints(bool val) { _verbose = val; return *this; }
	void clean_buf(void) { memset(io_buf, 0, buf_align); memset(io_buf, 'C', 16); }
};

gusli::io_buffer_t alloc_io_buffer(const uint32_t block_size, uint32_t n_blocks) {
	gusli::io_buffer_t map;
	map.byte_len = (uint64_t)block_size * (uint64_t)n_blocks;
	my_assert(posix_memalign(&map.ptr, block_size, map.byte_len) == 0);
	my_assert(map.is_valid_for(block_size));
	return map;
}
