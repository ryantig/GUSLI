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
#include "utils.hpp"
#include <chrono>
#include <cstdlib>		// Malloc/free
#include <thread>		// sleep_for / thread api
#include <unistd.h>		// close/read

const char *__log_to_human(const char *str, const size_t len, char* dst_buf, const size_t dst_buf_len) {
	const char *nibble_to_hex = "0123456789abcdef";
	char *dst = dst_buf;
	ASSERT_IN_PRODUCTION(dst_buf_len > (len * 4)); // At most 1 byte is written as \x55
	for (const char *s = str, *end = s + len; s < end; s++) {
		const unsigned char c = (*s);
		if (is_visible_ascii_char(c)) {
			*dst++ = c;
		} else {
			dst[0] = '\\';
			dst[1] = 'x';
			dst[2] = nibble_to_hex[c >> 4];
			dst[3] = nibble_to_hex[c & 0xF];
			dst += 4;
		}
	}
	*dst = 0;
	return dst_buf;

}

const char *__log_to_human(const char *str, const size_t len) {
	static char buf[1<<10];
	return __log_to_human(str, len, buf, sizeof(buf));
}

uint64_t str2_long_no_less_then(const char* str, long lb) {
	const long rv = atol(str);
	return (rv < lb) ? (unsigned long)lb : rv;
}

size_t strncpy_no_trunc_warning(char* dst, const char* src, int len) {
	char* start = dst;
	for (const char* end = &src[len-1]; (src < end) && (*src); )
		*dst++ = *src++;
	*dst = 0x0;		// null termination
	return (dst - start);
}

time_stamp_t get_time_now(__maybe_unused char reset_to) {
	using namespace std::chrono;
	const auto ts = system_clock::now(); // Prefer C++ stdlib function to be more platform independent.
	const auto ts_sec = time_point_cast<seconds>(ts); // Cast to time in seconds
	const nanoseconds ns = ts - ts_sec; // Remainder of nanoseconds

	return { .sec = static_cast<uint32_t>(ts_sec.time_since_epoch().count()), .nanosec = (uint32_t)ns.count() };
	// return time(NULL);
}

/*****************************************************************************/
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <unistd.h>				// Set permissions of socket
#include <sys/stat.h>
void sock_t::epoll_reply_start(void) {
	BUG_ON(has_epoll_running(), "trying to create epoll for fd=%d a second time", _fd);
	epoll_fd = epoll_create1(0);
	ASSERT_IN_PRODUCTION(has_epoll_running());
	struct epoll_event ev = {.events = (EPOLLRDHUP | EPOLLERR | EPOLLHUP | EPOLLET | EPOLLOUT | EPOLLIN | EPOLLPRI), .data = {.fd = this->_fd} };
	const int epoll_rv = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, this->_fd, &ev);
	ASSERT_IN_PRODUCTION(epoll_rv >= 0);
}

void sock_t::epoll_reply_wait(__maybe_unused const char* debug_prefix) const {
	ASSERT_IN_PRODUCTION(has_epoll_running());
	struct epoll_event events[2];
	int n;	// n events in the array
	// pr_info("%s: polling...\n", debug_prefix);
	do {
		n = epoll_wait(epoll_fd, &events[0], ARRAY_SIZE(events), -1);	// -1 == no timeout
	} while ((n == -1) && (errno == EINTR));
	if (n > 0) {
		// pr_info("%s: %d events 0x%x\n", debug_prefix, n, events[0].events);	// xor between values of 'enum EPOLL_EVENTS'
	} else
		pr_err("got something: %d, " PRINT_EXTERN_ERR_FMT "\n", n, PRINT_EXTERN_ERR_ARGS);
}

void sock_t::epoll_reply_stop(void) {
	if (has_epoll_running()) {
		struct epoll_event ev = {.events = 0, .data = {.fd = this->_fd} };
		(void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, this->_fd, &ev);
		close(epoll_fd);
		epoll_fd = -1;
	}
}

