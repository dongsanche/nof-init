#pragma once

// Shared NVMe-oF/RDMA initiator connection helpers (admin + I/O queue).
// Used by nof_init, stress_init and lifecycle_test. Needs a live RDMA target.
#include "cm.hpp"
#include "nvme.hpp"
#include "verbs.hpp"
#include "common.hpp"

#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <vector>
#include <chrono>

namespace nof {

static const char* SRC_IP  = "192.168.255.20";
static const char* DST_IP  = "192.168.255.10";
static const char* IFNAME  = "ens33";
static const uint16_t QD   = 32;   // admin queue must fit kernel NVME_AQ_DEPTH=32
static const char* SUBNQN  = "nqn.2016-06.io.spdk:cnode1";
static const char* HOSTNQN = "nqn.2014-08.org.nvmexpress:uuid:00000000-0000-0000-0000-000000000000";

struct Conn {
    ibv_qp* qp = nullptr;
    ibv_cq* cq = nullptr;
    std::vector<uint8_t> rsp;      // 64-byte single-response scratch
    ibv_mr* rsp_mr = nullptr;
    uint32_t remote_qpn = 0, remote_psn = 0, remote_comm_id = 0;
    uint64_t local_comm_id = 0, tid = 0;
};

inline ibv_gid ip_to_gid(const char* ip) {
    ibv_gid g{}; struct in_addr a;
    inet_pton(AF_INET, ip, &a);
    g.raw[10] = 0xff; g.raw[11] = 0xff;
    memcpy(g.raw + 12, &a.s_addr, 4);
    return g;
}

inline std::vector<uint8_t> build_req_private(uint16_t qid, uint16_t hrqsize, uint16_t hsqsize,
                                              uint16_t cntlid, const char* src_ip) {
    std::vector<uint8_t> pd(92, 0);
    pd[0] = 0x00; pd[1] = 0x40; put_be16(pd.data() + 2, 0xddd4);
    struct in_addr src, dst; inet_pton(AF_INET, src_ip, &src); inet_pton(AF_INET, DST_IP, &dst);
    memcpy(pd.data() + 16, &src.s_addr, 4); memcpy(pd.data() + 32, &dst.s_addr, 4);
    uint8_t* s = pd.data() + 36;
    put_le16(s + 0, 0); put_le16(s + 2, qid); put_le16(s + 4, hrqsize); put_le16(s + 6, hsqsize); put_le16(s + 8, cntlid);
    return pd;
}

// CM handshake + QP INIT->RTR->RTS + RTU. No recv is posted yet.
// rep_timeout_ms caps how long to wait for the CM REP (0 => default 50s).
inline bool establish_conn(ibv_context* ctx, ibv_pd* pd, uint16_t qid, uint16_t cntlid, uint16_t qd,
                           ibv_gid local_gid, int sgid_idx, ibv_gid remote_gid, uint64_t node_guid,
                           Conn& c, int rep_timeout_ms = 50000) {
    c.cq = ibv_create_cq(ctx, qd * 2 + 16, nullptr, nullptr, 0);
    if (!c.cq) return false;
    ibv_qp_init_attr ia{};
    ia.send_cq = c.cq; ia.recv_cq = c.cq;
    ia.cap.max_send_wr = qd; ia.cap.max_recv_wr = qd; ia.cap.max_send_sge = 2; ia.cap.max_recv_sge = 2;
    ia.qp_type = IBV_QPT_RC; ia.sq_sig_all = 1;
    c.qp = ibv_create_qp(pd, &ia);
    if (!c.qp) { ibv_destroy_cq(c.cq); c.cq = nullptr; return false; }
    uint32_t local_qpn = c.qp->qp_num;
    uint32_t local_psn = 0x123456 + qid;

    c.local_comm_id = ((uint64_t)rand() << 32) | (rand() & 0xffffffff);
    c.tid = c.local_comm_id;

    std::vector<uint8_t> body(MAD_BODY_LEN, 0);
    put_be32(body.data() + req_off::LOCAL_COMM_ID, (uint32_t)c.local_comm_id);
    put_be64(body.data() + req_off::SERVICE_ID, 0x01061144ull);
    put_be64(body.data() + req_off::LOCAL_CA_GUID, node_guid);
    put_be24(body.data() + req_off::LOCAL_QPN, local_qpn);
    body[req_off::RESPONDER_RESOURCES] = 128;
    body[req_off::INITIATOR_DEPTH] = 0;
    body[43] = 0xa1;
    put_be24(body.data() + req_off::STARTING_PSN, local_psn);
    body[47] = 0xa7;
    put_be16(body.data() + req_off::PARTITION_KEY, 0xffff);
    body[50] = 0x37; body[51] = 0xf0;
    put_be16(body.data() + 52, 0xffff); put_be16(body.data() + 54, 0xffff);
    memcpy(body.data() + req_off::PRIMARY_LOCAL_GID, local_gid.raw, 16);
    memcpy(body.data() + req_off::PRIMARY_REMOTE_GID, remote_gid.raw, 16);
    body[req_off::PRIMARY_TRAFFIC_CLASS] = 0; body[req_off::PRIMARY_HOP_LIMIT] = 64;
    body[94] = 0x00; body[95] = 0x98;
    auto pdata = build_req_private(qid, qd, qd - 1, cntlid, SRC_IP);
    memcpy(body.data() + req_off::PRIVATE_DATA, pdata.data(), pdata.size());

    auto pkt = build_cm_packet(CM_REQ_ATTR, c.tid, body);
    CmChannel ch;
    if (!ch.open(IFNAME, SRC_IP, DST_IP, 4791)) return false;
    if (!ch.send(pkt)) { ch.close(); return false; }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(rep_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<uint8_t> pl;
        if (ch.recv(pl, 200)) {
            ParsedMad mad;
            if (!parse_cm_packet(pl, mad)) continue;
            if (mad.attr_id == CM_REP_ATTR) {
                c.remote_qpn = get_be24(mad.body.data() + rep_off::LOCAL_QPN);
                c.remote_psn = get_be24(mad.body.data() + rep_off::STARTING_PSN);
                c.remote_comm_id = get_be32(mad.body.data() + rep_off::LOCAL_COMM_ID);
                QpBringUpParams bp;
                bp.qp = c.qp; bp.port = 1; bp.remote_qpn = c.remote_qpn;
                bp.local_psn = c.remote_psn; bp.remote_psn = local_psn;
                bp.sgid_index = sgid_idx; bp.dgid = remote_gid;
                if (ib_bring_qp_rts(bp)) { ch.close(); return false; }
                std::vector<uint8_t> rtu(MAD_BODY_LEN, 0);
                put_be32(rtu.data() + rtu_off::LOCAL_COMM_ID, (uint32_t)c.local_comm_id);
                put_be32(rtu.data() + rtu_off::REMOTE_COMM_ID, c.remote_comm_id);
                ch.send(build_cm_packet(CM_RTU_ATTR, c.tid, rtu));
                ch.close();
                usleep(500000);
                c.rsp.assign(64, 0);
                c.rsp_mr = ibv_reg_mr(pd, c.rsp.data(), c.rsp.size(), IBV_ACCESS_LOCAL_WRITE);
                if (!c.rsp_mr) return false;
                return true;
            }
        }
    }
    ch.close();
    return false;
}

// Drain any leftover (typically SEND) completions from a queue's CQ.
inline void drain_cq(Conn& c) {
    ibv_wc wc{};
    while (ibv_poll_cq(c.cq, 1, &wc) > 0) {}
}

// Fabric Connect. Posts its own single recv, waits, returns cntlid.
inline bool fabric_connect(Conn& c, ibv_pd* pd, uint16_t qid, uint16_t cntlid_in, uint16_t qd,
                           uint16_t& cntlid_out, uint32_t kato_ms = 0) {
    std::vector<uint8_t> data(nvme::CONNECT_DATA_LEN, 0);
    nvme::build_connect_data(data.data(), cntlid_in, SUBNQN, HOSTNQN);
    ibv_mr* dm = ibv_reg_mr(pd, data.data(), data.size(), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!dm) return false;
    uint8_t sqe[64] = {0};
    nvme::build_connect_sqe(sqe, qid, qd - 1, (uint64_t)data.data(), dm->rkey, kato_ms);
    ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    if (!sm) { ibv_dereg_mr(dm); return false; }

    memset(c.rsp.data(), 0, c.rsp.size());
    ibv_sge rs{ (uintptr_t)c.rsp.data(), (uint32_t)c.rsp.size(), c.rsp_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id = 1; rwr.sg_list = &rs; rwr.num_sge = 1;
    ibv_recv_wr* rbad = nullptr;
    if (ibv_post_recv(c.qp, &rwr, &rbad)) { ibv_dereg_mr(sm); ibv_dereg_mr(dm); return false; }
    ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
    ibv_send_wr wr{}; wr.wr_id = 0; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(c.qp, &wr, &bad)) { ibv_dereg_mr(sm); ibv_dereg_mr(dm); return false; }
    bool ok = false;
    for (int i = 0; i < 2000; i++) {
        ibv_wc wc{}; int n = ibv_poll_cq(c.cq, 1, &wc);
        if (n <= 0) { usleep(1000); continue; }
        if (wc.status != IBV_WC_SUCCESS) break;
        if (wc.opcode == IBV_WC_RECV) {
            uint16_t st = c.rsp[14] | (c.rsp[15] << 8);
            cntlid_out = c.rsp[0] | (c.rsp[1] << 8);
            ok = (st == 0);
            break;
        }
    }
    drain_cq(c);
    ibv_dereg_mr(sm);
    ibv_dereg_mr(dm);
    return ok;
}

// Single-shot synchronous admin command (no data). Posts its own recv, waits.
// Returns the raw CQE status (0 = success); optionally fills result_out with CQE DW0.
// Used by CC.EN, Keep Alive, Set/Get Features.
inline uint16_t admin_submit_sync(Conn& admin, ibv_pd* pd, uint8_t sqe[64], uint32_t* result_out = nullptr) {
    ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    if (!sm) return 0xffff;
    memset(admin.rsp.data(), 0, admin.rsp.size());
    ibv_sge rs{ (uintptr_t)admin.rsp.data(), (uint32_t)admin.rsp.size(), admin.rsp_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id = 1; rwr.sg_list = &rs; rwr.num_sge = 1;
    ibv_recv_wr* rbad = nullptr;
    if (ibv_post_recv(admin.qp, &rwr, &rbad)) { ibv_dereg_mr(sm); return 0xffff; }
    ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
    ibv_send_wr wr{}; wr.wr_id = 0; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(admin.qp, &wr, &bad)) { ibv_dereg_mr(sm); return 0xffff; }
    uint16_t st = 0xffff;
    for (int i = 0; i < 2000; i++) {
        ibv_wc wc{}; int n = ibv_poll_cq(admin.cq, 1, &wc);
        if (n <= 0) { usleep(1000); continue; }
        if (wc.status != IBV_WC_SUCCESS) { ERR("admin CQ error: %s", wc_status_name(wc.status)); break; }
        if (wc.opcode == IBV_WC_RECV) { st = admin.rsp[14] | (admin.rsp[15] << 8); if (result_out) *result_out = get_le32(admin.rsp.data()); break; }
    }
    drain_cq(admin);
    ibv_dereg_mr(sm);
    return st;
}

// Enable controller (CC.EN) via Property Set on the admin queue.
inline bool enable_controller(Conn& admin, ibv_pd* pd) {
    uint8_t ps[64] = {0};
    nvme::build_prop_set(ps, nvme::CC_REG, nvme::CC_ENABLE);
    return admin_submit_sync(admin, pd, ps) == 0;
}

inline void teardown_conn(Conn& c);

// Establish the admin queue + Fabric Connect + CC.EN with retries (target may be down).
// Returns true once connected. Each attempt uses a short CM timeout so a dead target
// is detected quickly instead of waiting the full 50s. Logs per-attempt progress with
// elapsed time + failure stage.
inline bool connect_admin_with_retry(ibv_context* ctx, ibv_pd* pd,
                                     ibv_gid local_gid, int sgid_idx, ibv_gid remote_gid,
                                     uint64_t node_guid, Conn& admin, uint16_t& cntlid,
                                     int max_attempts = 30, int attempt_delay_ms = 500) {
    auto t0 = std::chrono::steady_clock::now();
    auto el = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    for (int i = 1; i <= max_attempts; i++) {
        const char* stage = "establish";
        if (establish_conn(ctx, pd, 0, 0, QD, local_gid, sgid_idx, remote_gid, node_guid, admin, 3000)) {
            stage = "connect";
            if (fabric_connect(admin, pd, 0, 0xffff, QD, cntlid)) {
                stage = "enable";
                if (enable_controller(admin, pd)) {
                    LOG("reconnect attempt %d: OK (cntlid=%u) at t=%.3fs", i, cntlid, el());
                    return true;
                }
            }
        }
        LOG("reconnect attempt %d: FAIL at '%s' at t=%.3fs", i, stage, el());
        teardown_conn(admin);
        usleep(attempt_delay_ms * 1000);
    }
    return false;
}

// Send a Keep Alive command on the admin queue; returns true on success.
inline bool send_keep_alive(Conn& admin, ibv_pd* pd, uint16_t cid) {
    uint8_t ka[64] = {0};
    nvme::build_keep_alive(ka, cid);
    return admin_submit_sync(admin, pd, ka) == 0;
}

// Set Features (Number of Queues): request ncqr/nsqr 1-based; fills allocated (1-based NSQ).
inline bool set_num_queues(Conn& admin, ibv_pd* pd, uint16_t ncqr, uint16_t nsqr, uint16_t& allocated) {
    uint8_t sqe[64] = {0};
    nvme::build_set_features(sqe, 1, nvme::FID_NUM_QUEUES, nvme::nqr_request(ncqr, nsqr));
    uint32_t res = 0;
    uint16_t st = admin_submit_sync(admin, pd, sqe, &res);
    allocated = nvme::nqr_allocated(res);
    return st == 0;
}

// Get Features (Number of Queues): fills nqr = 1-based number of I/O submission queues.
inline bool get_num_queues(Conn& admin, ibv_pd* pd, uint16_t& nqr) {
    uint8_t sqe[64] = {0};
    nvme::build_get_features(sqe, 1, nvme::FID_NUM_QUEUES);
    uint32_t res = 0;
    uint16_t st = admin_submit_sync(admin, pd, sqe, &res);
    nqr = nvme::nqr_allocated(res);
    return st == 0;
}

// Abort a previously submitted command (sqid/cid). Result DW0 bit 0 = aborted.
inline bool abort_cmd(Conn& admin, ibv_pd* pd, uint16_t sqid, uint16_t abort_cid, uint32_t& result) {
    uint8_t sqe[64] = {0};
    nvme::build_abort(sqe, 1, sqid, abort_cid);
    result = 0;
    uint16_t st = admin_submit_sync(admin, pd, sqe, &result);
    return st == 0;
}

// Property Get: read a controller register into `value` (CQE result, 64-bit).
inline bool prop_get(Conn& admin, ibv_pd* pd, uint8_t attrib, uint32_t ofst, uint64_t& value) {
    uint8_t sqe[64] = {0};
    nvme::build_prop_get(sqe, attrib, ofst);
    uint16_t st = admin_submit_sync(admin, pd, sqe);
    if (st != 0) { value = 0; return false; }
    value = get_le64(admin.rsp.data());
    return true;
}

// Graceful shutdown: CC.SHN = normal, (best-effort) poll CSTS.SHST, then CC.EN = 0.
inline bool shutdown_controller(Conn& admin, ibv_pd* pd) {
    uint8_t sqe[64] = {0};
    nvme::build_prop_set(sqe, nvme::CC_REG, nvme::CC_ENABLE | nvme::CC_SHN_NORMAL);
    if (admin_submit_sync(admin, pd, sqe) != 0) return false;
    for (int i = 0; i < 20; i++) {
        uint64_t csts = 0;
        if (prop_get(admin, pd, 0, nvme::CSTS_REG, csts) &&
            ((csts >> nvme::CSTS_SHST_SHIFT) & 0x3) == nvme::CSTS_SHST_COMPLETE) break;
        usleep(10000);
    }
    nvme::build_prop_set(sqe, nvme::CC_REG, 0);
    return admin_submit_sync(admin, pd, sqe) == 0;
}

// Identify (admin, data-in via RDMA write). Fills `data` with the 4096-byte
// structure; returns true on success. cns=1 => controller, cns=0 => namespace.
inline bool identify(Conn& admin, ibv_pd* pd, uint16_t cid, uint32_t cns, uint32_t nsid,
                     std::vector<uint8_t>& data) {
    data.assign(nvme::IDENTIFY_DATA_SIZE, 0);
    ibv_mr* dmr = ibv_reg_mr(pd, data.data(), data.size(),
                             IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!dmr) return false;
    uint8_t sqe[64] = {0};
    nvme::build_identify(sqe, cid, cns, nsid, (uint64_t)data.data(), dmr->rkey, (uint32_t)data.size());
    ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    if (!sm) { ibv_dereg_mr(dmr); return false; }

    memset(admin.rsp.data(), 0, admin.rsp.size());
    ibv_sge rs{ (uintptr_t)admin.rsp.data(), (uint32_t)admin.rsp.size(), admin.rsp_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id = 1; rwr.sg_list = &rs; rwr.num_sge = 1;
    ibv_recv_wr* rbad = nullptr;
    if (ibv_post_recv(admin.qp, &rwr, &rbad)) { ibv_dereg_mr(sm); ibv_dereg_mr(dmr); return false; }
    ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
    ibv_send_wr wr{}; wr.wr_id = 0; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(admin.qp, &wr, &bad)) { ibv_dereg_mr(sm); ibv_dereg_mr(dmr); return false; }
    bool ok = false;
    for (int i = 0; i < 2000; i++) {
        ibv_wc wc{}; int n = ibv_poll_cq(admin.cq, 1, &wc);
        if (n <= 0) { usleep(1000); continue; }
        if (wc.status != IBV_WC_SUCCESS) break;
        if (wc.opcode == IBV_WC_RECV) { uint16_t st = admin.rsp[14] | (admin.rsp[15] << 8); ok = (st == 0); break; }
    }
    ibv_dereg_mr(sm);
    ibv_dereg_mr(dmr);
    return ok;
}

// Graceful teardown: CM DREQ/DREP, then QP -> ERR, then destroy QP/CQ/MR.
inline void teardown_conn(Conn& c) {
    if (!c.qp) return;
    cm_disconnect(IFNAME, SRC_IP, DST_IP, (uint32_t)c.local_comm_id,
                  c.remote_comm_id, c.remote_qpn, c.tid);
    ib_mark_qp_error(c.qp);
    if (c.rsp_mr) { ibv_dereg_mr(c.rsp_mr); c.rsp_mr = nullptr; }
    if (c.qp) { ibv_destroy_qp(c.qp); c.qp = nullptr; }
    if (c.cq) { ibv_destroy_cq(c.cq); c.cq = nullptr; }
}

} // namespace nof
