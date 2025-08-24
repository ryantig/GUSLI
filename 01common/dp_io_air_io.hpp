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
#include <stdio.h>
#include "gusli_server_api.hpp"
#include "00utils/atomics.hpp"
#include "dp_logging.hpp"

namespace gusli {

/*********************** Full IO api ***********************/
class server_io_req : public backend_io_req {		// Additional set of internal functions
 public:											// Note this is the same as base class, just add functions for the executor of the io
	// unique id functionality (io throttling / in air io)
	using uid_t = uint16_t;
	static constexpr const uid_t invalid_uid = 0x7FF;		// All 11 bits set
	void unique_id_assign(uid_t id = invalid_uid) { params._unique_id = id; }
	uid_t unique_id_get(void) const { return params._unique_id; }
	bool has_valid_unique_id(void) const { return (params._unique_id != invalid_uid); }
	void is_remote_set(bool v) { params._is_remote_bdev = v; }
	bool is_remote_get() const { return params._is_remote_bdev; }

	// IO Executor functionality
	bool is_valid(void) const { return (_exec == nullptr); }							// Verify no executor connected to io (from previous retry?)
	int64_t get_raw_rv(void) const { return out.rv; }
	void start_execution(void) { out.rv = io_error_codes::E_IN_TRANSFER; }
	const void *get_comp_ctx(void) const { return params._comp_ctx; }					// For debug prints of who handed this io to backend execution

	void client_receive_server_finish_io(int64_t rv);
	void internal_cancel(void) noexcept;
	~server_io_req() {
		BUG_ON(out.rv == io_error_codes::E_IN_TRANSFER, "Server Destroying io while it is still in air (running)!");
	}
};

/*********************** In Air IO's holder ***********************/
class in_air_ios_holder {
	t_lock_spinlock lock_;				// Producer Insert/remove mutual exclusion. Unrelated to ioring consumer
	uint64_t n_total_ios;				// Cumultive io stats
	uint64_t n_throttled_ios;
	using idx_t = server_io_req::uid_t;
	idx_t num_max_inflight_io;
	idx_t num_ios_in_air;
	server_io_req **ios_arr;			// For each in air io, store a pointer to it in element of its unique id index
	small_ints_set<500> bmp;			// Up to 500 ios. Stores a bit field, turned on bit for element in a set

	// Below all functions must be called while holding lock
	bool is_full( void) const { return (num_ios_in_air == num_max_inflight_io); }
	bool is_empty(void) const { return (num_ios_in_air == 0); }
	void del(server_io_req &io) {
		ASSERT_IN_PRODUCTION(!is_empty());
		const idx_t i = io.unique_id_get();
		ASSERT_IN_PRODUCTION(i < num_max_inflight_io);
		ASSERT_IN_PRODUCTION(ios_arr[i] == &io);
		ios_arr[i] = nullptr;
		bmp.remove(i);
		ASSERT_IN_PRODUCTION(--num_ios_in_air < num_max_inflight_io);
		io.unique_id_assign();	// Assign invalid value in case of io retry
	}
	idx_t find_next_free_element_from(idx_t i = 0) const {
		i = bmp.find_next_zero_bit(i);
		BUG_ON(i >= num_max_inflight_io, "IAIOL: Invalid call to this function. No space in array");
		return i;
	}
	idx_t find_next_used_element(idx_t i = 0) const {
		i = bmp.find_next_bit(i);
		BUG_ON(i >= num_max_inflight_io, "IAIOL: Invalid call to this function. No space in array");
		return i;
	}
 public:
	idx_t size(void) {
		t_lock_guard l(lock_);
		return num_ios_in_air;
	}
	in_air_ios_holder(idx_t n_max_ios) : n_total_ios(0), n_throttled_ios(0), num_max_inflight_io(n_max_ios), num_ios_in_air(0), bmp(num_max_inflight_io) {
		lock_.init();
		ASSERT_IN_PRODUCTION(bmp.size() < server_io_req::invalid_uid);	// Or else io.params._unique_id cannot hold the value
		ios_arr = new server_io_req*[n_max_ios];
		memset(ios_arr, 0, sizeof(server_io_req *) * n_max_ios);	// Set all ptrs to null
	}
	bool insert(server_io_req &io) {
		t_lock_guard l(lock_);
		if (unlikely(is_full())) {
			++n_throttled_ios;
			io.unique_id_assign();
			return false;
		}
		idx_t i = find_next_free_element_from();
		bmp.insert(i);
		ios_arr[i] = &io;
		io.unique_id_assign(i);
		++num_ios_in_air;
		++n_total_ios;
		return true;
	}
	void remove(server_io_req &io) {
		t_lock_guard l(lock_);
		del(io);
	}
	server_io_req *get_next_in_air_io(void) {
		t_lock_guard l(lock_);
		if (is_empty())
			return nullptr; // Do nothing
		idx_t i = find_next_used_element();
		return ios_arr[i];
	}

	void exec_for_each_io(void (*for_each_io_exec_fn)(server_io_req *io), bool remove = false) {
		t_lock_guard l(lock_);
		if (is_empty())
			return; // Do nothing
		uint32_t i;
		if (!remove) {
			for_each_set_bit(i, bmp) {
				ASSERT_IN_PRODUCTION(ios_arr[i] != nullptr);
				for_each_io_exec_fn(ios_arr[i]);		// Make sure you function does not require lock or dead lock will occure
			}
		} else {
			for_each_set_bit(i, bmp) {
				auto* ptr = ios_arr[i];
				ASSERT_IN_PRODUCTION(ptr != nullptr);
				if (remove)
					del(*ptr);							// Remove the io from the list
				for_each_io_exec_fn(ptr);				// Function can free the io. Make sure you function does not require lock or dead lock will occure
			}
		}
		BUG_ON((remove && !is_empty()), "IAIOL: corruption Corruption");
	}
	~in_air_ios_holder() {
		BUG_ON(lock_.is_locked(), "Circular buffer is still locked during destruction!");
		ASSERT_IN_PRODUCTION(num_ios_in_air == 0);
		for (idx_t i = 0; i < num_max_inflight_io; i++ )
			ASSERT_IN_PRODUCTION(ios_arr[i] == nullptr);	// All ios were returned;
		delete[] ios_arr;
		pr_info1("stats_ios{n_exec=%lu, n_throttle=%lu}\n", n_total_ios, n_throttled_ios);
	}
};

} // namespace gusli
