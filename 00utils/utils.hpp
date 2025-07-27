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
/* Common utils for all sub projects */
#include <stdio.h>		// FILE, printf
#include <string.h>	// strlen(), strcmp()
#include <stdint.h>		// uint64_t, uint32_t and such

/******************************* C/C++ issues ********************************/
#ifndef NULL
	#define NULL null_ptr
#endif
#ifndef typeof
	#define typeof(XYZ) __typeof__(XYZ)
#endif
#if (__GNUC__ >= 7)
	#define FALLTHRU __attribute__((fallthrough))
#else
	#define FALLTHRU
#endif
#ifndef __FUNCTION__
	#define __FUNCTION__ (__func__)
#endif

// Variant of static_assert() which is not supported by all compilers
#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2 * !!(condition)]))
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int : (-!!(e)); }))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __must_check    __attribute__((warn_unused_result)) //[[nodiscard]]
#define __maybe_unused  __attribute__((unused))       // [[maybe_unused]]

#define __stringify_1(x) #x
#define __stringify(x) __stringify_1(x)

#ifndef offsetof
	#define offset_of(struct_type, member) ((char*)&(((struct_type*)0)->member) - (char*)0)		// __builtin_offsetof(struct_type, member)
#endif
#ifndef container_of
	#define container_of(ptr, struct_type, member) ({                   \
		const typeof(((struct_type*)0)->member)* __mptr = (ptr);        \
		(struct_type*)((char*)__mptr - offset_of(struct_type, member)); \
	})
#endif
#ifndef ARRAY_SIZE
	#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

// special define to be able to print and grep easily
#define nvTODO(x, ...) ({})		// message(__FILE__ "(" __stringify(__LINE__) ") : TODO : " x "\n")

template<class T> static inline T min(         T x, T y) { return x < y ? x : y; }
template<class T> static inline T max(         T x, T y) { return x > y ? x : y; }
template<class T> static inline T div_round(   T x, T y) { return ((x + y/2) / y); }
template<class T> static inline T div_round_dn(T x, T y) { return (x / y); }
template<class T> static inline T div_round_up(T x, T y) { return ((x + y - 1) / y); }
template<class T> static inline T align_up(    T x, T y) { return (div_round_up(x,y) * y); }
template<class T> static inline T align_down(  T x, T y) { return ((x / y) * y); }
#ifndef MIN
	#define MIN(a,b) (((a)<(b))?(a):(b))
	#define MAX(a,b) (((a)>(b))?(a):(b))
#endif

struct no_implicit_constructors {	// No copy/move/assign operations to prevent accidental copying of resources, leaks, double free and performance degradation
	no_implicit_constructors(           const no_implicit_constructors& ) = delete;
	no_implicit_constructors(                 no_implicit_constructors&&) = delete;
	no_implicit_constructors& operator=(const no_implicit_constructors& ) = delete;
	no_implicit_constructors& operator=(      no_implicit_constructors&&) = delete;
	no_implicit_constructors() {}
};
struct no_constructors_at_all : no_implicit_constructors { // Stronger version, do not allow any constructors. Protect shared memory / placement new objects
	no_constructors_at_all() = default;
	~no_constructors_at_all() = delete;
};

/******************************** Debugger *********************************/
class tDbg : no_implicit_constructors {			// Debugging utility Singletone: Debugger/Logger/Timer.
	uint64_t timer;		// Used for profiling Timer start/measure
	int debugger_pid;	// Enusre the costly function called once
	FILE* flog;			// File to which logs are written
	tDbg() : debugger_pid(-1), flog(stdout) { timer_start(); } // Launch timer in case user uses get() before start
	~tDbg() {};
	int is_debugger_present(void);
	void break_point(void);
	static void dump_stack(void);
public:
	static tDbg& get(void); // Singletone
	// ----------------- Debugging utils
	static int get_cur_timestamp_str(char rv[32]);
	static int get_timestamp_str(    char rv[32], const uint32_t unix_epoch_sec);
	static uint64_t get_cur_timestamp_unix(void);
	void     timer_start(void) { timer = get_cur_timestamp_unix(); } // Dont use concurently from multiple threads
	uint64_t timer_get_start_time_usec(void) const { return this->timer; }
	uint64_t timer_get_elapsed_time_usec(void) const { return (get_cur_timestamp_unix() - timer); }
	static inline bool is_little_endian_machine(void) { const int i = 9; return *((char *)&i) == 9; }

