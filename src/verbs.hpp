#pragma once

#include <infiniband/verbs.h>
#include <cstdint>
#include <string>

// Open an RDMA device. name empty => first device.
// Returns null context on failure.
ibv_context* ib_open_device(const char* name);

// Query GID table and print it (indices 0..max-1).
void ib_dump_gid_table(ibv_context* ctx, int port);

// Fill gid + gid_index for RoCE (prefer an IPv4-mapped / routable v2 GID).
// Returns 0 on success.
int ib_pick_gid(ibv_context* ctx, int port, ibv_gid* out_gid, int* out_index);

// Format a GID as hex string.
std::string gid_to_string(const ibv_gid& gid);

struct QpBringUpParams {
    ibv_qp*   qp           = nullptr;
    uint8_t   port         = 1;
    int       path_mtu     = IBV_MTU_1024;
    uint32_t  remote_qpn   = 0;
    uint32_t  local_psn    = 0;
    uint32_t  remote_psn   = 0;
    int       sgid_index   = 0;
    ibv_gid   dgid        {};   // remote GID
    int       min_rnr_timer= 12;
    int       timeout      = 14;
    int       retry_cnt    = 7;
    int       rnr_retry    = 7;
    uint8_t   max_dest_rd_atomic = 1;
    uint8_t   max_rd_atomic      = 1;
};

// Bring an RC QP from RESET to RTS using the given peer params.
int ib_bring_qp_rts(const QpBringUpParams& p);

// Raise the QP to ERROR (clean-ish teardown for tests).
int ib_mark_qp_error(ibv_qp* qp);

// Human-readable name for an ibv_wc_status.
const char* wc_status_name(enum ibv_wc_status s);

// True if the WC status indicates the QP entered the error state (fatal:
// the connection must be torn down and re-established, not retried in place).
bool wc_is_fatal(enum ibv_wc_status s);