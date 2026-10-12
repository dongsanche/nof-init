#include "verbs.hpp"
#include "common.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#include <arpa/inet.h>

ibv_context* ib_open_device(const char* name) {
    int num = 0;
    ibv_device** devs = ibv_get_device_list(&num);
    if (!devs || num == 0) {
        ERR("no RDMA devices found");
        return nullptr;
    }

    ibv_device* chosen = nullptr;
    if (name && name[0]) {
        for (int i = 0; i < num; i++) {
            if (std::strcmp(ibv_get_device_name(devs[i]), name) == 0) {
                chosen = devs[i];
                break;
            }
        }
        if (!chosen) {
            ERR("device '%s' not found", name);
            ibv_free_device_list(devs);
            return nullptr;
        }
    } else {
        chosen = devs[0];
    }

    ibv_context* ctx = ibv_open_device(chosen);
    ibv_free_device_list(devs);
    if (!ctx) {
        ERR("ibv_open_device(%s) failed", ibv_get_device_name(chosen));
        return nullptr;
    }
    LOG("opened device %s", ibv_get_device_name(chosen));
    return ctx;
}

void ib_dump_gid_table(ibv_context* ctx, int port) {
    for (int i = 0; i < 8; i++) {
        ibv_gid g{};
        if (ibv_query_gid(ctx, (uint8_t)port, i, &g) == 0) {
            LOG("  GID[%d] = %s", i, gid_to_string(g).c_str());
        }
    }
}

static bool gid_is_nonzero(const ibv_gid& g) {
    const uint8_t* p = g.raw;
    for (int i = 0; i < 16; i++) if (p[i] != 0) return true;
    return false;
}

// prefer RoCEv2 IPv4-mapped GID (::ffff:a.b.c.d) or first nonzero GID
int ib_pick_gid(ibv_context* ctx, int port, ibv_gid* out_gid, int* out_index) {
    int first_nonzero = -1;
    for (int i = 0; i < 8; i++) {
        ibv_gid g{};
        if (ibv_query_gid(ctx, (uint8_t)port, i, &g) != 0) continue;
        if (!gid_is_nonzero(g)) continue;
        if (first_nonzero < 0) first_nonzero = i;
        // RoCEv2 IPv4-mapped: bytes 0..9 = 0, bytes 10-11 = 0xffff
        bool v4mapped = true;
        for (int k = 0; k < 10; k++) if (g.raw[k] != 0) { v4mapped = false; break; }
        if (v4mapped && g.raw[10] == 0xff && g.raw[11] == 0xff) {
            *out_gid = g;
            *out_index = i;
            return 0;
        }
    }
    if (first_nonzero >= 0) {
        ibv_query_gid(ctx, (uint8_t)port, first_nonzero, out_gid);
        *out_index = first_nonzero;
        return 0;
    }
    ERR("no usable GID found on port %d", port);
    return -1;
}

std::string gid_to_string(const ibv_gid& g) {
    char buf[128];
    // IPv4-mapped?
    bool v4 = true;
    for (int i = 0; i < 10; i++) if (g.raw[i] != 0) { v4 = false; break; }
    if (v4 && g.raw[10] == 0xff && g.raw[11] == 0xff) {
        std::snprintf(buf, sizeof(buf), "::ffff:%u.%u.%u.%u",
                      g.raw[12], g.raw[13], g.raw[14], g.raw[15]);
        return std::string(buf);
    }
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                  g.raw[0], g.raw[1], g.raw[2], g.raw[3], g.raw[4], g.raw[5], g.raw[6], g.raw[7],
                  g.raw[8], g.raw[9], g.raw[10], g.raw[11], g.raw[12], g.raw[13], g.raw[14], g.raw[15]);
    return std::string(buf);
}

