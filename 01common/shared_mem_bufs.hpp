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
#include <unordered_set>

namespace gusli {
/************************* Generic datapath logging **************************/
// IO logging
#define PRINT_IO_BUF_FMT   "BUF{0x%lx[b]=%p}"
#define PRINT_IO_BUF_ARGS(c) (c).byte_len, (c).ptr
#define PRINT_IO_REQ_FMT   "IO{%c:ofs=0x%lx, " PRINT_IO_BUF_FMT ", #rng=%d}"
#define PRINT_IO_REQ_ARGS(c) (c).op, (c).map.offset_lba_bytes, PRINT_IO_BUF_ARGS(c.map.data), (c).num_ranges()
#define PRINT_IO_SQE_ELEM_FMT   "sqe[%03u]"
#define PRINT_IO_CQE_ELEM_FMT   "cqe[%03u]"
#define PRINT_CLNT_IO_PTR_FMT   ".clnt_io_ptr[%p]"

// Memory registration logging
#define PRINT_MMAP_PREFIX     "MMAP: "
#define PRINT_REG_BUF_FMT    PRINT_MMAP_PREFIX "Register[%d%c] " PRINT_IO_BUF_FMT ", n_blocks=0x%x"
#define PRINT_UNR_BUF_FMT    PRINT_MMAP_PREFIX "UnRegist[%d%c] " PRINT_IO_BUF_FMT ", n_blocks=0x%x"
#define PRINT_REG_IDX_FMT    "[%di]"
#define PRINT_REG_BUF_ARGS(pr, buf) pr->get_buf_idx(), pr->get_buf_type(), PRINT_IO_BUF_ARGS(buf), pr->num_blocks

#if !defined(LIB_NAME) || !defined(LIB_COLOR)
	#error: Including file must define the above to use generic client/server logging system
#endif
#define pr_info1(fmt, ...) ({ pr_info( LIB_COLOR LIB_NAME ": " fmt NV_COL_R,    ##__VA_ARGS__); pr_flush(); })
#define pr_err1( fmt, ...) ({ pr_err(            LIB_NAME ": " fmt         ,    ##__VA_ARGS__); })
#define pr_note1(fmt, ...) ({ pr_note(           LIB_NAME ": " fmt         ,    ##__VA_ARGS__); })
#define pr_verb1(fmt, ...) ({ pr_verbs(LIB_COLOR LIB_NAME ": " fmt NV_COL_R,    ##__VA_ARGS__); pr_flush(); })

/************************* Clnt-Srvr Global memBuf **************************/
class base_shm_element {			// Represents a unique shared mem region (with appropriate file under /dev/shm). 1 per process, shared with all block devices
 public:
	t_shared_mem mem;
	void *other_party_ptr;			// Server store client side pointer to be able to translate client io bufs pointers to his addresses. Not used by client because it maps buffers to multiple servers / bdevs
	uint32_t buf_idx;				// Unique evergrowing identifier of a buffer. Same value on client and server. All other fields 'this' can differ between client and server
	uint32_t ref_count;				// Amount of block devices which use this memory region. Client Process: may register it with multiple servers. Server Process: may be in charge of multiple block devices
	base_shm_element(uint32_t bi, void *p = nullptr) : other_party_ptr(p), buf_idx(bi), ref_count(1) {}
	~base_shm_element() { ASSERT_IN_PRODUCTION(ref_count == 0); ref_count = ~0U; }							// Trap double destructor call
	base_shm_element(base_shm_element&& o) noexcept { move(o); }											// Inserting class into container
	base_shm_element& operator=(base_shm_element&& o) noexcept { return (this != &o) ? move(o) : *this; }	// Allow removal of elements from a container
	void* convert_to_my(io_buffer_t &buf) const {		// Convert Client pointer to Server pointer, Happens for every IO
		const size_t offset = (size_t)buf.ptr - (size_t)other_party_ptr;
		void* const rv = (void*)((size_t)mem.get_buf() + offset);
		if (mem.is_mapped(rv, buf.byte_len)) {
			pr_verb1("Remapping " PRINT_REG_IDX_FMT ".io_buf{ %p -> %p }\n", buf_idx, buf.ptr, rv);
			return buf.ptr = rv;
		}
		pr_verb1("Remapping" PRINT_REG_IDX_FMT ".io_buf{ %p not in my buffer }\n", buf_idx, buf.ptr);
		return NULL;
	}
	const io_buffer_t get_buf() const { return io_buffer_t::construct(mem.get_buf(), mem.get_n_bytes()); }
 private:
	base_shm_element& move(base_shm_element& o) noexcept {	// Move constructor
		this->~base_shm_element();
		mem = std::move(o.mem);
		other_party_ptr = o.other_party_ptr;
		buf_idx = o.buf_idx;
		ref_count = o.ref_count;
		o.buf_idx = o.ref_count = 0;
		return *this;
	}
};

class shm_io_bufs_global_t : no_implicit_constructors {			// Singleton: Managing buffers for the entire process
	std::vector<base_shm_element> bufs;							// All Mapped io buffers
	uint32_t client_unique_buf_idx_generator;
	t_lock_spinlock lock_;
	int n_refs;													// Amount of objects using this global construct (Multiple servers in the same process, Client & Server if fork() is used in tests, etc )
	static constexpr const size_t max_num_mapped_ranges = 16;	// Too much io ranges will make datapath slow, because we serch them linearly. Solve this in future
	static constexpr const char* file_fmt = "/gs_iobuf_%06d";	// Buffer with index 'x' will have this file format under /dev/shm
 protected:
	shm_io_bufs_global_t() : client_unique_buf_idx_generator(0), n_refs(1) { lock_.init(); bufs.reserve(max_num_mapped_ranges); }
	~shm_io_bufs_global_t() { ASSERT_IN_PRODUCTION(n_refs == 0); lock_.destroy(); bufs.clear(); }
 public:
	static shm_io_bufs_global_t* get(const char* debug_who);	// Singletone refs
	static void                  put(const char* debug_who);

