#pragma once
#include "gusli_server_api.hpp"
#include "sample_code.hpp"
#include <unistd.h>		// close()
#include <fcntl.h>		// open()
#include <pthread.h>	// pthread_self()

/* Example of a backend io execution server which is
	1. read-only (does not support writes)
	2. Backed up by an empty file, in local dir of the executable
	3. First 8 bytes of each block store the address of that block
		so unit-tests can verify the correctness of reads
	4. Completes io's immediately (syncronously)
*/
class server_ro_lba {
	static void _1_map_fill(const gusli::io_map_t &m, uint32_t block_size_bytes) {
		for (uint64_t b = 0; b < m.data.byte_len; b += block_size_bytes) {
			uint64_t *dst = (uint64_t*)((uint64_t)m.data.ptr + b);
			*dst = (m.offset_lba_bytes + b);
		}
	}
	static void _mmio_fill(const gusli::io_multi_map_t* mio, uint32_t block_size_bytes) {
		for (uint32_t i = 0; i < mio->n_entries; i++)
			_1_map_fill(mio->entries[i], block_size_bytes);
	}
 public: // Unit-test environment api to fill and verify content of io
	static void test_1_map_verify_and_clean(const gusli::io_map_t &m, uint32_t block_size_bytes) {
		for (uint64_t b = 0; b < m.data.byte_len; b += block_size_bytes) {
			uint64_t *dst = (uint64_t*)((uint64_t)m.data.ptr + b);
			my_assert(*dst == (m.offset_lba_bytes + b));
			*dst = -1;		// Future reads must execute to access the data again
		}
	}
	static void test_mmio_verify_and_clean(const gusli::io_multi_map_t* mio, uint32_t block_size_bytes) {
		for (uint32_t i = 0; i < mio->n_entries; i++)
			test_1_map_verify_and_clean(mio->entries[i], block_size_bytes);
	}
	static void test_mmio_print(const gusli::io_multi_map_t* mio, const char* prefix) {
		log("\t%s: mio=%p, size=0x%lx, buf_size=0x%lx\n", prefix, mio, mio->my_size(), mio->buf_size());
		for (uint32_t i = 0; i < mio->n_entries; i++) {
			const gusli::io_map_t& m = mio->entries[i];
			log("\t\t%u) len=0x%lx[b], off=0x%lx[b], %p\n", i, m.data.byte_len, m.offset_lba_bytes, m.data.ptr);
		}
	}
 private:
	#define dslog(s, fmt, ...) ({ _log("%s: " fmt, (s)->binfo.name, ##__VA_ARGS__); })
	gusli::global_srvr_context::init_params p;
	gusli::bdev_info binfo = gusli::bdev_info{ .bdev_descriptor = -1, .block_size = 4096, .num_total_blocks = (1 << 30), .name = "", .num_max_inflight_io = 255, .reserved = 'r' };
	static gusli::bdev_info open1(void *ctx, const char* who) {
		server_ro_lba *me = (server_ro_lba*)ctx;
		me->binfo.bdev_descriptor = open(me->binfo.name, O_RDWR | O_CREAT | O_LARGEFILE, (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
		my_assert(me->binfo.bdev_descriptor > 0);
		dslog(me, "open: fd=%d, remote client=%s\n", me->binfo.bdev_descriptor, who);
		my_assert(strcmp(who, UNITEST_CLNT_NAME) == 0);
		return me->binfo;
	}
	static int close1(void *ctx, const char* who) {
		server_ro_lba *me = (server_ro_lba*)ctx;
		const int prev_fd = me->binfo.bdev_descriptor;
		if (me->binfo.is_valid()) {
			close(me->binfo.bdev_descriptor);
			me->binfo.bdev_descriptor = 0;
		}
		const int rv = remove(me->binfo.name);
		my_assert(rv >= 0);
		dslog(me, "close: fd=%d, rv=%d, who=%s\n", prev_fd, rv, who);
		return 0;
	}
	static void exec_io(void *ctx, class gusli::server_io_req& io) {
		server_ro_lba *me = (server_ro_lba *)ctx;
		my_assert(io.has_callback());			// Do not support io without callback for now
		io.start_execution();
		if (io.params.op == gusli::io_type::G_WRITE) {
			io.set_error(gusli::E_BACKEND_FAULT);
		} else if (io.params.num_ranges() <= 1) {
			_1_map_fill(io.params.map, me->binfo.block_size);
		} else {
			const gusli::io_multi_map_t* mio = io.get_multi_map();
			dslog(me, "Serving IO: #rng = %u, buf_size=%lu[b]\n", io.params.num_ranges(), io.params.buf_size());
			test_mmio_print(mio, me->p.server_name);
			_mmio_fill(mio, me->binfo.block_size);
		}
		io.set_success(io.params.buf_size());
	}
	#undef dslog
 public:
	server_ro_lba(const char* _name, const char* listen_addr) {
		strncpy(p.listen_address, listen_addr, sizeof(p.listen_address)-1);
		p.log = stderr,	p.server_name = "USRV",
		p.vfuncs = {.caller_context = this, .open1 = server_ro_lba::open1, .close1 = server_ro_lba::close1, .exec_io = server_ro_lba::exec_io };
		snprintf(binfo.name, sizeof(binfo.name), "%s%s", gusli::global_clnt_context::thread_names_prefix, _name);
	}
	void run(void) {
		const int rename_rv = pthread_setname_np(pthread_self(), binfo.name);	// For debug, set its thread to block device name
		my_assert(rename_rv == 0);
		gusli::global_srvr_raii srvr(p);
		my_assert(srvr.BREAKING_VERSION == 1);
		my_assert(srvr.run() >= 0);
	}
};