int ib_bring_qp_rts(const QpBringUpParams& p) {
    ibv_qp_attr attr{};

    // RESET -> INIT
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = p.port;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_ATOMIC | IBV_ACCESS_LOCAL_WRITE;
    int fl = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(p.qp, &attr, fl)) {
        ERRNO("modify_qp INIT");
        return -1;
    }

    // INIT -> RTR
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = (ibv_mtu)p.path_mtu;
    attr.dest_qp_num        = p.remote_qpn;
    attr.rq_psn             = p.remote_psn;
    attr.max_dest_rd_atomic = p.max_dest_rd_atomic;
    attr.min_rnr_timer      = p.min_rnr_timer;
    attr.ah_attr.is_global  = 1;
    attr.ah_attr.grh.dgid   = p.dgid;
    attr.ah_attr.grh.sgid_index = (uint8_t)p.sgid_index;
    attr.ah_attr.grh.hop_limit   = 1;
    attr.ah_attr.grh.traffic_class = 0;
    attr.ah_attr.grh.flow_label    = 0;
    attr.ah_attr.sl           = 0;
    attr.ah_attr.port_num     = p.port;
    attr.ah_attr.src_path_bits = 0;
    fl = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
         IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(p.qp, &attr, fl)) {
        ERRNO("modify_qp RTR");
        return -1;
    }

    // RTR -> RTS
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = (uint8_t)p.timeout;
    attr.retry_cnt     = (uint8_t)p.retry_cnt;
    attr.rnr_retry     = (uint8_t)p.rnr_retry;
    attr.sq_psn        = p.local_psn;
    attr.max_rd_atomic = p.max_rd_atomic;
    fl = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
         IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(p.qp, &attr, fl)) {
        ERRNO("modify_qp RTS");
        return -1;
    }

    return 0;
}

int ib_mark_qp_error(ibv_qp* qp) {
    ibv_qp_attr attr{};
    attr.qp_state = IBV_QPS_ERR;
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE);
}

const char* wc_status_name(enum ibv_wc_status s) {
    switch (s) {
    case IBV_WC_SUCCESS:              return "SUCCESS";
    case IBV_WC_LOC_LEN_ERR:          return "LOC_LEN_ERR";
    case IBV_WC_LOC_QP_OP_ERR:        return "LOC_QP_OP_ERR";
    case IBV_WC_LOC_EEC_OP_ERR:       return "LOC_EEC_OP_ERR";
    case IBV_WC_LOC_PROT_ERR:         return "LOC_PROT_ERR";
    case IBV_WC_WR_FLUSH_ERR:         return "WR_FLUSH_ERR";
    case IBV_WC_MW_BIND_ERR:          return "MW_BIND_ERR";
    case IBV_WC_BAD_RESP_ERR:         return "BAD_RESP_ERR";
    case IBV_WC_LOC_ACCESS_ERR:       return "LOC_ACCESS_ERR";
    case IBV_WC_REM_INV_REQ_ERR:      return "REM_INV_REQ_ERR";
    case IBV_WC_REM_ACCESS_ERR:       return "REM_ACCESS_ERR";
    case IBV_WC_REM_OP_ERR:           return "REM_OP_ERR";
    case IBV_WC_RETRY_EXC_ERR:        return "RETRY_EXC_ERR";
    case IBV_WC_RNR_RETRY_EXC_ERR:    return "RNR_RETRY_EXC_ERR";
    case IBV_WC_LOC_RDD_VIOL_ERR:     return "LOC_RDD_VIOL_ERR";
    case IBV_WC_REM_INV_RD_REQ_ERR:   return "REM_INV_RD_REQ_ERR";
    case IBV_WC_REM_ABORT_ERR:        return "REM_ABORT_ERR";
    case IBV_WC_INV_EECN_ERR:         return "INV_EECN_ERR";
    case IBV_WC_INV_EEC_STATE_ERR:    return "INV_EEC_STATE_ERR";
    case IBV_WC_FATAL_ERR:            return "FATAL_ERR";
    case IBV_WC_RESP_TIMEOUT_ERR:     return "RESP_TIMEOUT_ERR";
    case IBV_WC_GENERAL_ERR:          return "GENERAL_ERR";
    default:                          return "UNKNOWN";
    }
}

bool wc_is_fatal(enum ibv_wc_status s) {
    return s != IBV_WC_SUCCESS;
}
