#pragma once
#include "gusli_client_api.hpp"
#include "00utils/atomics.hpp"
#include <aio.h>
#include <signal.h>

#define AIO_VERBOSE (0)
namespace gusli {

class io_request_executor_base {
 public:
	virtual ~io_request_executor_base() {}
	void attach_to_io(       io_request &io) { (void)io; /*io.__internal_context = (void*)this;*/ }
	//static void* get_executor_of_io(io_request &io) { return io.__internal_context; }
	static const io_multi_map_t *get_mm(io_request &io) { return io.get_multi_map(); }
	static void set_rv(io_request *io, int64_t rv) { io->out.rv = rv; }
	static void finish(io_request *io) { /*io->__internal_context = NULL;*/ io->complete();	}
	static void log_io_failed(const io_request &io,                        int rv) { if (AIO_VERBOSE) printf("Async io=%c failed (%d): "     PRINT_EXTERN_ERR_FMT "\n", io.params.op, rv, PRINT_EXTERN_ERR_ARGS); }
	static void log_io_succes(const io_request &io, const struct aiocb *r, int rv) { if (AIO_VERBOSE) printf("Async io=%c completed: %d[b] vlba=%5u[b], nlba=%5u[b]\n", io.params.op, rv, (unsigned)r->aio_offset, (unsigned)r->aio_nbytes); }
};

// Code resembles: https://github.com/ai-dynamo/nixl/blob/main/src/plugins/posix/aio_queue.h
class aio_request_executor : public io_request_executor_base {						// Execute async io with aio
	class io_request* io;					// Link to original IO
	uint16_t num_ranges;					// Num aios needed to complete the input io
	uint16_t send_error;					// At least 1 request cound not be submitted for execution
	atomic_uint32_t num_remaining_req;		// Remaining in air io's, atomic counter due to async completions
	union {
		struct aiocb  req1;					// Single request, allocated inline to save mallocs
		struct aiocb* reqs;					// Multi request, additional array
	} u;
	void prep_aio(struct aiocb *r, const io_map_t& map) {
		r->aio_fildes = io->params.bdev_descriptor;
		r->aio_buf =    map.data.ptr;
		r->aio_nbytes = map.data.byte_len;
		r->aio_offset = map.offset_lba_bytes;
		r->aio_sigevent.sigev_notify = SIGEV_THREAD;
		r->aio_sigevent.sigev_notify_function = aio_request_executor::__aio_comp_cb;
		r->aio_sigevent.sigev_notify_attributes = NULL;
		r->aio_sigevent.sigev_value.sival_ptr = (void*)this;		// Connect request with io executor
	}
	int launch_1_aio_rew(struct aiocb *r) {
		const int rv = (io->params.op == G_READ) ? aio_read(r) : aio_write(r); // We used rv on stack to avoid race condition between 2 writes to out.rv (send rv and completion rv)
		if (rv < 0) {
			send_error = 1;
			__aio_comp_cb(r->aio_sigevent.sigev_value);				// Directly call the error callback
			// Here 'this' can be deleted!
		}
		return rv;
	}