void sock_t::__init_remote(uint32_t port, const char* ip, bool is_tcp, bool is_blocking, struct sockaddr_in& sin) {
	const int arg = 1;
	if (is_tcp)  {
		_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); _type = type::S_TCP;
	} else {
		_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);  _type = type::S_UDP;
	}
	if (_fd < 0) { pr_err("Failed to create socket type=%c: " PRINT_EXTERN_ERR_FMT "\n", _type, PRINT_EXTERN_ERR_ARGS);  return; }
	if (!is_blocking)
		ioctl(_fd, FIONBIO, &arg, sizeof(arg));
	sin.sin_family = AF_INET;
	if (ip)
		resolve_sin_addr(ip, &sin);						// Client connect to specific server
	else
		sin.sin_addr.s_addr = INADDR_ANY;				// Server listen to any client
	sin.sin_port = htons(port);
}

void sock_t::__init_local(const char* socket_path, bool is_blocking, struct sockaddr_un& addr) {
	const int arg = 1;
	_fd = socket(AF_UNIX, SOCK_STREAM, 0);  _type = type::S_UDS;
	if (_fd < 0) { pr_err("Failed to create socket type=%c: " PRINT_EXTERN_ERR_FMT "\n", _type, PRINT_EXTERN_ERR_ARGS);  return; }
	if (!is_blocking)
		ioctl(_fd, FIONBIO, &arg, sizeof(arg));
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
	if (_fd < 0) pr_err("Failed to create socket type=%c: " PRINT_EXTERN_ERR_FMT "\n", _type, PRINT_EXTERN_ERR_ARGS);
}

void sock_t::__bind_server(const struct connect_addr &ca) {
	int rv;
	if ((rv = bind(_fd, &ca.u.b, ca.get_len(is_remote()))) < 0) {
		const char* where = (is_remote()) ? "inet" : ca.u.u.sun_path;
		pr_err("Bind failed(%s) type=%c, rv=%d, " PRINT_EXTERN_ERR_FMT "\n", where, _type, rv, PRINT_EXTERN_ERR_ARGS);
		close(_fd); _fd = -1; return;
	}
	if (this->_type == type::S_UDS) {		// Allow sudo app to talk to non sudo app via unix domain socket
		static constexpr const mode_t all_mode = S_IRWXU | S_IRWXG | S_IRWXO;  // rwxrwxrwx
		pr_verbs("socket[%s].fd[%u].chmod[%o]\n", ca.u.u.sun_path, _fd, all_mode);
		//if (fchmod(_fd, blk_mode) < 0)	// Possibly do this on non uds
		if (chmod(ca.u.u.sun_path, all_mode) < 0)
			pr_err("socket[%s].fd[%u].chmod[%o] failed " PRINT_EXTERN_ERR_FMT "\n", ca.u.u.sun_path, _fd, all_mode, PRINT_EXTERN_ERR_ARGS);
	}
	if ((rv = listen(_fd, 256 /*Max clients*/)) < 0) {
		/*if (errno != EINTR) {
			pr_err("Listen failed type=%c, rv=%d, " PRINT_EXTERN_ERR_FMT "\n", _type, rv, PRINT_EXTERN_ERR_ARGS);
			close(_fd); _fd = -1; return;
		}*/
	}
}

int sock_t::srvr_listen(uint32_t port, bool is_tcp, struct connect_addr& ca, bool is_blocking) {
	const int arg = 1;
	__init_remote(port, NULL, is_tcp, is_blocking, ca.u.i);
	setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &arg, sizeof(arg));
	__bind_server(ca);
	return _fd;
}

int sock_t::srvr_listen(const char* socket_path, struct connect_addr& ca, bool is_blocking) {
	__init_local(socket_path, is_blocking, ca.u.u);
	if (_fd < 0)
		return -1;
	unlink(socket_path); // Remove existing socket file if any
	__bind_server(ca);
	if (!is_alive())
		unlink(socket_path);
	return _fd;
}

void sock_t::srvr_set_listen_timeout(uint32_t n_sec) {
	const struct timeval tv = {.tv_sec = n_sec, .tv_usec = 0 };
	setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
}

int sock_t::srvr_accept_clnt(struct connect_addr& ca) const {
	socklen_t sinlen = ca.get_len(is_remote());
	ASSERT_IN_PRODUCTION(uses_connection());
	return accept(_fd, &ca.u.b, &sinlen);
}

