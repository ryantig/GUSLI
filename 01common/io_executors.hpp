#pragma once
#include "gusli_client_api.hpp"
#include "00utils/atomics.hpp"
#include <aio.h>
#include <signal.h>
namespace gusli {

class io_request_executor_base : no_implicit_constructors {
 protected:
	class io_request& io;					// Link to original IO
	int64_t total_bytes;					// Total transferred bytes accross all io ranges
	bool has_finished_all_async_tasks;
	const io_multi_map_t *get_mm(void) const { return io.get_multi_map(); }
	void log_io_failed(                                  int64_t rv) const { pr_verb1("io[%c].failed[%ld]: "                            PRINT_EXTERN_ERR_FMT "\n", io.params.op,                         rv, PRINT_EXTERN_ERR_ARGS); }
	void log_io_succes(                                  int64_t rv) const { pr_verb1("io[%c].n_ranges[%u].completed[%ld[b]]\n",                                   io.params.op, io.params.num_ranges(), rv); }
	void log_io_range_failed(uint64_t lba, uint64_t len, int64_t rv) const { pr_verb1("io[%c].range[0x%lx].len[0x%lx].failed[%ld]: "    PRINT_EXTERN_ERR_FMT "\n", io.params.op, lba, len,               rv, PRINT_EXTERN_ERR_ARGS); }
	void log_io_range_succes(uint64_t lba, uint64_t len, int64_t rv) const { pr_verb1("io[%c].range[0x%lx].len[0x%lx].completed[%ld[b]]\n",                        io.params.op, lba, len,               rv); }
 private:
	void set_io_rv(void) const {
		if ((uint64_t)total_bytes == io.params.buf_size()) {
			log_io_succes(total_bytes);
			io.out.rv = total_bytes;
		} else {
			log_io_failed(total_bytes);
			io.out.rv = (int64_t)io_error_codes::E_BACKEND_FAULT;
		}
	}
 public:
	io_request_executor_base(class io_request& _io) : io(_io), total_bytes(0), has_finished_all_async_tasks(false) {
		if (io.params._async_no_comp)
			io._exec = this;
	}
	virtual ~io_request_executor_base() {
		io._exec = NULL;		// Disconnect executor from io
		set_io_rv();
		io.complete();
	}
	virtual void run(void) = 0;
	void mark_done_send_err_async_work(void) {					// Async work not started, we are still in submit io (polling code has not started, completion callback is not awaited yet)
		delete this;
	}
	void mark_done_with_all_async_work(void) {
		if (!io.params._async_no_comp)
			delete this;										// Notify IO / remove executor
		else													// IO will query the executor when polling and destroy it
			has_finished_all_async_tasks = true;
	}
	virtual enum io_error_codes is_still_running(void) {
		return (has_finished_all_async_tasks ? io_error_codes::E_OK : io_error_codes::E_IN_TRANSFER);
	}
};

/*****************************************************************************/
class aio_request_executor : public io_request_executor_base {						// Execute async io with aio, assume class allocated on heap (cant be on stack because non blocking)
	uint16_t num_ranges;					// Num aios needed to complete the input io
	uint16_t send_error;					// At least 1 request cound not be submitted for execution
	atomic_uint32_t num_remaining_req;		// Remaining in air io's, atomic counter due to async completions
	union {
		struct aiocb  req1;					// Single request, allocated inline to save mallocs
		struct aiocb* reqs;					// Multi request, additional array
	} u;
	void prep_aio(struct aiocb *r, const io_map_t& map) {
		r->aio_fildes = io.params.bdev_descriptor;
		r->aio_buf =    map.data.ptr;
		r->aio_nbytes = map.data.byte_len;
		r->aio_offset = map.offset_lba_bytes;
		r->aio_sigevent.sigev_notify = SIGEV_THREAD;
		r->aio_sigevent.sigev_notify_function = aio_request_executor::__aio_comp_cb;
		r->aio_sigevent.sigev_notify_attributes = NULL;
		r->aio_sigevent.sigev_value.sival_ptr = (void*)this;		// Connect request with io executor
	}
	int launch_1_aio_rew(struct aiocb *r) {
		const int rv = (io.params.op == G_READ) ? aio_read(r) : aio_write(r); // We used rv on stack to avoid race condition between 2 writes to out.rv (send rv and completion rv)
		if (rv < 0) {
			send_error = 1;
			__aio_comp_cb(r->aio_sigevent.sigev_value);				// Directly call the error callback, Here 'this' might not exist anymore
		}
		return rv;
	}
	void __analyze_1_range(struct aiocb *r) {
		const int err = aio_error(r);
		const int64_t range_total_bytes = ((0 == err) ? (int64_t)aio_return(r) : (int64_t)0);
		total_bytes += range_total_bytes;
		if (err == 0) log_io_range_succes(r->aio_offset, r->aio_nbytes, range_total_bytes);
		else  		  log_io_range_failed(r->aio_offset, r->aio_nbytes, err);
	}
	static void __aio_comp_cb(sigval_t sigval) {
		aio_request_executor* exec = (aio_request_executor*)sigval.sival_ptr;
		if ((exec->num_ranges > 1) && (exec->num_remaining_req.dec() > 0)) {
			return; // Still waiting for other IO's
		} else if (exec->send_error) {							// Nop: do not update completed bytes
		} else if (exec->num_ranges > 1) {						// All ranges completed, analyze them
			for (uint32_t i = 0; i < exec->num_ranges; i++)
				exec->__analyze_1_range(&exec->u.reqs[i]);
		} else {
				exec->__analyze_1_range(&exec->u.req1);
		}
		exec->mark_done_with_all_async_work();
	}
 public:
	void run(void) override {
		if (num_ranges > 1) {
			if (!u.reqs) { 	// Initialization error
				return mark_done_send_err_async_work();
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
	aio_request_executor(class io_request& _io) : io_request_executor_base(_io) {
		num_ranges = _io.params.num_ranges();
		send_error = 0;
		if (num_ranges > 1) {
			u.reqs = (typeof(u.reqs))calloc(num_ranges, sizeof(struct aiocb));
			if (u.reqs) {
				const io_multi_map_t *mm = get_mm();
				for (uint32_t i = 0; i < mm->n_entries; i++)
					prep_aio(&u.reqs[i], mm->entries[i]);
			}
		} else {
			memset(&u.req1, 0, sizeof(u.req1));
			prep_aio(&u.req1, io.params.map);
		}
	}
	~aio_request_executor() {
		if (num_ranges > 1)
			free(u.reqs);
	}
};

/*****************************************************************************/
class sync_request_executor : public io_request_executor_base {	// On stack executor
	int64_t _do_1_map(int fd, enum io_type op, const io_map_t &map) {
		const uint64_t num_bytes = map.data.byte_len, off = map.offset_lba_bytes;
		void *buf = (void*)map.data.ptr;
		lseek(fd, off, SEEK_SET);
		const int64_t rv = (op == G_READ) ? read(fd, buf, num_bytes) : write(fd, buf, num_bytes);
		total_bytes += rv;
		if (rv > 0) log_io_range_succes(off, num_bytes, rv);
		else  		log_io_range_failed(off, num_bytes, rv);
		return rv;
	}
 public:
	void run(void) override {
		const int fd = io.params.bdev_descriptor;
		if (!io.params._has_mm) {
			_do_1_map(fd, io.params.op, io.params.map);
		} else {
			const io_multi_map_t *mm = get_mm();
			for (uint32_t i = 0; i < mm->n_entries; i++) {
				const int64_t rv = _do_1_map(fd, io.params.op, mm->entries[i]);
				if (rv <= 0)
					break;		// Fast failure, dont continue with io if 1 range already failed
			}
		}
		mark_done_with_all_async_work();
	}
	sync_request_executor(class io_request& _io) : io_request_executor_base(_io) {}
	~sync_request_executor() {}
};

/*****************************************************************************/
class spdk_request_executor : public io_request_executor_base {						// Execute async io with spdk
public:
	spdk_request_executor(class io_request& _io) : io_request_executor_base(_io) {
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
		total_bytes += ((rv==0) ? (int64_t)io.params.map.data.byte_len : (int64_t)0);
	}
	void run(void) override {}
	~spdk_request_executor() { }
};

}; // namespace gusli