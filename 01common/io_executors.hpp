#pragma once
#include "gusli_client_api.hpp"
#include "00utils/atomics.hpp"
#include <aio.h>
#include <signal.h>
#include <semaphore.h>		// Waiting for async io completion
namespace gusli {

struct io_autofail_executor : no_implicit_constructors {			// autofail io, dont execute anything, used on stack
	io_autofail_executor(class io_request& io, io_error_codes rv) {
		DEBUG_ASSERT(io._exec == nullptr);		// This executor never connects nor disconnects from io
		io.out.rv = rv;
		io.complete();
	}
};

/*****************************************************************************/
class io_request_executor_base : no_implicit_constructors {
 protected:
	class io_request* io;							// Link to original IO. If cacnel() not called will always be a valid pointer.
	int64_t total_bytes = 0L;						// Total transferred bytes accross all io ranges
	uint16_t num_ranges;							// Just cache this to be able to access the field even if io canceles and gets free
	bool was_rv_already_set_by_remote = false;		// was 'rv' of io already set, Typically false. If true, no need to analize 'total_bytes';
	const io_multi_map_t *get_mm(void) const { return io->get_multi_map(); }
	enum io_type op(void) const { return io->params.op; }
 private:
	bool is_async_executor = false;					// Can async_work_done() be called Asynchronously by executor
	struct cancelation_atomic_t {					// Synchronization between cancel() and asyncronous completion of io. Relevant only for async callback io/executors
		t_lock_spinlock lock;						// Protect the critical section
		bool has_finished_all_async_tasks = false;	// Set possibly asyncrounsly by internal completion
		bool was_canceled = false;					// Requested externally
	} cmp;
	struct detach_atomic_t {						// Synchronization between io detaching from executor and executor termination.
		atomic_uint32_t count;						// Refcount starts with 2: io and executor, when io does not need executor
		bool io_already_detached_from_me = false;	// Alternative to atomic refcount, used whne no async callbacks arrive
	} ref;
	void __dec_ref(bool is_self) {					// IO and self hold reference. When both decreased auto-free
		if (!is_self) {
			ASSERT_IN_PRODUCTION(ref.io_already_detached_from_me == false);			// Detect double call of IO disconenct
			ref.io_already_detached_from_me = true;
		}
		log_put(is_self ? 'o' : 'e');		// o = operation, e = executor
		const bool should_destroy = (is_async_executor ? (ref.count.dec() == 0) :
						(cmp.has_finished_all_async_tasks && ref.io_already_detached_from_me));
		if (should_destroy)
			delete this;
	}