void sock_t::nice_close(bool force) {
	epoll_reply_stop();
	if (_fd >= 0) {
		if (force) shutdown(_fd, SHUT_RDWR);	// Stop all read/write on socket, existing data remains in the socket
		close(_fd); _fd = -1;
	}
}

int sock_t::resolve_sin_addr(const ip_addr_str ip, struct sockaddr_in *sin) const {
	int ip_found = inet_pton(sin->sin_family, ip, &sin->sin_addr);
	ASSERT_IN_PRODUCTION(is_remote());							// Why calling this function
	if (!ip_found) {
		pr_debug("Try resolving ip: %s\n", ip);
		struct hostent *hent = gethostbyname(ip);	// Maybe host was given
		if (hent) {
			struct in_addr **addr_list = (struct in_addr **) hent->h_addr_list;
			if (addr_list[0] != NULL) { // for (int i = 0; addr_list[i] != NULL; i++) {
				sin->sin_addr = *addr_list[0];
				ip_found = 1;
			}
		}
	}
	return ip_found;
}

int sock_t::__connct_clnt(const struct connect_addr &ca) {
	int rv = 0;
	epoll_reply_start();
	ASSERT_IN_PRODUCTION(uses_connection());
	do {
		epoll_reply_wait("client epoll connect");
		rv = connect(_fd, &ca.u.b, ca.get_len(is_remote()));
	} while (rv < 0 && (errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY));
	return rv;
}

int sock_t::clnt_connect_to_srvr_tcp(uint32_t port, const ip_addr_str ip, struct connect_addr& ca, bool is_blocking) {
	__init_remote(port, (const char*)ip, true /*is_tcp*/, is_blocking, ca.u.i);
	return __connct_clnt(ca);
}

int sock_t::clnt_connect_to_srvr_uds(const char* socket_path, struct connect_addr& ca, bool is_blocking) {
	__init_local(socket_path, is_blocking, ca.u.u);
	return __connct_clnt(ca);
}

int sock_t::clnt_connect_to_srvr_udp(uint32_t port, const ip_addr_str ip, struct connect_addr& ca, bool is_blocking) {
	__init_remote(port, (const char*)ip, false /*udp*/, is_blocking, ca.u.i);
	epoll_reply_start();
	return _fd;
}

void sock_t::print_address(char addr[32], const struct connect_addr& dst) const {
	if (is_remote()) {
		const struct sockaddr_in* sin = &dst.u.i;
		addr[0] = 0;
		inet_ntop(sin->sin_family, &sin->sin_addr, addr, 32);
		const int len = strlen(addr);
		snprintf(&addr[len], 32 - len, ":%u", htons(sin->sin_port));
	} else {
		const struct sockaddr_un* sin = &dst.u.u;
		strncpy_no_trunc_warning(addr, sin->sun_path, 32);
	}
}

ssize_t sock_t::send_msg(const void *buf, size_t len, const struct connect_addr& ca) const {
	if (is_remote()) {
		return sendto(_fd, buf, len, 0, &ca.u.b, ca.get_remote_len());
	} else {
		return write( _fd, buf, len);
	}
}

#include <sys/ioctl.h>
#include <fcntl.h>
void sock_t::set_blocking(bool is_blocking) {
	const int oldmode = fcntl(_fd, F_GETFL, NULL);
	const int newmode = (is_blocking ? (oldmode & (~O_NONBLOCK)) : (oldmode | O_NONBLOCK));
	if (oldmode < 0) {
		pr_err("Error getting fd[%d].blocking=%u, " PRINT_EXTERN_ERR_FMT "\n", _fd, is_blocking, PRINT_EXTERN_ERR_ARGS);
	} else if (fcntl(_fd, F_SETFL, newmode) < 0) {
		pr_err("Error setting fd[%d].flags0x[%x->%x], " PRINT_EXTERN_ERR_FMT "\n", _fd, oldmode, newmode, PRINT_EXTERN_ERR_ARGS);
	}
}