	// ----------------- Log utils
	static void invoke_crash(const char* condition, const char* func_name, const char* file_name, int line_num, const char* fmt, ...) __attribute__((format(printf, 5, 6)));
	// static void invoke_crash(const std::exception&) { pr_emerg("Exception: %s\n", exc.what());  BUG_NOT_IMPLEMENTED(); }
	static int print(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	static void log_file_set(FILE* _f) { tDbg::get().flog = _f; }

	static void flog_flush(void); // flush all traces to disk, use between tests
	static inline void cpu_relax(void) { asm volatile("rep; nop" ::: "memory"); }
};

// Below replace assert() for 2 reasons: 1. Enable it in release mode. 2. Put breakpoint or make a coredump on assertion
#define BUG_ON(condition, format, ...) ({  \
	const bool hit___ = !!(condition); \
	if (unlikely(hit___)) { tDbg::invoke_crash(#condition, __PRETTY_FUNCTION__, __FILE__, __LINE__, format, ##__VA_ARGS__); } \
})
#define BUG_NOT_IMPLEMENTED()      BUG_ON(true,    "Not Implemented!")
#define ASSERT_IN_PRODUCTION(cond) BUG_ON(!(cond), "Assertion failed!")
#ifdef NDEBUG
	#define DEBUG_ASSERT(...)
#else
	#define DEBUG_ASSERT  ASSERT_IN_PRODUCTION
#endif

/********************************* Printing **********************************/
// https://telepathy.freedesktop.org/doc/telepathy-glib/telepathy-glib-debug-ansi.html
#define NV_COL_GREEN "\x1b[32m"
#define NV_COL_YELL "\x1b[1;33m" // Yellow
#define NV_COL_R "\x1b[0;0m" // Reset color
#define NV_COL_NONE	NV_COL_R
#define NV_COL_EMRG "\x1b[1;31m" // BoldRed, Bold format is 1;
#define NV_COL_PURPL "\x1b[1;35m" // Purple
#define NV_COL_BLUE "\x1b[16;34m" // Blue
#define NV_COL_WHITE_BOLD "\x1b[1;37m"
#ifndef TRACE_LEVEL
	#define TRACE_LEVEL (5)
#endif

#if (0)				// Set to 1 to prefix date+time stamp for every log entry
	#define tDbg__print(fmt, ...) ({ \
		char _ts[32]; tDbg::get_cur_timestamp_str(_ts); \
		tDbg::print("%s " fmt, _ts, ## __VA_ARGS__);\
	})
#elif (0)			// Set to 1 to prefix by cpu core, and time stamp to debug concurrent corruptions
	#include <sched.h>
	#define tDbg__print(fmt, ...) ({ \
		char _ts[32]; tDbg::get_cur_timestamp_str(_ts); \
		tDbg::print("%02u) %s " fmt, sched_getcpu(), _ts, ## __VA_ARGS__); \
	})
#else
	#define tDbg__print(fmt, ...) tDbg::print(fmt, ## __VA_ARGS__)
#endif
#define pr_emerg(fmt, ...) ({ if (TRACE_LEVEL >= 0) tDbg__print(NV_COL_EMRG  fmt NV_COL_R, ##__VA_ARGS__); })
#define pr_alert(fmt, ...) ({ if (TRACE_LEVEL >= 1) tDbg__print(NV_COL_YELL  fmt NV_COL_R, ##__VA_ARGS__); })
#define pr_crit( fmt, ...) ({ if (TRACE_LEVEL >= 2) tDbg__print(NV_COL_PURPL fmt NV_COL_R, ##__VA_ARGS__); })
#define pr_err(  fmt, ...) ({ if (TRACE_LEVEL >= 3) tDbg__print(NV_COL_GREEN fmt NV_COL_R, ##__VA_ARGS__); })
#define pr_warn( fmt, ...) ({ if (TRACE_LEVEL >= 4) tDbg__print(NV_COL_BLUE  fmt NV_COL_R, ##__VA_ARGS__); })
#define pr_note( fmt, ...) ({ if (TRACE_LEVEL >= 5) tDbg__print(             fmt,          ##__VA_ARGS__); }) // 5 should be the default level
#define pr_info( fmt, ...) ({ if (TRACE_LEVEL >= 6) tDbg__print(             fmt,          ##__VA_ARGS__); }) // traces
#define pr_debug(fmt, ...) ({ if (TRACE_LEVEL >= 7) tDbg__print(             fmt,          ##__VA_ARGS__); })
#define pr_verbs(fmt, ...) ({ if (TRACE_LEVEL >= 8) tDbg__print(             fmt,          ##__VA_ARGS__); })