	void log_start(                                                ) const { pr_verb1("exec[%p].o[%p].io[%c].n_ranges[%u].start"                                     "\n", this, io, op(), num_ranges); }
	void log_free(                                                 ) const { pr_verb1("exec[%p].o[%p].free"                                                          "\n", this, io                  ); }
	void log_set_rv(                                               ) const { pr_verb1("exec[%p].o[%p].done[%ld[b]].rv[%ld] "                    PRINT_EXTERN_ERR_FMT "\n", this, io,  total_bytes,             io->out.rv, PRINT_EXTERN_ERR_ARGS); }
	void log_cancel(                                               ) const { pr_verb1("exec[%p].o[%p].was_cancel[%d]"                                                "\n", this, io,     cmp.was_canceled); }
	void log_put(                                           char rv) const { pr_verb1("exec[%p].o[%p].put[%c(%d%d)]"                                                       "\n", this, io, rv, cmp.has_finished_all_async_tasks, ref.io_already_detached_from_me ); }
 protected:
	void log_io_range_failed(uint64_t lba, uint64_t len, int64_t rv) const { pr_verb1("exec[%p].o[%p].range[0x%lx].len[0x%lx].failed[%ld]: "    PRINT_EXTERN_ERR_FMT "\n", this, io, lba, len,     rv, PRINT_EXTERN_ERR_ARGS); }
	void log_io_range_succes(uint64_t lba, uint64_t len, int64_t rv) const { pr_verb1("exec[%p].o[%p].range[0x%lx].len[0x%lx].completed[%ld[b]]\n",                        this, io, lba, len,     rv); }
	void async_work_done(void) {
		if (is_async_executor) cmp.lock.lock();
		cmp.has_finished_all_async_tasks = true;
		if (!cmp.was_canceled) {
			if (!was_rv_already_set_by_remote) {
				BUG_ON(io->out.rv != (int64_t)io_error_codes::E_IN_TRANSFER, "Wrong flow rv=%lu", io->out.rv);
				io->out.rv = ((uint64_t)total_bytes == io->params.buf_size()) ? total_bytes : (int64_t)io_error_codes::E_BACKEND_FAULT;	// Partial io not supported
			}
			log_set_rv();
			io->complete();									// User callback may not call cancel() so no dead lock
		}
		if (is_async_executor) cmp.lock.unlock();
		__dec_ref(true);
	}
	void send_async_work_failed(void) { async_work_done(); }// Async work not started, we are still in submit io (polling code has not started, completion callback is not awaited yet)
 public:													// API to use by IO
	io_request_executor_base(class io_request& _io, const bool is_async) : io(&_io), num_ranges(io->params.num_ranges()) {
		is_async_executor = is_async;
		if (is_async_executor) { ref.count.set(2); cmp.lock.init(); }
		io->out.rv = io_error_codes::E_IN_TRANSFER;
		log_start();
	}
	virtual ~io_request_executor_base() {
		log_free();
		if (is_async_executor) { cmp.lock.destroy(); }
	}
	virtual void run(void) = 0;								// Start IO execution
	virtual enum io_request::cancel_rv cancel(void) {		// Assume IO calls cancel() or detach_io() exactly once, no concurency here
		io_request::cancel_rv rv;
		if (is_async_executor) cmp.lock.lock();
		if (cmp.has_finished_all_async_tasks) {
			rv = io_request::cancel_rv::G_ALLREADY_DONE;
		} else {
			cmp.was_canceled = true;
			rv = io_request::cancel_rv::G_CANCELED;
		}
		if (is_async_executor) cmp.lock.unlock();
		log_cancel();
		detach_io();
		return rv;
	};

	virtual enum io_error_codes is_still_running(void) {		// Query executor status, used for non blocking io's
		return (cmp.has_finished_all_async_tasks ? io_error_codes::E_OK : io_error_codes::E_IN_TRANSFER);
	}
	void detach_io(void) { __dec_ref(false); }
};

/*****************************************************************************/
class aio_request_executor : public io_request_executor_base {						// Execute async io with aio, assume class allocated on heap (cant be on stack because non blocking)
	uint32_t send_error;					// At least 1 request cound not be submitted for execution
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
	int launch_1_aio_rw(struct aiocb *r) {
		const int rv = (op() == G_READ) ? aio_read(r) : aio_write(r); // We used rv on stack to avoid race condition between 2 writes to out.rv (send rv and completion rv)
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
		exec->async_work_done();
	}
 public:
	void run(void) override {
		if (num_ranges > 1) {
			if (!u.reqs) { 	// Initialization error
				return send_async_work_failed();
			}
			num_remaining_req.set(num_ranges);
			const uint32_t n_req_to_run_on_stack = num_ranges;
			for (uint32_t i = 0; i < n_req_to_run_on_stack; i++) {
				if (launch_1_aio_rw(&u.reqs[i]) < 0) {
					/*if (errno == EAGAIN) { // If we hit the kernel limit, cancel all submitted I/Os and return error
						for (uint32_t j = 0; j < i; i++) {
							aio_cancel(u.reqs[i].aio_fildes &u.reqs[i]);
						}
					} // Todo: Avoid sending the rest of the IO's: do atomic_sub on counter
					*/
				}
			}
		} else {
			launch_1_aio_rw(&u.req1);
		}
	}
	aio_request_executor(class io_request& _io) : io_request_executor_base(_io, true), send_error(0) {
		if (num_ranges > 1) {
			u.reqs = (typeof(u.reqs))calloc(num_ranges, sizeof(struct aiocb));
			if (u.reqs) {
				const io_multi_map_t *mm = get_mm();
				for (uint32_t i = 0; i < mm->n_entries; i++)
					prep_aio(&u.reqs[i], mm->entries[i]);
			}
		} else {
			memset(&u.req1, 0, sizeof(u.req1));
			prep_aio(&u.req1, io->params.map);
		}
	}
	~aio_request_executor() {
		if (num_ranges > 1)
			free(u.reqs);
	}
};

/****************************** Blocking executors ****************************************/
struct blocking_request_executor : public io_request_executor_base {
	blocking_request_executor(class io_request& _io) : io_request_executor_base(_io, false) {}
	enum io_request::cancel_rv cancel(void) override { BUG_ON(true, "IO was already completed, cancel does nothing and should not be called"); return io_request_executor_base::cancel(); }
};

class sync_request_executor : public blocking_request_executor {
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
		const int fd = io->params.bdev_descriptor;
		if (!io->params._has_mm) {
			_do_1_map(fd, op(), io->params.map);
		} else {
			const io_multi_map_t *mm = get_mm();
			for (uint32_t i = 0; i < mm->n_entries; i++) {
				const int64_t rv = _do_1_map(fd, op(), mm->entries[i]);
				if (rv <= 0)
					break;		// Fast failure, dont continue with io if 1 range already failed
			}
		}
		async_work_done();
	}
	sync_request_executor(class io_request& _io) : blocking_request_executor(_io) {}
};