int readn(int fd, char *ptr, const int n, enum io_state *io_state) {
	int	nleft = n;
	*io_state = ios_ok;
	while (nleft > 0) {
		const int nread = read(fd, ptr, nleft);		// Alternatively use recv(fd, ptr, nleft, 0 /*MSG_DONTWAIT | MSG_NOSIGNAL*/);
		if (nread < 0) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				*io_state = ios_block;
				return (n - nleft);		// Partial read, buffer is empty
			} else {
				*io_state = ios_error;
				return -1;
			}
		} else if (nread == 0) {
			*io_state = ios_close;		// EOF read
			return (n - nleft);		// Partial read, socket was closed
		}
		nleft -= nread;
		ptr +=   nread;
	}
	return n;	// Full read succeeded
}

int writen(int fd, const char *ptr, const int n, enum io_state *io_state) {
	int nleft = n;
	while (nleft > 0) {
		const int nwritten = write(fd, ptr, nleft);		// Alternatively use send(fd, ptr, nleft, 0);
		if (nwritten <= 0) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN) {
				*io_state = ios_block;
				return (n - nleft);		// Partial write, buffer is full
			} else {
				*io_state = ios_error;
				return -1;
			}
		}
		nleft -= nwritten;
		ptr +=   nwritten;
	}
	return n;	// Full write succeeded
}

int thread_api_bound_to_1_core(uint16_t core_idx) {
	cpu_set_t cpu_set;
	CPU_ZERO(&cpu_set);
	CPU_SET(core_idx, &cpu_set);
	return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
}

int thread_api_set_high_cpu_usage(const char *thread_name) {
	if (false) {		// change to true for NVMESH-3892, db flush get stuck
		struct sched_param param;
		int p = -1;
		int rv = pthread_getschedparam(pthread_self(), &p, &param);
		if (rv != 0) {
			pr_err("%s: Cannot get sched_policy. rv=%d, " PRINT_EXTERN_ERR_FMT "\n", thread_name, rv, PRINT_EXTERN_ERR_ARGS);
			return -1;
		}
		pr_info("%s: policy=%s(%u), priority=%d\n", thread_name, (p == SCHED_FIFO) ? "SCHED_FIFO" : (p == SCHED_RR) ? "SCHED_RR" : (p == SCHED_OTHER) ? "SCHED_OTHER" : "???", p, param.sched_priority);
		param.sched_priority = 98;  // Priority range: 1 (low) to 99 (high) for real-time
		rv = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
		if (rv != 0) {
			pr_err("%s: Cannot set sched_policy. rv=%d, " PRINT_EXTERN_ERR_FMT "\n", thread_name, rv, PRINT_EXTERN_ERR_ARGS);
			return -1;
		}
		pr_note("%s: set policy=SCHED_FIFO, priority=%d\n", thread_name, param.sched_priority);
	}
	return 0;
}

static inline void __log_io_buf_size(unsigned prev, unsigned rv, unsigned len, const char* what) {
	if (rv < len) { pr_err( "Setting %s[b]: %u -> %u, rv=%u, degraded performance...\n", what, prev, len, rv);
	} else		    pr_info("Setting %s[b]: %u -> %u, rv=%u\n", what, prev, len, rv);
}
void sock_t::set_io_buffer_size(unsigned rlen, unsigned wlen) {
	unsigned prev = 0, rv = rlen;
	socklen_t optlen = sizeof(rlen);
	static constexpr const char* sock_opt_err = "Cannot sockopt. " PRINT_EXTERN_ERR_FMT "\n";
	BUG_ON(getsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &prev, &optlen) < 0, sock_opt_err, PRINT_EXTERN_ERR_ARGS);
	BUG_ON(setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &rlen,  optlen) < 0, sock_opt_err, PRINT_EXTERN_ERR_ARGS);
	BUG_ON(getsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &rv  , &optlen) < 0, sock_opt_err, PRINT_EXTERN_ERR_ARGS);
	__log_io_buf_size(prev, rv, rlen, "read_buf");
	BUG_ON(getsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &prev, &optlen) < 0, sock_opt_err, PRINT_EXTERN_ERR_ARGS);
	BUG_ON(setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &wlen,  optlen) < 0, sock_opt_err, PRINT_EXTERN_ERR_ARGS);
	BUG_ON(getsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &rv  , &optlen) < 0, sock_opt_err, PRINT_EXTERN_ERR_ARGS);
	__log_io_buf_size(prev, rv, wlen, "writ_buf");
}

