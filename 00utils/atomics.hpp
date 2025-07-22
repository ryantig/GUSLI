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
#include <pthread.h>
#include <stdint.h>		// uint64_t, uint32_t and such
#include "utils.hpp"		// BUG_ON
/* __atomic_* is gcc-specific, and conforms C++11 memory model anyway. Not usings std::atomic, as it does not fit cooperative threads framework" */
template<class T> class atomic {
	T v;
 public:
	atomic() : v(0) {}
	atomic(T new_val) : v(new_val) {}
	inline void set(T new_val) { __atomic_store_n(&v, new_val, __ATOMIC_SEQ_CST); }
	inline T read(void) const { return __atomic_load_n(&v, __ATOMIC_SEQ_CST); }
	inline T inc(void) { /* (++(*v)); */ return __atomic_add_fetch(&v, 1, __ATOMIC_SEQ_CST); }
	inline T dec(void) { /* (--(*v)); */ return __atomic_sub_fetch(&v, 1, __ATOMIC_SEQ_CST); }
	inline T xchg(T new_val) {return __atomic_exchange_n(&v, new_val, __ATOMIC_SEQ_CST); } // x = v; v = new_val; return x
	inline T cmpxchg(T old_val, T new_val) {
		T tmp = old_val;
		__atomic_compare_exchange_n(&v, &tmp, new_val, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		return tmp;
	} // v = ((v==o) ? n : o); return v; }
};
typedef atomic<uint32_t> atomic_uint32_t, atomic_t;
typedef atomic<uint64_t> atomic_uint64_t;

/************************************ Locks **********************************/
class t_lock_mutex {			// Please do not use those, they extremely slow down performance and cannot be integrated with SPDK
	pthread_mutex_t m;			// Non recursive mutex
 public:
	void init(void) {          pthread_mutex_init(   &m, NULL); }	// Todo: Consider using adaptive mutex: PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
	void lock(void) {   BUG_ON(pthread_mutex_lock(   &m) != 0, "Failed to lock!"); }
	void unlock(void) { BUG_ON(pthread_mutex_unlock( &m) != 0, "Failed to unlock!"); }
	void destroy(void) {       pthread_mutex_destroy(&m); }
	bool is_locked(void) { if (pthread_mutex_trylock(&m) == 0) { unlock(); return false; } return true; }
};

class t_lock_mutex_recursive {	// Please do not use those, they extremely slow down performance and cannot be integrated with SPDK
	pthread_mutex_t m;			// Recursive mutex from the same thread
 public:
	void init(void) {
		pthread_mutexattr_t attr;
		BUG_ON(pthread_mutexattr_init(&attr) != 0, "Failed to init mutex attributes");
		BUG_ON(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0, "Failed to set recursive mutex attributes");
		BUG_ON(pthread_mutex_init(&m, &attr) != 0, "Failed to create mutex");
	}
	void lock(void) {   BUG_ON(pthread_mutex_lock(   &m) != 0, "Failed to lock!"); }
	void unlock(void) { BUG_ON(pthread_mutex_unlock( &m) != 0, "Failed to unlock!"); }
	void destroy(void) {       pthread_mutex_destroy(&m); }
	bool is_locked(void) { if (pthread_mutex_trylock(&m) == 0) { unlock(); return false; } return true; }
};

class t_lock_spinlock {			// For fast very short critical sections
	pthread_spinlock_t m;
 public:
	void init(void) {          pthread_spin_init(   &m, PTHREAD_PROCESS_PRIVATE); }
	void lock(void) {   BUG_ON(pthread_spin_lock(   &m) != 0, "Failed to lock!"); }
	void unlock(void) { BUG_ON(pthread_spin_unlock( &m) != 0, "Failed to unlock!"); }
	void destroy(void) {       pthread_spin_destroy(&m); }
	bool is_locked(void) { if (pthread_spin_trylock(&m) == 0) { unlock(); return false; } return true; }
};

class t_no_lock {				// Dumy no lock, which does not protect from race conditions. Used when another (external) mechanism/algorithm guarantees atomicity
	bool _is_locked;
 public:
	void init(void) {   _is_locked = false; }
	void lock(void) {   _is_locked = true; }
	void unlock(void) { _is_locked = false; }
	void destroy(void) { }
	bool is_locked(void) { return _is_locked; }
};

template <class T>
class t_lock_guard {
	T& _lock;
 public:
	explicit t_lock_guard(T& lock) : _lock(lock) { _lock.lock(); }
	~t_lock_guard() { _lock.unlock(); }
};

/************************************ Serializer **********************************/
class t_serializer {		// Launch async task and wait for its rv (return value). Poor performance, Dont use in run time code!
	pthread_cond_t gcond = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;
	int rv;
 public:
	void init(void) {    pthread_mutex_lock(&glock); }
	void destroy(void) { pthread_mutex_unlock(&glock); }
	int wait_for_async_rv(void) {			// Caller thread: Launch async task and wait
		pthread_cond_wait(&gcond, &glock);
		return rv;
	}
	void wakeup_with_rv(int _rv) {			// Async task thread: Notify about completion
		rv = _rv;
		pthread_cond_signal(&gcond);
	}
};

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
class t_shared_mem {
	char* name;				// Name of the file in /dev/shm. Producer saves it for freeing shared memory
	void *buf;				// Shared memory itself
	size_t n_bytes;			// Length of the buffer
	bool is_external_buf;	// Was buffer given externally or internally mapped
	void debug_print(const char* ref) const {
		static constexpr const bool verbose = false;	// Use for debugging
		if (verbose && buf) printf("%p, %s%s, who=%s\n", buf, name, ref, (name != NULL ? "producer" : "consumer"));
	}
	void _destroy(void) {
		debug_print("--");
		if (name) { shm_unlink(name); free(name); name = NULL; }
		if (buf)  { if (!is_external_buf) munmap(buf, n_bytes); buf = NULL; }
		n_bytes = 0;
	}
	void unsafe_clear(void) { memset((void*)this, 0, sizeof(*this)); }
	int _init(const char* _name, size_t _n_bytes, bool is_producer, void* external_buf) {
		int rv = 0;
		if (is_producer) {
			shm_unlink(_name);
			name = strdup(_name);
		}
		const int o_flag = (is_producer ? (O_CREAT /*| O_EXCL*/) : 0);
		const int fd = shm_open(_name, O_RDWR | o_flag, 0666);
		n_bytes = _n_bytes;
		if (fd <= 0) {
			pr_err("shm_open |%s|" PRINT_EXTERN_ERR_FMT "\n", _name, PRINT_EXTERN_ERR_ARGS); rv = -__LINE__; goto __out;
		}
		if (ftruncate(fd, n_bytes) < 0) {	    // Set the size of the shared memory object
			pr_err("ftruncate %lu[b] " PRINT_EXTERN_ERR_FMT "\n", n_bytes, PRINT_EXTERN_ERR_ARGS); rv = -__LINE__; goto __out;
		}
		if (external_buf) {
			void* _map = mmap(external_buf, n_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
			if (_map == MAP_FAILED) {
				pr_err("mmap fixed" PRINT_EXTERN_ERR_FMT "\n", PRINT_EXTERN_ERR_ARGS); rv = -__LINE__; goto __out;
			}
			is_external_buf = true;
			buf = external_buf;
		} else {
			buf = mmap(0, n_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (buf == MAP_FAILED) {
				pr_err("mmap " PRINT_EXTERN_ERR_FMT "\n", PRINT_EXTERN_ERR_ARGS); rv = -__LINE__; goto __out;
			}
		}
	__out:
		if (fd > 0)
			close(fd);
		if (rv)
			_destroy();
		debug_print("++");
		return rv;
	}
 public:
	t_shared_mem() : name(NULL), buf(NULL), n_bytes(0), is_external_buf(false) { }
	t_shared_mem(t_shared_mem&& o) noexcept : name(o.name), buf(o.buf), n_bytes(o.n_bytes), is_external_buf(o.is_external_buf) { o.unsafe_clear(); }	// Allow inserting class into container
	t_shared_mem& operator=(t_shared_mem&& o) {			// Allow removal of elements from a container
		debug_print("==");
		if (this != &o) {
			this->_destroy();
			memcpy((void*)this, &o, sizeof(o));
			o.unsafe_clear();
		}
		return *this;
	}
	~t_shared_mem() { _destroy(); }
	int init_producer(const char* _name, size_t _n_bytes, void* external_buf = NULL) { return _init(_name, _n_bytes, true,  external_buf); }
	int init_consumer(const char* _name, size_t _n_bytes, void* external_buf = NULL) { return _init(_name, _n_bytes, false, external_buf); }
	void* get_buf(void) const { return buf; }
	const char* get_producer_name(void) const { return name; }
	bool is_mapped(const void* ptr, size_t len = 0) const { return (ptr >= buf) && (((size_t)ptr + len) <= ((size_t)buf + n_bytes)); }
	bool is_exact_mapped(const void* ptr, size_t len = 0) const { return (ptr == buf) && (len == n_bytes); }
};
