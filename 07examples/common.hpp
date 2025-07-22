#pragma once
#include <stdio.h>			// FILE, printf
#include <string.h>			// memset, memcmp
#include <stdint.h>			// uint64_t, uint32_t
#include <vector>
#include <semaphore.h>		// Waiting for async io completion
#include <thread>			// thread::sleep
#include <stdarg.h>			// Debug print to log

inline int _log(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
inline int _log(const char *fmt, ...) {
	int res = 0;
	va_list ap;
	va_start(ap, fmt);
	res = vfprintf(stderr, fmt, ap);	/* Skip prefix */
	va_end(ap);
	return res;
}
#define log(fmt, ...) ({ _log("\x1b[1;37m" "UserApp: " fmt "\x1b[0;0m",          ##__VA_ARGS__); fflush(stderr); })
#define my_assert(expr) ({ if (!(expr)) { fprintf(stderr, "Assertion failed: " #expr ", %s() %s[%d] ", __PRETTY_FUNCTION__, __FILE__, __LINE__); std::abort(); } })

// Typically
#define MAX_SERVER_IN_FLIGHT_IO (256)
static constexpr const char* spdk_srvr_listen_addre = "/dev/shm/gs8888_uds";
