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
extern "C" {		// Spdk includes
	#include "spdk/stdinc.h"
	#include "spdk/thread.h"
	#include "spdk/bdev.h"
	#include "spdk/env.h"
	#include "spdk/event.h"
	#include "spdk/log.h"
	#include "spdk/string.h"
	#include "spdk/bdev_zone.h"
};
#include "gusli_server_api.hpp"
#include "../common.hpp"
/* Example of a backend io execution server which is
	1. based on spdk block device
*/

/***************************** spdk block device ***************************************/
class backend_dev_t {
 public:
	struct spdk_bdev       *bdev = NULL;
	struct spdk_bdev_desc  *bdev_desc = NULL;
	struct spdk_io_channel *bdev_io_channel = NULL;
	const char             *bdev_name = NULL;	// Will be set as spdk_bdev_get_name(bdev)
	class server_spdk_ram  *my_srvr = NULL;
	struct spdk_bdev_io_wait_entry retry;
	void try_later(void (*work)(void *)) {
		SPDK_NOTICELOG("%s: Queueing work %p\n", bdev_name, work);
		retry.bdev = bdev;
		retry.cb_fn = work;
		retry.cb_arg = (void*)this;
		spdk_bdev_queue_io_wait(bdev, bdev_io_channel, &retry);
	}

	static void usage_help(void) { printf(" -b <bdev>                 name of the bdev to use\n"); }
	static int parse_arg(int ch, char *arg) {
		SPDK_DEBUGLOG("param: %c=%s\n", ch, arg); (void)ch; (void)arg;
		switch (ch) {
			case 'b':	/*this->bdev_name = arg;*/ return 0;
			default: return -EINVAL;
		}
	}
	void disconnect(void) {
		if (bdev_io_channel) {
			spdk_put_io_channel(bdev_io_channel);
			bdev_io_channel = NULL;
		}
		if (bdev_desc) {
			spdk_bdev_close(bdev_desc);
			bdev_desc = NULL;
		}
	}
	static void reset_zone_cb(struct spdk_bdev_io *bdev_io, bool success, void *ctx) {
		backend_dev_t *me = (backend_dev_t *)ctx;
		spdk_bdev_free_io(bdev_io);
		if (!success)
			SPDK_ERRLOG("%s: bdev io reset zone error: %d\n", me->bdev_name, EIO);
	}
	static void reset_zone(void *ctx) {
		backend_dev_t *me = (backend_dev_t *)ctx;
		int rv = spdk_bdev_zone_management(me->bdev_desc, me->bdev_io_channel, 0, SPDK_BDEV_ZONE_RESET, reset_zone_cb, me);
		if (rv == -ENOMEM) {
			me->try_later(reset_zone);
		} else if (rv) {
			SPDK_ERRLOG("%s: error %s, while resetting zone: %d\n", me->bdev_name, spdk_strerror(-rv), rv);
		}
	}
};

class server_spdk_ram : private gusli::srvr_backend_bdev_api {
	#define dslog(s, fmt, ...) ({ SPDK_NOTICELOG("%s: " fmt, (s)->binfo.name, ##__VA_ARGS__); })
	#define dserr(s, fmt, ...) ({ SPDK_ERRLOG(   "%s: " fmt, (s)->binfo.name, ##__VA_ARGS__); })
	#define PRINT_IO_REQ_FMT   "IO[%c|%p].#rng[%u].size[%lu[b]]"
	#define PRINT_IO_REQ_ARGS(io)  (io).params.op(), (&io), io.params.num_ranges(), io.params.buf_size()

