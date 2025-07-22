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
	static constexpr const char* default_bdev_name = "dhs_bdev";
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

class server_spdk_ram {
	#define dslog(s, fmt, ...) ({ SPDK_NOTICELOG("%s: " fmt, (s)->binfo.name, ##__VA_ARGS__); })
	#define dserr(s, fmt, ...) ({ SPDK_ERRLOG(   "%s: " fmt, (s)->binfo.name, ##__VA_ARGS__); })
	gusli::global_srvr_context::init_params p;
	gusli::bdev_info binfo = gusli::bdev_info{ .bdev_descriptor = -1, .block_size = 0, .num_total_blocks = 0, .name = "", .num_max_inflight_io = MAX_SERVER_IN_FLIGHT_IO, .reserved = 's' };
	struct backend_dev_t back;
	gusli::global_srvr_raii *gs = NULL;
	static void bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx) {
		SPDK_NOTICELOG("Unsupported bdev event: type=%d, bdev=%p, ctx=%p\n", type, bdev, ctx);
	}

	static gusli::bdev_info open1(void *ctx, const char* who) {
		server_spdk_ram *me = (server_spdk_ram*)ctx;
		const int rv = spdk_bdev_open_ext(me->back.bdev_name, true, server_spdk_ram::bdev_event_cb, NULL, &me->back.bdev_desc);
		if (rv != 0) {
			me->binfo.bdev_descriptor = -1;
			dserr(me, "Open bdev %s - XX, client=%s, fd=%d, rv=%d\n", me->back.bdev_name, who, me->binfo.bdev_descriptor, rv);
			return me->binfo;
		}
		struct spdk_bdev *b = me->back.bdev = spdk_bdev_desc_get_bdev(me->back.bdev_desc);
		me->binfo.bdev_descriptor = 555;	// Just some number, take from spdk for debug
		me->binfo.block_size = spdk_bdev_get_block_size(b);
		me->binfo.num_total_blocks = spdk_bdev_get_num_blocks(b); // spdk_bdev_get_write_unit_size(b);
		const uint32_t buf_align = spdk_bdev_get_buf_align(b), reminder = (me->binfo.block_size % buf_align);
		my_assert((buf_align <= me->binfo.block_size));		// Client operates in block sizes
		my_assert(reminder == 0);
		dslog(me, "Open bdev %s - OK, client=%s, fd=%d, align=0x%x[b] totalblks=%lu rv=%d\n", me->back.bdev_name, who, me->binfo.bdev_descriptor, buf_align, me->binfo.num_total_blocks, rv);
		me->back.bdev_io_channel = spdk_bdev_get_io_channel(me->back.bdev_desc);
		if (me->back.bdev_io_channel == NULL) {
			dserr(me, "Could not create bdev I/O channel, closing!!\n");
			close1(me, who);
		}
		if (spdk_bdev_is_zoned(me->back.bdev)) {
			dslog(me, "Zoned bdev %s - will reset zone\n", me->back.bdev_name);
			me->back.reset_zone(me->back.bdev);
		}
		return me->binfo;
	}

	static int close1(void *ctx, const char* who) {
		server_spdk_ram *me = (server_spdk_ram*)ctx;
		const int prev_fd = me->binfo.bdev_descriptor;
		me->back.disconnect();
		if (me->binfo.is_valid()) {
			me->binfo.bdev_descriptor = -1;
		}
		dslog(me, "close bdev %s - OK, client=%s, fd=%d\n", me->back.bdev_name, who, prev_fd);
		return 0;
	}

	static void cmp_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
		gusli::server_io_req& io = *(gusli::server_io_req*)cb_arg;
		SPDK_NOTICELOG("Callback IO[%c]: #rng = %u, buf_size=%lu[b]\n", io.params.op, io.params.num_ranges(), io.params.buf_size());
		if (success) {
			io.set_success(io.params.map.data.byte_len);
		} else {
			SPDK_ERRLOG("io[%c], bdev io error: %d\n", io.params.op, EIO);
			io.set_error(gusli::io_error_codes::E_PERM_FAIL_NO_RETRY);
		}
		if (bdev_io) spdk_bdev_free_io(bdev_io);
	}

	static void exec_io(void *ctx, class gusli::server_io_req& io) {
		server_spdk_ram *me = (server_spdk_ram *)ctx;
		my_assert(io.has_callback());			// Do not support io without callback for now
		io.start_execution();

		auto *desc = me->back.bdev_desc;
		auto *chan = me->back.bdev_io_channel;
		if (io.params.num_ranges() <= 1) {
			int rv = -1;
			gusli::io_map_t &map = io.params.map;
			dslog(me, "Serving IO[%c]: #rng = %u, buf_size=%lu[b]\n", io.params.op, io.params.num_ranges(), io.params.buf_size());
			if (io.params.op == gusli::G_READ) {
				rv = spdk_bdev_read( desc, chan, map.data.ptr, map.offset_lba_bytes, map.data.byte_len, cmp_cb, &io);
			} else if (io.params.op == gusli::G_WRITE) {
				rv = spdk_bdev_write(desc, chan, map.data.ptr, map.offset_lba_bytes, map.data.byte_len, cmp_cb, &io);
			}
			if (rv != 0) {
				cmp_cb(NULL, false, me);
				dserr(me,"error %s while writing to bdev: %d\n", spdk_strerror(-rv), rv);
			}
		} else {
			const gusli::io_multi_map_t* mio = io.get_multi_map();
			dslog(me, "Serving IO: #rng = %u, buf_size=%lu[b]\n", io.params.num_ranges(), io.params.buf_size());
			io.set_error(gusli::io_error_codes::E_PERM_FAIL_NO_RETRY);
			(void)mio;
		}
	}
	#undef dslog
 public:
	server_spdk_ram(const char* _name, const char* listen_addr) {
		strncpy(p.listen_address, listen_addr, sizeof(p.listen_address)-1);
		p.log = stderr;
		p.server_name = "SSRV";
		p.has_external_polling_loop = true;
		p.use_blocking_client_accept = false;	// Must do this if you want to run > 1 server on the same cpu core (spdk thread)
		p.vfuncs = {.caller_context = this, .open1 = server_spdk_ram::open1, .close1 = server_spdk_ram::close1, .exec_io = server_spdk_ram::exec_io };
		snprintf(binfo.name, sizeof(binfo.name), "%s%s", gusli::global_clnt_context::thread_names_prefix, _name);
		back.bdev_name = _name;
		back.my_srvr = this;
		const int rename_rv = pthread_setname_np(pthread_self(), binfo.name);	// For debug, set its thread to block device name
		my_assert(rename_rv == 0);
		gs = new gusli::global_srvr_raii(p);
		my_assert(gs->BREAKING_VERSION == 1);
	}
	~server_spdk_ram() { delete gs; }
	int run_once(void) { return gs->run(); }
	struct backend_dev_t *get_bdev(void) { return &back; }
	const gusli::global_srvr_context::init_params &get_gparams(void) const { return p; }
};
