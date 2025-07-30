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
#include "gusli_client_api.hpp"
#include "00utils/atomics.hpp"

namespace gusli {
/************************* Generic datapath logging **************************/
#define PRINT_IO_BUF_FMT   "BUF{0x%lx[b]=%p}"
#define PRINT_IO_BUF_ARGS(c) (c).byte_len, (c).ptr
#define PRINT_IO_REQ_FMT   "IO{%c:ofs=0x%lx, " PRINT_IO_BUF_FMT ", #rng=%d}"
#define PRINT_IO_REQ_ARGS(c) (c).op, (c).map.offset_lba_bytes, PRINT_IO_BUF_ARGS(c.map.data), (c).num_ranges()
#define PRINT_IO_SQE_ELEM_FMT   "sqe[%03u]"
#define PRINT_IO_CQE_ELEM_FMT   "cqe[%03u]"
#define PRINT_CLNT_IO_PTR_FMT   ".clnt_io_ptr[%p]"

#if !defined(LIB_NAME) || !defined(LIB_COLOR)
	#error: Including file must define the above to use generic client/server logging system
#endif
#define pr_info1(fmt, ...) ({ pr_info( LIB_COLOR LIB_NAME ": " fmt NV_COL_R,    ##__VA_ARGS__); pr_flush(); })
#define pr_err1( fmt, ...) ({ pr_err(            LIB_NAME ": " fmt         ,    ##__VA_ARGS__); })
#define pr_note1(fmt, ...) ({ pr_note(           LIB_NAME ": " fmt         ,    ##__VA_ARGS__); })
#define pr_verb1(fmt, ...) ({ pr_verbs(LIB_COLOR LIB_NAME ": " fmt NV_COL_R,    ##__VA_ARGS__); pr_flush(); })

/************************* Client-Srvr Common memBuf **************************/
struct base_shm_element {
	t_shared_mem mem;
	void *other_party_ptr;		// For debug only: Client stores servers mem pointer, Server store client side pointer
	uint32_t buf_idx;			// Index of buffer, same value on client and server, all fields of 'mem' can differ between client and server
	void* convert_to_my(io_buffer_t &buf) const {
		const size_t offset = (size_t)buf.ptr - (size_t)other_party_ptr;
		void* const rv = (void*)((size_t)mem.get_buf() + offset);
		if (mem.is_mapped(rv, buf.byte_len)) {
			pr_verb1("Remapping[%di].io_buf{ %p -> %p }\n", buf_idx, buf.ptr, rv);
			return buf.ptr = rv;
		}
		pr_verb1("Remapping[%di].io_buf{ %p not in my buffer }\n", buf_idx, buf.ptr);
		return NULL;
	}
};

class shm_io_bufs_t {
	std::vector<base_shm_element> bufs;	// Mapped User external io buffers
 public:
	shm_io_bufs_t() { /* bufs.reserve(16); */ }
	~shm_io_bufs_t() { bufs.clear(); }
	size_t size(void) const { return bufs.size(); }
	int find(const io_buffer_t &buf) const {
		for (size_t i = 0; i < bufs.size(); i++) {
			if (bufs[i].mem.is_exact_mapped((void*)buf.ptr, buf.byte_len))
				return i;
		}
		return -1;	// Not an element in the vector
	}

	int find(const uint32_t buf_idx) const {
		for (size_t i = 0; i < bufs.size(); i++) {
			if (bufs[i].buf_idx == buf_idx)
				return i;
		}
		return -1;	// Not an element in the vector
	}

	bool does_include(const io_buffer_t &buf) const {
		for (const base_shm_element& shm_buf : bufs) {
			if (shm_buf.mem.is_mapped((void*)buf.ptr, buf.byte_len))
				return true;
		}
		return false;	// Not contained in any mapped range
	}

	bool remap_to_local(io_buffer_t &buf) const {
		for (const base_shm_element& shm_buf : bufs) {
			if (shm_buf.convert_to_my(buf)) {
				return true;
			} else { } /* We dont know in which range the io resides so try with a different buffer */
		}
		return false;	// Not contained in any mapped range
	}
	base_shm_element* insert(uint32_t buf_idx, void *other_party_ptr = nullptr) {
		base_shm_element* rv = &bufs.emplace_back();
		rv->buf_idx = buf_idx;
		rv->other_party_ptr = other_party_ptr;
		return rv;
	}
	void remove_idx(int vec_idx) {  bufs.erase(bufs.begin() + vec_idx); }

	bool add_other_party_ptr(uint32_t buf_idx, void *other_party_ptr) {
		const int vec_idx = find(buf_idx);
		if (vec_idx >= 0) {
			bufs[vec_idx].other_party_ptr = other_party_ptr;
			return true;
		}
		return false;
	}
	//    base_shm_element& operator [](int vec_idx)       { return bufs[vec_idx]; }
	const base_shm_element& operator [](int vec_idx) const { return bufs[vec_idx]; }
};

} // namespace gusli