	// Memory register / Unregister API
	size_t size(void) const { return bufs.size(); }
	      base_shm_element* find1(         const io_buffer_t &buf);			// Called on client to register/unregister user buffer
	const base_shm_element* does_intersect(const io_buffer_t &buf) const;
	      base_shm_element* find2(         const uint32_t buf_idx);			// Called on server when receiving unique buffer idx from client
	void dec_ref(const base_shm_element *el, bool may_free = true);			// Done with buffer 'el', found with functions above
	const base_shm_element* insert_on_client(const io_buffer_t& r);
	const base_shm_element* insert_on_server(const char* name, uint32_t buf_idx, void* client_pointer, uint64_t n_bytes);

	// Datapath IO API
	bool does_include(const io_buffer_t &buf) const {						// Client: check if IO is included in registered buffers
		for (const base_shm_element& shm_buf : bufs) {
			if (shm_buf.mem.is_mapped((void*)buf.ptr, buf.byte_len))
				return true;
		}
		return false;	// Not contained in any mapped range, IO cannot be sent from client to server
	}
	bool remap_to_local(io_buffer_t &buf) const {							// Server: Remap client io buffer address to servers address
		for (const base_shm_element& shm_buf : bufs) {
			if (shm_buf.convert_to_my(buf)) {
				return true;
			} else { }  // We dont know in which range the io resides so try with a different buffer
		}
		return false;	// Not contained in any mapped range. Clients bug! IO cannot be executred by server
	}
};

/************************* Clnt-Srvr PerBdev memBuf **************************/
class shm_io_bufs_unuque_set_for_bdev {		// Each block device cannot allow multiple registration to buffers
	std::unordered_set<uint32_t> u;			// Unique set of already registered buffers
 public:
	size_t size(void) const { return u.size(); }
	bool has(uint32_t buf_idx) const { return u.find(  buf_idx) != u.end(); }
	bool add(uint32_t buf_idx)       { return u.insert(buf_idx).second; }
	bool del(uint32_t buf_idx)       { return u.erase( buf_idx) > 0; }
};

} // namespace gusli
