#pragma once
#include <stdio.h>			// FILE, printf
#include <string.h>			// memset, memcmp
#include <stdint.h>			// uint64_t, uint32_t
#include <vector>
#include <semaphore.h>		// Waiting for async io completion
#include <thread>			// thread::sleep
#include <stdarg.h>			// Debug print to log

#include "gusli_client_api.hpp"

int _log(const char *fmt,...) __attribute__ ((format (printf, 1, 2)));
int _log(const char *fmt,...) {
	int res = 0;
	va_list ap;
	va_start(ap, fmt);
	res = vfprintf(stderr, fmt, ap);	/* Skip prefix */
	va_end(ap);
	return res;
}
#define log(fmt, ...) ({ _log("\x1b[1;37m" "UserApp: " fmt "\x1b[0;0m",          ##__VA_ARGS__); fflush(stderr); })
#define my_assert(expr) ({ if (!(expr)) { fprintf(stderr, "Assertion failed: " #expr ", %s() %s[%d] ", __PRETTY_FUNCTION__, __FILE__, __LINE__); std::abort(); } })


enum io_exec_mode { ILLEGAL = 0, SYNC_BLOCKING_1_BY_1, ASYNC_CB, POLLABLE, URING_BLOCKING, URING_POLLABLE, N_MODES } mode;
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
#define for_each_exec_mode(i) for (int i = io_exec_mode::SYNC_BLOCKING_1_BY_1; i < io_exec_mode::N_MODES; i++)

/***************************** Example of io usage ***************************************/
struct unitest_io {
	static constexpr const int buf_size = (1 << 16);	// 64K
	const int buf_align = sysconf(_SC_PAGESIZE);
	gusli::io_request io;
	char* io_buf = NULL;		// Source for write, destination buffer for read.
	sem_t wait;					// Block sender until io returns
	int counter = 0;			// Number of executed ios
	void print_io_comp(void) { log("\t  +cmp[%u] %s-%c rv=%d\n", counter++, io_exec_mode_str(mode), io.params.op, io.get_error()); }
	static void __comp_cb(struct unitest_io *c) {
		c->print_io_comp();
		my_assert(sem_post(&c->wait) == 0);	// Unblock waiter
	}
	unitest_io() { my_assert(posix_memalign((void**)&io_buf, buf_align, buf_size) == 0);	}
	~unitest_io() { if (io_buf) free(io_buf); }
	void exec(gusli::io_type _op, io_exec_mode _mode, bool expect_success = true) {
		mode = _mode;
		const int n_bytes = (int)io.params.buf_size();
		my_assert(n_bytes < buf_size);
		log("\tSubmit[%u] %s-%c %u[b], n_ranges=%u\n", counter, io_exec_mode_str(mode), _op, n_bytes, io.params.num_ranges());
		io.params.op = _op;
		if (mode == io_exec_mode::ASYNC_CB) {
			io.params.set_completion(this, __comp_cb);
			my_assert(sem_init(&wait, 0, 0) == 0);
		} else if ((mode == POLLABLE) || (mode == URING_POLLABLE)) {
			io.params.set_async_pollable();
		} else if ((mode == SYNC_BLOCKING_1_BY_1) || (mode == URING_BLOCKING)) {
			io.params.set_blocking();
		} else {my_assert(false); }
		io.params.try_using_uring_api = ((mode == URING_POLLABLE) || (mode == URING_BLOCKING));
		io.submit_io();
		if (mode == io_exec_mode::ASYNC_CB) {
			my_assert(sem_wait(&wait) == 0);
		} else if ((mode == POLLABLE) || (mode == URING_POLLABLE)) {
			while (io.get_error() == gusli::io_error_codes::E_IN_TRANSFER) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			print_io_comp();
		} else {
			print_io_comp();
		}
		if (_op == gusli::G_READ)
			io_buf[n_bytes] = 0;		// Null termination for prints
		const bool io_succeeded = (io.get_error() == gusli::io_error_codes::E_OK);
		my_assert(io_succeeded == expect_success);
	}
	void clean_buf(void) { memset(io_buf, 0, buf_align); memset(io_buf, 'C', 16); }
};