class remote_aio_blocker : public blocking_request_executor {						// Convert remote async request io to blocking
	sem_t wait;					// Block sender until io returns
	static void __cb(remote_aio_blocker *exec) {
		pr_verb1("exec[%p].o[%p].blocked_rio: completion arrived\n", exec, exec->io);
		exec->was_rv_already_set_by_remote = true;
		BUG_ON(sem_post(&exec->wait) != 0, "Cant unblock waiter");		// Must be last line because after unblock executor can get free
	}
 public:
	remote_aio_blocker(io_request &_io) : blocking_request_executor(_io) {
		io->params.set_completion(this, this->__cb);
		BUG_ON(sem_init(&wait, 0, 0) != 0, "Cannot init blocking io");
	}
	void run(void) override {}
	enum io_error_codes is_still_running(void) override {
		BUG_ON(sem_wait(&wait) != 0, "Cannot wait for blocking io");
		pr_verb1("exec[%p].o[%p].blocked_rio: un-block, finish\n", this, io);
		io->params.set_completion(NULL, NULL);
		async_work_done();
		return io_error_codes::E_OK;
	}
};

/*****************************************************************************/
class server_side_executor : public io_request_executor_base {						// Convert remote async request io to blocking
	int (*fn)(void *ctx, class io_request& io);
	void* ctx;
 public:
	server_side_executor(int (*_fn)(void *, class io_request&), void *_ctx, io_request &_io) : io_request_executor_base(_io, false), fn(_fn), ctx(_ctx) {}
	~server_side_executor() { }
	void run(void) override {
		const int exec_rv = fn(ctx, *io);
		pr_verb1("exec[%p].o[%p].Server io: rv=%d\n", this, io, exec_rv);
		total_bytes = (exec_rv >= 0) ? (int64_t)io->params.buf_size() : (int64_t)exec_rv;
		detach_io();
		async_work_done();
	}
	enum io_request::cancel_rv cancel(void) override { BUG_NOT_IMPLEMENTED(); return io_request_executor_base::cancel(); }
};
/*****************************************************************************/
class spdk_request_executor : public io_request_executor_base {						// Execute async io with spdk
public:
	spdk_request_executor(class io_request& _io) : io_request_executor_base(_io, false) {
		int rv = 0;
		BUG_ON(io->params._has_mm, "Not implementded yet");
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
		total_bytes += ((rv==0) ? (int64_t)rv : (int64_t)0);
	}
	void run(void) override {BUG_NOT_IMPLEMENTED();}
	enum io_request::cancel_rv cancel(void) override { BUG_NOT_IMPLEMENTED(); return io_request_executor_base::cancel(); }
	~spdk_request_executor() { }
};

}; // namespace gusli

