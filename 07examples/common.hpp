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
#define log_unitest(fmt, ...) ({ _unitest_log_fn("\x1b[1;37m" "UserApp: " fmt "\x1b[0;0m", ##__VA_ARGS__); fflush(stderr); })
#define log_line(fmt, ...) log_unitest("----------------- " fmt " -----------------\n",          ##__VA_ARGS__)
#define my_assert(expr) ({ if (!(expr)) { fprintf(stderr, "Assertion failed: " #expr ", %s() %s[%d] ", __PRETTY_FUNCTION__, __FILE__, __LINE__); std::abort(); } })

#define MAX_SERVER_IN_FLIGHT_IO (256)
static constexpr const char* spdk_srvr_listen_address = "/dev/shm/gs8888_uds";
static constexpr const char* spdk_srvr_bdev0_uuid = "8888spdk5555uuid____";

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