	gusli::bdev_info binfo;
	backend_dev_t back;
	static void bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx) {
		server_spdk_ram *me = (server_spdk_ram*)ctx;
		dslog(me, "Unsupported bdev event: type=%d, bdev=%p\n", type, bdev);
	}

	gusli::bdev_info open1(const char* who) override {
		const int rv = spdk_bdev_open_ext(back.bdev_name, true /*write*/, server_spdk_ram::bdev_event_cb, this, &back.bdev_desc);
		if (rv != 0) {
			binfo.bdev_descriptor = -1;
			dserr(this, "Open bdev %s - XX, client=%s, fd=%d, rv=%d\n", back.bdev_name, who, binfo.bdev_descriptor, rv);
			return binfo;
		}
		struct spdk_bdev *b = back.bdev = spdk_bdev_desc_get_bdev(back.bdev_desc);
		binfo.bdev_descriptor = 555;	// Just some number, take from spdk for debug
		binfo.block_size = spdk_bdev_get_block_size(b);
		binfo.num_total_blocks = spdk_bdev_get_num_blocks(b); // spdk_bdev_get_write_unit_size(b);
		binfo.num_max_inflight_io = MAX_SERVER_IN_FLIGHT_IO;
		const uint32_t buf_align = spdk_bdev_get_buf_align(b), reminder = (binfo.block_size % buf_align);
		my_assert((buf_align <= binfo.block_size));		// Client operates in block sizes
		my_assert(reminder == 0);
		dslog(this, "Open bdev %s - OK, client=%s, fd=%d, align=0x%x[b] totalblks=%lu rv=%d\n", back.bdev_name, who, binfo.bdev_descriptor, buf_align, binfo.num_total_blocks, rv);
		back.bdev_io_channel = spdk_bdev_get_io_channel(back.bdev_desc);
		if (back.bdev_io_channel == NULL) {
			dserr(this, "Could not create bdev I/O channel, closing!!\n");
			close1(who);
		}
		if (spdk_bdev_is_zoned(back.bdev)) {
			dslog(this, "Zoned bdev %s - will reset zone\n", back.bdev_name);
			back.reset_zone(back.bdev);
		}
		return binfo;
	}

	int close1(const char* who) override {
		const int prev_fd = binfo.bdev_descriptor;
		back.disconnect();
		if (binfo.is_valid()) {
			binfo.bdev_descriptor = -1;
		}
		dslog(this, "close bdev %s - OK, client=%s, fd=%d\n", back.bdev_name, who, prev_fd);
		return 0;
	}

	struct io_ranges_counter {							// Class is 64[bits] so use in in place of the pointer, instead of allocate/deallocate
		uint32_t n_remaining;
		uint32_t n_failed;
		io_ranges_counter(gusli::server_io_req& io) : n_remaining(io.params.num_ranges()), n_failed(0) {}
	};
	static void cmp_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *io_ptr) {
		gusli::server_io_req& io = *(gusli::server_io_req*)io_ptr;
		io_ranges_counter* exec = (io_ranges_counter*)io.get_private_exec_u64_addr();
		SPDK_NOTICELOG(PRINT_IO_REQ_FMT " Callback range ok=%u, remaining=%u\n", PRINT_IO_REQ_ARGS(io), success, exec->n_remaining);
		if (!success)
			exec->n_failed++;
		if (--exec->n_remaining == 0) {					// No need for atomic counter as all on same thread
			if (exec->n_failed == 0) {
				*io.get_private_exec_u64_addr()  = NULL;
				io.set_success(io.params.buf_size());
			} else {
				SPDK_ERRLOG(PRINT_IO_REQ_FMT " io error=%d, num_ranges_failed=%u\n", PRINT_IO_REQ_ARGS(io), EIO, exec->n_failed);
				*io.get_private_exec_u64_addr()  = NULL;
				io.set_error(gusli::io_error_codes::E_PERM_FAIL_NO_RETRY);
			}
		}
		if (bdev_io) spdk_bdev_free_io(bdev_io);
	}

	void exec_io(gusli::server_io_req& io) override {
		my_assert(io.params.has_callback());			// Do not support io without callback for now
		io.start_execution();
		*((io_ranges_counter*)io.get_private_exec_u64_addr()) = io_ranges_counter(io);	// Attach ranges execution to io
		auto *desc = back.bdev_desc;
		auto *chan = back.bdev_io_channel;
		dslog(this, PRINT_IO_REQ_FMT " Serving\n", PRINT_IO_REQ_ARGS(io));
		int (*const exec_fn)(spdk_bdev_desc *, spdk_io_channel *, void *, uint64_t, uint64_t, spdk_bdev_io_completion_cb, void *) =
					(io.params.op() == gusli::G_READ) ? spdk_bdev_read : spdk_bdev_write;
		if (io.params.num_ranges() <= 1) {
				const gusli::io_map_t &map = io.params.map();
				const int rv = exec_fn(desc, chan, map.data.ptr, map.offset_lba_bytes, map.data.byte_len, cmp_io_cb, &io);
				if (rv != 0) {
					dserr(this, PRINT_IO_REQ_FMT " io exec range[%u] error=%d (%s)\n", PRINT_IO_REQ_ARGS(io), 0, rv, spdk_strerror(-rv));
					cmp_io_cb(NULL, false, &io);
				}
		} else {
			const gusli::io_multi_map_t* mio = io.get_multi_map();
			for (uint32_t i = 0; i < mio->n_entries; i++) {
				const gusli::io_map_t &map = mio->entries[i];
				const int rv = exec_fn(desc, chan, map.data.ptr, map.offset_lba_bytes, map.data.byte_len, cmp_io_cb, &io);
				if (rv != 0) {
					dserr(this, PRINT_IO_REQ_FMT " io exec range[%u] error=%d (%s)\n", PRINT_IO_REQ_ARGS(io), i, rv, spdk_strerror(-rv));
					cmp_io_cb(NULL, false, &io);
				}
			}
		}
	}
 public:
	int last_run_once_rv = 0;					// Stores last run once rv for debugging, not mandatory
	server_spdk_ram(const char* _name, const char* listen_addr) {
		my_assert(this->BREAKING_VERSION == 1);
		strncpy(par.listen_address, listen_addr, sizeof(par.listen_address)-1);
		par.log = stderr;
		par.server_name = "SSRV";
		par.has_external_polling_loop = true;
		par.use_blocking_client_accept = false;	// Must do this if you want to run > 1 server on the same cpu core (spdk thread)
		binfo.clear();
		snprintf(binfo.name, sizeof(binfo.name), "%s%s", gusli::global_clnt_context::thread_names_prefix, _name);
		back.bdev_name = _name;
		back.my_srvr = this;
		const int rename_rv = pthread_setname_np(pthread_self(), binfo.name);	// For debug, set its thread to block device name
		my_assert(rename_rv == 0);
		dslog(this, "construced, metadata=|%s|\n", create_and_get_metadata_json());
	}
	int run_once(void) { return last_run_once_rv = gusli::srvr_backend_bdev_api::run(); }
	virtual ~server_spdk_ram() { dslog(this, "Destructor\n"); }
	#undef dslog
	#undef dserr
};
