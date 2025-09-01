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

#define LIB_NAME "GUSLIi"           // Common infrastructure
#define LIB_COLOR NV_COL_NONE
#include "shared_mem_bufs.hpp"

#define PRINT_GLOBAL_BUF_FMT     PRINT_MMAP_PREFIX "%s.shm_io_bufs_global_t[%p].refs[%d]"
#define PRINT_GLOBAL_BUF_ARGS()  debug_who, shm_io_bufs_singleton, (shm_io_bufs_singleton ? shm_io_bufs_singleton->n_refs: 0)
namespace gusli {

static shm_io_bufs_global_t* shm_io_bufs_singleton = nullptr;

shm_io_bufs_global_t* shm_io_bufs_global_t::get(const char* debug_who) {
	if (!shm_io_bufs_singleton) {
		shm_io_bufs_singleton = new shm_io_bufs_global_t();
		pr_info1(PRINT_GLOBAL_BUF_FMT ".construct\n", PRINT_GLOBAL_BUF_ARGS());
	} else {
		t_lock_guard l(shm_io_bufs_singleton->lock_);
		shm_io_bufs_singleton->n_refs++;
		pr_info1(PRINT_GLOBAL_BUF_FMT ".did_inc\n", PRINT_GLOBAL_BUF_ARGS());
	}
	return shm_io_bufs_singleton;
}

void shm_io_bufs_global_t::put(const char* debug_who) {
	if (!shm_io_bufs_singleton) {
		pr_err1(PRINT_GLOBAL_BUF_FMT ".put_be4_get\n", PRINT_GLOBAL_BUF_ARGS());
		return;
	} else {
		t_lock_guard l(shm_io_bufs_singleton->lock_);
		shm_io_bufs_singleton->n_refs--;
		pr_info1(PRINT_GLOBAL_BUF_FMT ".did_dec\n", PRINT_GLOBAL_BUF_ARGS());
		if (shm_io_bufs_singleton->n_refs > 0)
			return;
	}	// Here release the lock to be able to delete
	pr_info1(PRINT_GLOBAL_BUF_FMT ".destruct.total_n_buffers=%u\n", PRINT_GLOBAL_BUF_ARGS(), shm_io_bufs_singleton->client_unique_buf_idx_generator);
	if (shm_io_bufs_singleton->n_refs == 0)
		delete shm_io_bufs_singleton;
	shm_io_bufs_singleton = nullptr;
}

base_shm_element* shm_io_bufs_global_t::find1(const io_buffer_t &buf) {
	for (base_shm_element& s : bufs) {
		if (s.mem.is_exact_mapped((void*)buf.ptr, buf.byte_len))
			return &s;
	}
	return nullptr;
}

base_shm_element* shm_io_bufs_global_t::find2(const uint32_t buf_idx) {
	for (base_shm_element& s : bufs) {
		if (s.buf_idx == buf_idx)
			return &s;
	}
	return nullptr;
}

const base_shm_element* shm_io_bufs_global_t::does_intersect(const io_buffer_t &buf) const {
	for (const base_shm_element& s : bufs) {
		if (s.mem.is_intersect((void*)buf.ptr, buf.byte_len))
			return &s;
	}
	return nullptr;
}

void shm_io_bufs_global_t::dec_ref(const base_shm_element *el, bool may_free) {
	const int vec_idx = std::distance((const base_shm_element *)bufs.data(), el);
	DEBUG_ASSERT(el->ref_count >= 1);
	if (!may_free)
		DEBUG_ASSERT(el->ref_count >= 2);
	if (--bufs[vec_idx].ref_count == 0)
		bufs.erase(bufs.begin() + vec_idx);
}

const base_shm_element* shm_io_bufs_global_t::insert_on_client(const io_buffer_t& r) {
	base_shm_element* rv = find1(r);
	if (rv) {			// This buffer is already known (mapped to a different bdev), increase ref count
		rv->ref_count++;
		return rv;
	}
	rv = (base_shm_element*)does_intersect(r);
	if (rv) {			// This buffer is partially intersects with at least 1 buffer, illegal
		pr_err1("Error: trying to register buffer " PRINT_IO_BUF_FMT " intersects with another registered buffer " PRINT_REG_IDX_FMT PRINT_IO_BUF_FMT "\n", PRINT_IO_BUF_ARGS(r), rv->buf_idx, PRINT_IO_BUF_ARGS(rv->get_buf()));
		return nullptr;
	}
	BUG_ON(bufs.size() >= max_num_mapped_ranges, "Too much mapped ranges, performance will degrade...");
	rv = &bufs.emplace_back(++client_unique_buf_idx_generator);
	char name[32];
	snprintf(name, sizeof(name), file_fmt, rv->buf_idx);
	int map_rv = rv->mem.init_producer(name, r.byte_len, (void*)r.ptr);
	ASSERT_IN_PRODUCTION(map_rv == 0);
	return rv;
}

const base_shm_element* shm_io_bufs_global_t::insert_on_server(const char* name, uint32_t buf_idx, void* client_pointer, uint64_t n_bytes) {
	base_shm_element* rv = find2(buf_idx);
	if (rv) {			// This buffer is already known (mapped to a different bdev), increase ref count
		rv->ref_count++;
		return rv;
	}
	BUG_ON(bufs.size() >= max_num_mapped_ranges, "Too much mapped ranges, performance will degrade...");
	rv = &bufs.emplace_back(buf_idx, client_pointer);
	int map_rv = rv->mem.init_consumer(name, n_bytes);
	ASSERT_IN_PRODUCTION(map_rv == 0);
	return rv;
}

std::vector<io_buffer_t> shm_io_bufs_global_t::get_all_bufs(const shm_io_bufs_unique_set_for_bdev& u) {
	t_lock_guard l(with_lock());
	std::vector<io_buffer_t> rv;
	rv.reserve(bufs.size());
	for (const base_shm_element& shm_buf : bufs) {
		if (u.has(shm_buf.buf_idx))					// Deliberately, return the buffers ordered as 'bufs' not as 'u'
			rv.emplace_back(shm_buf.get_buf());
	}
	return rv;
}

} // namespace