#define pr_flush() tDbg::flog_flush()
static inline bool is_visible_ascii_char(const unsigned char c) { return ((c >= 0x21) && (c <= 0x7F)); }
static inline bool char_is_space(  char c) { return (c <= ' ') && (c != 0); }
static inline bool char_is_visible(char c) { return (c >  ' '); }
static inline char toupper_unsafe(char letter) { return (letter >= 'a') ? (letter-0x20) : letter; }
const char *__log_to_human(const char *str, const size_t len);
const char *__log_to_human(const char *str, const size_t len, char* dst_buf, const size_t dst_buf_len);
uint64_t str2_long_no_less_then(const char* str, long lb);
size_t strncpy_no_trunc_warning(char* dst, const char* src, int len); // In realease compilation gcc issues warning on string truncation when using strncpy

/* For easier grep in logs, there is a specific printing format for important variables */
#define PRINT_EXTERN_ERR_FMT  "errno=%d:%s"
#define PRINT_EXTERN_ERR_ARGS errno, strerror(errno)
static inline char bool_YN(bool x) { return (x ? 'Y' : 'N'); }
#ifndef scnprintf	// More secure version, returns correct amount of written bytes
	#define scnprintf(buf, len, ...) ({ int _x = snprintf(buf, len, __VA_ARGS__); ((_x >= (int)len-1) ? (int)len-1 : _x); })
#endif
#define BUF_ADD(...) count += scnprintf(&buf[count], buf_len - count, __VA_ARGS__)

/************************ Sandbox Unitest Environment ************************/
#ifdef UNITEST_ENV
	#define NVR_ROOT_DIR "."       // Sandbox local dir  ./
#else
	#define NVR_ROOT_DIR ""        // Production su root  /
#endif
constexpr static inline bool is_running_in_sandbox(void) { return (NVR_ROOT_DIR[0] != 0); }

class time_stamp_t {
	static constexpr const uint32_t giga = 1000*1000*1000;
 public:
	uint32_t sec;
	uint32_t nanosec;
	uint64_t to_uint64(void) const { return ((uint64_t)sec << 32) | (uint64_t)nanosec; }
	friend bool operator< (const time_stamp_t& lhs, const time_stamp_t& rhs) { return lhs.to_uint64() < rhs.to_uint64(); }
	friend bool operator> (const time_stamp_t& lhs, const time_stamp_t& rhs) { return lhs.to_uint64() > rhs.to_uint64(); }

	// Below debug unitest only functions for human readable time stamps
	time_stamp_t get_readable_format() const { return {.sec = __builtin_bswap32(sec), .nanosec = __builtin_bswap32(nanosec)}; }
	static time_stamp_t get_readable_format(time_stamp_t ts) { return ts.get_readable_format();	}
	void set_zero(void) { sec = 0; nanosec = 0; }
};

time_stamp_t get_time_now(char reset_to = 0);

