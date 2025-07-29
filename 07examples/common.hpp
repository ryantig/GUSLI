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
#include <stdio.h>			// FILE, printf
#include <string.h>			// memset, memcmp
#include <stdint.h>			// uint64_t, uint32_t
#include <vector>
#include <semaphore.h>		// Waiting for async io completion
#include <thread>			// thread::sleep
#include <stdarg.h>			// Debug print to log

inline int _unitest_log_fn(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
inline int _unitest_log_fn(const char *fmt, ...) {
	int res = 0;
	va_list ap;
	va_start(ap, fmt);
	res = vfprintf(stderr, fmt, ap);	/* Skip prefix */
	va_end(ap);
	return res;
}
#define log_unitest(    fmt, ...) ({ _unitest_log_fn("\x1b[1;37m" "UserApp: " fmt "\x1b[0;0m", ##__VA_ARGS__); fflush(stderr); })
#define log_uni_success(fmt, ...) ({ _unitest_log_fn("\x1b[1;32m" fmt "\x1b[0;0m", ##__VA_ARGS__); fflush(stderr); })
#define log_uni_failure(fmt, ...) ({ _unitest_log_fn("\x1b[1;31m" fmt "\x1b[0;0m", ##__VA_ARGS__); fflush(stderr); })
#define log_line(fmt, ...) log_unitest("----------------- " fmt " -----------------\n",          ##__VA_ARGS__)
#define my_assert(expr) ({ if (!(expr)) { fprintf(stderr, "Assertion failed: " #expr ", %s() %s[%d] ", __PRETTY_FUNCTION__, __FILE__, __LINE__); std::abort(); } })

#define MAX_SERVER_IN_FLIGHT_IO (256)
namespace spdk_test {
	static constexpr const char* srvr_listen_address[] = {"/dev/shm/gs8888_uds",  "/dev/shm/gs9999aa_uds"};
	static constexpr const char* srvr_bdevs_uuid[] = {    "8888spdk5555uuid____", "9999spdk5555uuid____"};
	static constexpr const char* bdev_name[] = {          "dhs_bdev0",            "dhs_bdev1"};		// Same as spdk config file
	static constexpr const int num_bdevs = 2;
};

#include <unistd.h>  // for fork()
#include <sys/wait.h>
static inline void wait_for_process(__pid_t pid, const char* who) {
	int status;
	while (-1 == waitpid(pid, &status, 0));
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		log_unitest("\t %s rv=%d\n", who, WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		log_unitest("\t %s killed_ by signal=%d\n", who, WTERMSIG(status));
	} else {
		log_unitest("\t %s rv=%d\n", who, WEXITSTATUS(status));
	}
}

/*****************************************************************************/
/* First 8 bytes of each block store the address of that block
	so unit-tests can verify the correctness of reads */
namespace test_lba {
	static inline void map1_fill(const gusli::io_map_t &m, uint32_t block_size_bytes) {
		for (uint64_t b = 0; b < m.data.byte_len; b += block_size_bytes) {
			uint64_t *dst = (uint64_t*)((uint64_t)m.data.ptr + b);
			*dst = (m.offset_lba_bytes + b);
		}
	}
	static inline void mmio_fill(const gusli::io_multi_map_t* mio, uint32_t block_size_bytes) {
		for (uint32_t i = 0; i < mio->n_entries; i++)
			map1_fill(mio->entries[i], block_size_bytes);
	}
	static inline void map1_verify(const gusli::io_map_t &m, uint32_t block_size_bytes) {
		for (uint64_t b = 0; b < m.data.byte_len; b += block_size_bytes) {
			uint64_t *dst = (uint64_t*)((uint64_t)m.data.ptr + b);
			my_assert(*dst == (m.offset_lba_bytes + b));
		}
	}
	static inline void map1_verify_and_clean(const gusli::io_map_t &m, uint32_t block_size_bytes) {
		for (uint64_t b = 0; b < m.data.byte_len; b += block_size_bytes) {
			uint64_t *dst = (uint64_t*)((uint64_t)m.data.ptr + b);
			my_assert(*dst == (m.offset_lba_bytes + b));
			*dst = -1;		// Future reads must execute to access the data again
		}
	}
	static inline void mmio_verify(const gusli::io_multi_map_t* mio, uint32_t block_size_bytes) {
		for (uint32_t i = 0; i < mio->n_entries; i++)
			map1_verify(mio->entries[i], block_size_bytes);
	}
	static inline void mmio_verify_and_clean(const gusli::io_multi_map_t* mio, uint32_t block_size_bytes) {
		for (uint32_t i = 0; i < mio->n_entries; i++)
			map1_verify_and_clean(mio->entries[i], block_size_bytes);
	}
	static inline void map1_print(const gusli::io_map_t &m, const char* prefix) {
		log_unitest("%s map: len=0x%lx[b], off=0x%lx[b], %p\n",prefix, m.data.byte_len, m.offset_lba_bytes, m.data.ptr);
	}
	static inline void mmio_print(const gusli::io_multi_map_t* mio, const char* prefix) {
		log_unitest("\t%s: mio=%p, size=0x%lx, buf_size=0x%lx\n", prefix, mio, mio->my_size(), mio->buf_size());
		for (uint32_t i = 0; i < mio->n_entries; i++) {
			const gusli::io_map_t& m = mio->entries[i];
			log_unitest("\t\t%u) len=0x%lx[b], off=0x%lx[b], %p\n", i, m.data.byte_len, m.offset_lba_bytes, m.data.ptr);
		}
	}
};
