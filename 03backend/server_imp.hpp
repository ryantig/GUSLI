#pragma once
#include "utils.hpp"
#include "gusli_server_api.hpp"
#include "server_clnt_api.hpp"
namespace gusli {

#define LIB_NAME "GUSLIs"
#define LIB_COLOR NV_COL_PURPL
#define pr_infoS(fmt, ...) pr_info1( "[%s:%s] " fmt, par.server_name, binfo.name, ##__VA_ARGS__)
#define pr_errS( fmt, ...) pr_err1(  "[%s:%s] " fmt, par.server_name, binfo.name, ##__VA_ARGS__)
#define pr_noteS(fmt, ...) pr_note1( "[%s:%s] " fmt, par.server_name, binfo.name, ##__VA_ARGS__)
#define pr_verbS(fmt, ...) pr_verb1( "[%s:%s] " fmt, par.server_name, binfo.name, ##__VA_ARGS__)

struct bdev_stats_srvr {
	uint64_t n_doorbels_wakeup_clnt, n_w_sub, n_w_cmp;
	uint64_t n_io_range_single, n_io_range_multi;
	bdev_stats_srvr() { memset(this, 0, sizeof(*this)); }
	void inc(const MGMT::msg_content::t_payload::t_dp_cmd& p) {
		n_doorbels_wakeup_clnt++;
		if (p.sender_added_new_work) n_w_cmp++;
		if (p.sender_ready_for_work) n_w_sub++;
	}
	void inc(const io_request& io) {
		if (io.params.num_ranges() > 1)
			n_io_range_multi++;
		else
			n_io_range_single++;
	}
	int print_stats(char* buf, int buf_len) {
		return scnprintf(buf, buf_len, "d={%lu/sub=%lu/cmp=%lu}, io={r1=%lu,rm=%lu}", n_doorbels_wakeup_clnt, n_w_sub, n_w_cmp, n_io_range_single, n_io_range_multi);
	}
};

class global_srvr_context_imp : public global_srvr_context, public base_library  {
	init_params par;				// Underlying bdev configuration
	bdev_info binfo;
	sock_t sock;						// Control path accept-client socket
	sock_t io_sock;						// Communication socket with client
	connect_addr ca;					// Connected client address
	class datapath_t dp;
	bdev_stats_srvr stats;
	bool is_initialized = false;
	void client_accept(connect_addr& addr);
	void client_reject(void);
	int  __clnt_bufs_register(const MGMT::msg_content &msg) __attribute__((warn_unused_result));
	int  __clnt_bufs_unregist(const MGMT::msg_content &msg) __attribute__((warn_unused_result));
	void __clnt_close(const char* reason);
	int  send_to(             const MGMT::msg_content &msg, size_t n_bytes, const struct connect_addr &addr) const __attribute__((warn_unused_result));
	friend class global_srvr_context;

	void parse_args(int argc, char* const argv[]);
 public:
	global_srvr_context_imp() : base_library(LIB_NAME) { binfo.clear(); }
	int init_impl(void);
	int run_impl(void);
	int destroy_impl(void);
};

global_srvr_context& global_srvr_context::get(void) noexcept {
	static class global_srvr_context_imp gs_ctx;
	return gs_ctx;
}

static inline       global_srvr_context_imp* _impl(      global_srvr_context* g) { return       (global_srvr_context_imp*)g; }
static inline const global_srvr_context_imp* _impl(const global_srvr_context* g) { return (const global_srvr_context_imp*)g; }

} // namespace gusli