 public:
	void run(void) {
		if (num_ranges > 1) {
			if (!u.reqs) { 	// Initialization error
				set_rv(io, io_error_codes::E_BACKEND_FAULT);
				delete this;
				return;
			}
			num_remaining_req.set(num_ranges);
			const uint32_t n_req_to_run_on_stack = num_ranges;
			for (uint32_t i = 0; i < n_req_to_run_on_stack; i++) {
				if (launch_1_aio_rew(&u.reqs[i]) < 0) {
					/*if (errno == EAGAIN) { // If we hit the kernel limit, cancel all submitted I/Os and return error
						for (uint32_t j = 0; j < i; i++) {
							aio_cancel(io.params.bdev_descriptor, &u.reqs[i]);
						}
					} // Todo: Avoid sending the rest of the IO's: do atomic_sub on counter
					*/
				}
			}
		} else {
			launch_1_aio_rew(&u.req1);
		}
	}
	aio_request_executor(class io_request& _io) : io(&_io) {
		num_ranges = _io.params.num_ranges();
		send_error = 0;
		if (num_ranges > 1) {
			u.reqs = (typeof(u.reqs))calloc(num_ranges, sizeof(struct aiocb));
			if (u.reqs) {
				const io_multi_map_t *mm = get_mm(_io);
				for (uint32_t i = 0; i < mm->n_entries; i++)
					prep_aio(&u.reqs[i], mm->entries[i]);
			}
		} else {
			memset(&u.req1, 0, sizeof(u.req1));
			prep_aio(&u.req1, io->params.map);
		}
		attach_to_io(_io);
	}
	~aio_request_executor() {
		finish(io);
	}
	static void __aio_comp_cb(sigval_t sigval) {
		aio_request_executor* exec = (aio_request_executor*)sigval.sival_ptr;
		io_request *io = exec->io;
		if ((exec->num_ranges > 1) && (exec->num_remaining_req.dec() > 0)) {
			return; // Still waiting for other IO's
		} else if (exec->send_error) {
			set_rv(io, io_error_codes::E_BACKEND_FAULT); log_io_failed(*io, -1);
		} else if (exec->num_ranges > 1) {
			int64_t total_bytes = 0;
			for (uint32_t i = 0; i < exec->num_ranges; i++) {
				struct aiocb *r = &exec->u.reqs[i];
				const int err = aio_error(r);
				if (err == 0) {
					total_bytes += (int64_t)aio_return(r);					log_io_succes(*io, r, (int)aio_return(r));
				} else {
					total_bytes = (int64_t)io_error_codes::E_BACKEND_FAULT; log_io_failed(*io, err);
					break;
				}
			}
			set_rv(io, total_bytes);
		} else {
			struct aiocb *r = &exec->u.req1;
			const int err = aio_error(r);
			if (err == 0) {
				set_rv(io, (int64_t)aio_return(r));							log_io_succes(*io, r, (int)aio_return(r));
			} else {
				set_rv(io, io_error_codes::E_BACKEND_FAULT); log_io_failed(*io, err);
			}
		}
		delete exec;
	}
};

/*****************************************************************************/
class sync_request_executor : public io_request_executor_base {	// On stack executor
	ssize_t _do_1_map(int fd, enum io_type op, const io_map_t &map) {
		const uint64_t num_bytes = map.data.byte_len;
		void *buf = (void*)map.data.ptr;
		lseek(fd, map.offset_lba_bytes, SEEK_SET);
		return (op == G_READ) ? read(fd, buf, num_bytes) : write(fd, buf, num_bytes);
	}
 public:
	sync_request_executor(class io_request& io) {
		const int fd = io.params.bdev_descriptor;
		int64_t total_bytes = 0;
		if (!io.params._has_mm) {
			const int64_t rv = _do_1_map(fd, io.params.op, io.params.map);
			total_bytes = ((rv > 0) ? rv : (int64_t)io_error_codes::E_BACKEND_FAULT);
		} else {
			const io_multi_map_t *mm = get_mm(io);
			for (uint32_t i = 0; i < mm->n_entries; i++) {
				const int64_t rv = _do_1_map(fd, io.params.op, mm->entries[i]);
				if (rv > 0) {
					total_bytes += rv;
				} else {
					total_bytes = (int64_t)io_error_codes::E_BACKEND_FAULT;
					break;
				}
			}
		}
		set_rv(&io, total_bytes);
		finish(&io);
	}
	~sync_request_executor() { }
};

/*****************************************************************************/
class spdk_request_executor : public io_request_executor_base {						// Execute async io with spdk
public:
	spdk_request_executor(class io_request& io) {
		int rv = 0;
		BUG_ON(io.params._has_mm, "Not implementded yet");
		#if SUPPORT_SPDK
			struct spdk_bdev_desc *desc = this->spdk_dev.desc;
			struct spdk_io_channel *channel = this->spdk_dev.channel;
			auto cb_lambda = [](struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
											auto req = static_cast<io_request*>(cb_arg);
											req->out.rv = success ? req->params.num_blocks : E_PERM_FAIL_NO_RETRY;
											req->complete();
											spdk_bdev_free_io(bdev_io);
										};
			if (params.op == G_READ) {
				rv = spdk_bdev_read(desc, channel, params.data_buf, params.map.offset_lba_bytes, params.num_blocks, cb_lambda, this);
			} else if (params.op == G_WRITE) {
				rv = spdk_bdev_write(desc, channel, params.data_buf,params.map.offset_lba_bytes, params.num_blocks, cb_lambda, this);
			}
		#endif
		set_rv(&io, (rv==0) ? (int64_t)io.params.map.data.byte_len : (int64_t)io_error_codes::E_INTERNAL_FAULT);
		finish(&io);
	}
	~spdk_request_executor() { }
};

}; // namespace gusli