/*****************************************************************************/
#if defined(HAS_URING_LIB)
#include <liburing.h>				// To use uring library do: sudo apt install -y liburing-dev   or   sudo dnf install liburing-devel
namespace gusli {
class uring_request_executor : public io_request_executor_base {	// Execute async io with liburing, assume class allocated on heap (cant be on stack because non blocking)
	typedef void (*prep_func_t)(struct io_uring_sqe*, int fd, const void*buf, unsigned int nbytes, __u64 offset);
	struct io_uring uring;
	int num_completed;				// Number of completed io ranges so far
	bool had_failure;
	prep_func_t prep_op;  // Pointer to prep function
	bool init_uring_queue(void) {
		io_uring_params p = {};
		const int urv = io_uring_queue_init_params(num_ranges, &uring, &p);
		if (urv < 0) {
			pr_err1("exec[%p].o[%p] Failed to initialize io_uring[%u], rv=%d(%s) " PRINT_EXTERN_ERR_FMT "\n", this, io, num_ranges, urv, strerror(-urv), PRINT_EXTERN_ERR_ARGS);
			had_failure = true;
		} else if (false) {
			char buf[256];
			int buf_len = 256, count = 0;
			buf[0] = 0;
			if (p.features | IORING_FEAT_SQPOLL_NONFIXED) BUF_ADD("SQPOLL,");
			if (p.features | IORING_FEAT_FAST_POLL)       BUF_ADD("IOPOLL,");
			if (buf[0] == 0) BUF_ADD("None");
			pr_verb1("uring flags=0x%x, params={%s}\n", p.flags, buf);
		}
		return !had_failure;
	}
	bool prep_uringio(const io_map_t& map) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&uring);
		if (!sqe) {
			pr_err1("exec[%p].o[%p] Error get io_uring.sqe, io_ranges=%u\n", this, io, num_ranges);
			had_failure = true;
		} else {
			prep_op(sqe, io->params.bdev_descriptor, map.data.ptr, map.data.byte_len, map.offset_lba_bytes);
			sqe->user_data = (__u64)this;
		}
		return !had_failure;
	}
	void __analyze_1_range(const struct io_uring_cqe *cqe) {
		pr_verb1("cqe=%p exec=0x%llx, rv=%d\n", cqe, cqe->user_data, cqe->res);
		if (cqe->res > 0)
			total_bytes += cqe->res;
	}
public:
	uring_request_executor(class io_request& _io) : io_request_executor_base(_io, false) {
		num_completed = 0;
		had_failure = false;
		prep_op = (op() == G_READ) ? (prep_func_t)io_uring_prep_read : (prep_func_t)io_uring_prep_write;
		if (!init_uring_queue()) return;
		if (num_ranges > 1) {
			const io_multi_map_t *mm = get_mm();
			for (uint32_t i = 0; i < mm->n_entries; i++)
				if (!prep_uringio(mm->entries[i])) return;
		} else {
			if (!prep_uringio(io->params.map)) return;
		}
	}
	~uring_request_executor() { io_uring_queue_exit(&uring); }
	void run(void) override {
		if (unlikely(had_failure)) { return send_async_work_failed(); }
		BUG_ON(io->has_callback(), "Wrong executor usage, async mode unsupported yet");
		const int n_submit = io_uring_submit(&uring);
		if (n_submit != num_ranges) {
			BUG_ON(n_submit > 0, "partial io_uring submission not supported yet: %d/%d", n_submit, num_ranges);
			had_failure = true; return send_async_work_failed();
		}
		if (io->params._async_no_comp)
			return;									// Nothing to do, user will poll for completion
		while (num_completed != num_ranges) {		// Blocking mode, poll uring ourselves
			struct io_uring_cqe *cqe;
			const int wait_rv = io_uring_wait_cqe(&uring, &cqe);
			if (wait_rv < 0) {
				pr_err1("exec[%p].o[%p] Failed to get cqe" PRINT_EXTERN_ERR_FMT "\n", this, io, PRINT_EXTERN_ERR_ARGS);
				return send_async_work_failed();
			}
			__analyze_1_range(cqe);
			num_completed++;
			io_uring_cqe_seen(&uring, cqe);
		}
		async_work_done();
	}
	enum io_request::cancel_rv cancel(void) override {
		is_still_running();									// Optimization: Poll cqes last time. If IO already completed return success instead of cancel.
		const auto rv = io_request_executor_base::cancel();
		if (rv == io_request::cancel_rv::G_CANCELED)
			async_work_done();			// Because there is no async work left
		return rv;
	}
	enum io_error_codes is_still_running(void) override {
		const io_error_codes rv = io_request_executor_base::is_still_running();
		if (rv != io_error_codes::E_IN_TRANSFER)
			return rv;
		struct io_uring_cqe* cqe;					// Process all available completions
		unsigned head, count = 0;
		io_uring_for_each_cqe(&uring, head, cqe) {
			__analyze_1_range(cqe);
			count++;
		}
		io_uring_cq_advance(&uring, count);			// Mark all seen
		num_completed += count;
		if (num_completed == num_ranges)
			async_work_done();
		return io_request_executor_base::is_still_running();
	}
};
}; // namespace gusli
#endif