#include <netinet/tcp.h>
void sock_t::set_optimize_for_latency(bool do_optimize) {
	int optval = do_optimize ? 1 : 0;	// Disable TCP nagel algorithm, dont gather multiple small packets, but send them immediatly
	if (setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0)
		pr_emerg("Unable to turn off NAGEL-TCP algorithm, latency can be high!\n");
}

/*****************************************************************************/
tDbg& tDbg::get(void) {
	static class tDbg dbg;
	return dbg;
}

// Determine if running with debugger
#include <fcntl.h>	// reading proc file
#include <sys/stat.h>
int tDbg::is_debugger_present(void) {
	if (debugger_pid >= 0)
		return debugger_pid; // Already called this func
	char buf[4096];
	int rv = 0, status_fd = open("/proc/self/status", O_RDONLY);
	if (status_fd == -1)
		return rv; // Cant read /proc?
	const ssize_t num_read = read(status_fd, buf, sizeof(buf));
	if (num_read <= 0)
		return rv;
	buf[num_read] = 0;
	// pr_info("%s\n",buf);
	static constexpr const char t_pid[] = "TracerPid:";
	const char *debugger_pid_str = strstr(buf, t_pid);
	if (debugger_pid_str)
		debugger_pid = !!atoi(debugger_pid_str + sizeof(t_pid) - 1);
	return debugger_pid;
}

#include <signal.h>		// raise signal for breakpoints
void tDbg::break_point(void) {
	if (is_debugger_present()) {
		raise(SIGTRAP); // Do not create core dump. Just break point in debugger
	} else { // Create core dump to debug later.
		#ifdef WINDOWS_PHONE
			__breakpoint();
		#else
			raise(SIGABRT);
		#endif
	}
}

// Taken from http://man7.org/linux/man-pages/man3/backtrace.3.html
#include <execinfo.h>	// For stack dump
void tDbg::dump_stack(void) {
	#define SIZE 200
	void *buffer[SIZE];
	int j, nptrs = backtrace(buffer, SIZE);
	char **strings = backtrace_symbols(buffer, nptrs);
	if (strings == NULL) {
		pr_emerg("No crash stack available..\n");
		return;
	}
	pr_emerg("Crash stack of depth %d\n", nptrs);
	for (j = 0; j < nptrs; j++)
		pr_emerg("\t%s\n", strings[j]);
	free(strings);
}

#include <stdarg.h>				// va_list
void tDbg::invoke_crash(const char* condition, const char* func_name, const char* file_name, int line_num, const char* fmt, ...) {
	tDbg& d = tDbg::get();
	char now_str[32];
	d.get_cur_timestamp_str(now_str);
	pr_emerg("------------[ cut here ]------------\nBUG!!!! at %s, %s() %s[%d]: (%s) ", now_str, func_name, file_name, line_num, condition);
	if (fmt[0]) { // print the reason
		fprintf(d.flog, "Reason: ");
		va_list ap;
		va_start(ap, fmt);
		vfprintf(d.flog, fmt, ap);
		va_end(ap);
	}
	fprintf(d.flog, "\n");
	d.dump_stack();
	fflush(d.flog);
	d.break_point();
}

int tDbg::print(const char* fmt, ...) {
	int rv = 0;
	tDbg& d = tDbg::get();
	va_list ap;
	va_start(ap, fmt);
	rv = vfprintf(d.flog, fmt, ap);
	va_end(ap);
	// d.flog_flush(); // Enable when debugging a crash to see latest prints
	return rv;
}

void tDbg::flog_flush(void) {
	#ifndef NDEBUG
		const tDbg& d = tDbg::get();
		fflush(d.flog);		// In release compilation, flushing logs is too heavy to execute
	#endif
}