/******************************* Networking **********************************/
#include <arpa/inet.h>
#include <sys/un.h>
struct connect_addr {
	union connect_addr_u {
		struct sockaddr    b;		// Base type
		struct sockaddr_in i;		// ip address
		struct sockaddr_un u;		// domain socket
	} u;
	void clean(void) { memset(this, 0, sizeof(*this)); }
	connect_addr() { clean(); }
	bool is_empty(void) const { return u.i.sin_port == 0; }
	int get_len(bool is_remote) const { return is_remote ? sizeof(u.i) : sizeof(u.u); }
	int get_remote_len(void) const { return sizeof(u.i); }
};
class sock_t {
 public:
	using ip_addr_str = char[20];	// "XXX.XXX.XXX.XXX" - human readable, 16[b] is enough, we use more to allow passing host name
	enum type { S_UNK = 0, S_UDP = 'U', S_TCP = 'T', S_UDS = 'D'};
	static bool is_ip_of_local_host(const ip_addr_str ip) { return (!strncmp(ip,"127.", 4) || !strncmp(ip,"::1", 3)); } // Actually IP4: 127.0.0.0/8, IP6 single address
 private:
	int _fd;			// internal file descriptor to read/write from/to
	int epoll_fd;		// Can wait for reply using epol, or busy loop
	enum type _type;
	int resolve_sin_addr(const ip_addr_str ip, struct sockaddr_in *srvr) const;
	void epoll_reply_start(void);		// Used to send message and blocking wait for reply
	bool has_epoll_running(void) const { return epoll_fd > 0; }
	void epoll_reply_stop(void);
	void __init_remote(uint32_t port, const char* ip, bool is_tcp, bool is_blocking, struct sockaddr_in& sin);
	void __init_local( const char* socket_path,                    bool is_blocking, struct sockaddr_un& addr);
	void __bind_server(const struct connect_addr &ca);
	int  __connct_clnt(const struct connect_addr &ca);
 public:
	sock_t() = default; // Make it trivially memset(0)
	sock_t(int fd, enum type t = type::S_UNK) : _fd(fd), epoll_fd(-1), _type(t) {}
	void epoll_reply_wait(const char* debug_prefix_to_print_in_logs = "") const;
	inline int fd(void) const { return _fd; }
	bool is_alive(void) const { return _fd > 0; }
	bool is_remote(void) const       { return (_type == type::S_TCP) || (_type == type::S_UDP); }
	enum type get_type(void) const { return _type; }
	bool uses_connection(void) const { return (_type == type::S_TCP) || (_type == type::S_UDS); }
	void set_blocking(bool is_blocking);
	void set_io_buffer_size(unsigned rlen, unsigned wlen);
	void set_optimize_for_latency(bool do_optimize);		//	True - Best latency, False - Best throughput
	int  srvr_listen(             uint32_t port, bool is_tcp,             connect_addr& ca, bool is_blocking);		// TCP / UDP
	int  srvr_listen(             const char* domain_socket_path,         connect_addr& ca, bool is_blocking);		// Unix domain socket
	void srvr_set_listen_timeout(uint32_t n_sec);
	int  srvr_accept_clnt(                                                connect_addr& ca) const;
	int  clnt_connect_to_srvr_udp(uint32_t port, const ip_addr_str ip,    connect_addr& ca, bool is_blocking);
	int  clnt_connect_to_srvr_tcp(uint32_t port, const ip_addr_str ip,    connect_addr& ca, bool is_blocking);
	int  clnt_connect_to_srvr_uds(const char* domain_socket_path,         connect_addr& ca, bool is_blocking);
	void print_address(                        char addr[32]      , const connect_addr& src) const;				// Upon incommin msg, extract server address
	void nice_close(bool force = false);
	ssize_t send_msg(const void *buf, size_t len,                   const connect_addr& ca) const;
};

enum io_state {
	ios_ok = 0,
	ios_close = 1,		// file descriptor was closed
	ios_block = 2,		// Not enough bytes for IO, has to wait
	ios_error = 3		// file descriptor got error, stop waiting for io
};
static inline bool io_state_broken(enum io_state st) { return (st == ios_error || st == ios_close); }
int readn( int fd,       char *buf, const int n_bytes, enum io_state *output);
int writen(int fd, const char *buf, const int n_bytes, enum io_state *output);

int thread_api_bound_to_1_core(uint16_t core_idx);
int thread_api_set_high_cpu_usage(const char *thread_name);

#include "bitmap.hpp"
uint32_t crc32_le_unoptimized(uint32_t salt, unsigned char const *p, size_t size);	// No AVX/SSE optimization, regular assembly instructions. Dont use in datapath

/*****************************************************************************/
class base_library {				// Generic interface for executable using libraries. Check permission, ensure only 1 copy runs, etc
 protected:
	__pid_t pid;					// My pid
	bool have_permissions_to_run(void) const;
 public:
	const char *lib_name;			// Short name of the executable / main thread name / appear as 'ps'. May not be identical to its file name (and not found by ps -f)
	char lib_info_json[256];
	volatile bool shutting_down;	// Executable in error state, turning off, stop all threads
	base_library(const char *lib_name);
	int start( void);
	int finish(const char* prefix, int rv);
	bool is_initialized(void) const { return lib_info_json[0] != 0; }
};

/*****************************************************************************/
// EOF.