#include <sys/time.h>
#include <time.h>			// strftime
int tDbg::get_timestamp_str(char rv[32], const uint32_t unix_epoch_sec) {
	const time_t nowtime = unix_epoch_sec;
	struct tm tmInfo;
	localtime_r(&nowtime, &tmInfo);
	return strftime(rv, 32, "%Y.%m.%d-%H:%M:%S", &tmInfo);
}

int tDbg::get_cur_timestamp_str(char rv[32]) {
	struct timeval tv;	// https://en.cppreference.com/w/c/chrono/localtime
	gettimeofday(&tv, NULL);
	int n_bytes = get_timestamp_str(rv, tv.tv_sec);
	n_bytes += snprintf(&rv[n_bytes], (32-n_bytes), ".%06lu", tv.tv_usec);
	return n_bytes;
}

uint64_t tDbg::get_cur_timestamp_unix(void) {
  struct timeval tp;
  gettimeofday(&tp, nullptr);
  return (uint64_t)(tp.tv_sec * 1000000 + tp.tv_usec);
}

/*****************************************************************************/
static uint32_t crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

uint32_t crc32_le_unoptimized(uint32_t crc, const unsigned char *p, size_t size) {
	crc = crc ^ ~0U;
	while (size--)
		crc = crc32_tab[(crc^*p++)&0xFF]^(crc >> 8);
	return crc ^ ~0U;
}

/*****************************************************************************/
static void pr_start_lib(const char *exe_name, int argc, const char* const argv[], unsigned pid) {
	#ifdef NDEBUG
		static constexpr const char *opt_level = "RELEASE";
	#else
		static constexpr const char *opt_level = "DEBUG";
	#endif
	static constexpr const char *comp_date = __stringify(COMPILATION_DATE);
	pr_note("%s: %s, git=%s(" /*"%s:"*/ "0x%lx), TraceLevel=%d, opt=%s, pid=%u ", exe_name, comp_date, __stringify(VER_TAGID), /*__stringify(BRANCH_NAME),*/ COMMIT_ID, TRACE_LEVEL, opt_level, pid);
	const bool is_sandbox_make_file = (comp_date[0] == 0x1b);	// Red warning
	const bool is_sandbox_cpph_file = is_running_in_sandbox();
	BUG_ON(is_sandbox_make_file != is_sandbox_cpph_file, "Sandbox Compilation error, do clean rebuild! {Makefile=%u cpp=%u, 1char=%u}\n", is_sandbox_make_file, is_sandbox_cpph_file, comp_date[0]);
	if (argc) {
		pr_note("%d[args]: ",argc);
		for (int i = 0; i < argc; i++) {
			pr_note("|%s| ", argv[i]);
		}
	}
	pr_note("\n");
}

base_library::base_library(const char *_lib_name) : lib_name(_lib_name), shutting_down(false) {
	memset(lib_info_json, 0, sizeof(lib_info_json));
	pid = getpid();
	pr_start_lib(_lib_name, 0, NULL, pid);
}

bool base_library::have_permissions_to_run(void) const {
	if (false && (!is_running_in_sandbox()) && getuid()) {	// Currently no need for root access
		pr_emerg("%s: must run as root!\n", lib_name);
		return false;
	}
	return true;
}

int base_library::start(void) {
	if (!have_permissions_to_run())
		return -1;
	if ((int)sysconf(_SC_PAGESIZE) < 4096) {
		pr_emerg("%s: Cannot work with small pages %d[b]!\n", lib_name, (int)sysconf(_SC_PAGESIZE));
		return -2;
	}
	return 0;
}

int base_library::finish(const char* prefix, int rv) {
	const __pid_t cur_pid = getpid();
	memset(lib_info_json, 0, sizeof(lib_info_json));
	if (pid != cur_pid) {
		pr_emerg("%s: Error: library initialized from pid %u but destroyed from pid=%u\n", lib_name, pid, cur_pid);
		rv = -5;
	}
	pr_note("%s%s: destroyed, pid=%u " NV_COL_R "rv=%d\n\n", prefix, lib_name, pid, rv);
	return rv;
}

/*****************************************************************************/
// EOF.
