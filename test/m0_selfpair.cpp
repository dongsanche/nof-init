// M0: validate raw-verbs RC QP bring-up (RESET -> INIT -> RTR -> RTS) without CM.
// Two QPs are created on the same rxe device/port and manually cross-configured,
// then a SEND/RECV ping-pong proves the data path works.
#include "verbs.hpp"
#include "common.hpp"

#include <cstring>
#include <string>
#include <vector>

static const int PORT = 1;
static const int QDEPTH = 16;

struct Qp {
    ibv_qp* qp = nullptr;
    ibv_cq* cq = nullptr;
    uint32_t qpn = 0;
    uint32_t psn = 0;
};

int main(int argc, char** argv) {
    const char* devname = argc > 1 ? argv[1] : nullptr;
    ibv_context* ctx = ib_open_device(devname);
    if (!ctx) return 1;

    ibv_device_attr dev_attr{};
    if (ibv_query_device(ctx, &dev_attr)) { ERRNO("ibv_query_device"); return 1; }
    LOG("max_qp=%d max_qp_wr=%d max_sge=%d max_cqe=%d max_qp_rd_atom=%d",
        dev_attr.max_qp, dev_attr.max_qp_wr, dev_attr.max_sge,
        dev_attr.max_cqe, dev_attr.max_qp_rd_atom);

    ib_dump_gid_table(ctx, PORT);

    ibv_gid gid{};
    int sgid_index = 0;
    if (ib_pick_gid(ctx, PORT, &gid, &sgid_index)) return 1;
    LOG("using GID[%d] = %s", sgid_index, gid_to_string(gid).c_str());

    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) { ERRNO("ibv_alloc_pd"); return 1; }

    // --- create two QPs (A = "active", B = "passive") on the same device ---
    auto create_qp = [&](Qp& q) {
        q.cq = ibv_create_cq(ctx, QDEPTH, nullptr, nullptr, 0);
        if (!q.cq) { ERRNO("ibv_create_cq"); return -1; }
        ibv_qp_init_attr ia{};
        ia.send_cq = q.cq;
        ia.recv_cq = q.cq;
        ia.srq = nullptr;
        ia.cap.max_send_wr = QDEPTH;
        ia.cap.max_recv_wr = QDEPTH;
        ia.cap.max_send_sge = 1;
        ia.cap.max_recv_sge = 1;
        ia.cap.max_inline_data = 0;
        ia.qp_type = IBV_QPT_RC;
        ia.sq_sig_all = 1;
        q.qp = ibv_create_qp(pd, &ia);
        if (!q.qp) { ERRNO("ibv_create_qp"); return -1; }
        q.qpn = q.qp->qp_num;
        return 0;
    };

    Qp qa, qb;
    if (create_qp(qa) || create_qp(qb)) return 1;
    qa.psn = 0x1234;
    qb.psn = 0x5678;
    LOG("QPA qpn=%u psn=0x%x ; QPB qpn=%u psn=0x%x", qa.qpn, qa.psn, qb.qpn, qb.psn);

    // --- cross configure: A talks to B, B talks to A (same local GID) ---
    QpBringUpParams pa;
    pa.qp = qa.qp; pa.port = PORT; pa.remote_qpn = qb.qpn; pa.local_psn = qa.psn;
    pa.remote_psn = qb.psn; pa.sgid_index = sgid_index; pa.dgid = gid;

    QpBringUpParams pb;
    pb.qp = qb.qp; pb.port = PORT; pb.remote_qpn = qa.qpn; pb.local_psn = qb.psn;
    pb.remote_psn = qa.psn; pb.sgid_index = sgid_index; pb.dgid = gid;

    if (ib_bring_qp_rts(pa)) { ERR("bring QPA to RTS failed"); return 1; }
    if (ib_bring_qp_rts(pb)) { ERR("bring QPB to RTS failed"); return 1; }
    LOG("both QPs in RTS");

    // --- ping-pong: A sends to B ---
    std::vector<char> sendbuf(64, 'x');
    std::vector<char> recvbuf(64, '\0');
    std::strcpy(sendbuf.data(), "hello-roce");

    ibv_mr* mr_send = ibv_reg_mr(pd, sendbuf.data(), sendbuf.size(),
                                 IBV_ACCESS_LOCAL_WRITE);
    ibv_mr* mr_recv = ibv_reg_mr(pd, recvbuf.data(), recvbuf.size(),
                                 IBV_ACCESS_LOCAL_WRITE);
    if (!mr_send || !mr_recv) { ERRNO("ibv_reg_mr"); return 1; }

    // B posts a RECV first
    ibv_sge rsge{};
    rsge.addr = (uintptr_t)recvbuf.data();
    rsge.length = recvbuf.size();
    rsge.lkey = mr_recv->lkey;
    ibv_recv_wr rwr{};
    rwr.wr_id = 0xB00;
    rwr.sg_list = &rsge;
    rwr.num_sge = 1;
    ibv_recv_wr* bad_r = nullptr;
    if (ibv_post_recv(qb.qp, &rwr, &bad_r)) { ERRNO("ibv_post_recv"); return 1; }

    // A posts a SEND
    ibv_sge ssge{};
    ssge.addr = (uintptr_t)sendbuf.data();
    ssge.length = sendbuf.size();
    ssge.lkey = mr_send->lkey;
    ibv_send_wr swr{};
    swr.wr_id = 0xA00;
    swr.sg_list = &ssge;
    swr.num_sge = 1;
    swr.opcode = IBV_WR_SEND;
    swr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad_s = nullptr;
    if (ibv_post_send(qa.qp, &swr, &bad_s)) { ERRNO("ibv_post_send"); return 1; }

    // poll A: SEND completion
    bool a_done = false, b_done = false;
    for (int i = 0; i < 1000 && !(a_done && b_done); i++) {
        ibv_wc wc{};
        int n = ibv_poll_cq(qa.cq, 1, &wc);
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) { ERR("A WC status %d", wc.status); return 1; }
            LOG("A WC: opcode=%d wr_id=0x%llx", wc.opcode, (unsigned long long)wc.wr_id);
            a_done = true;
        }
        n = ibv_poll_cq(qb.cq, 1, &wc);
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) { ERR("B WC status %d", wc.status); return 1; }
            LOG("B WC: opcode=%d wr_id=0x%llx byte_len=%u",
                wc.opcode, (unsigned long long)wc.wr_id, wc.byte_len);
            b_done = true;
        }
    }

    if (!a_done || !b_done) { ERR("ping-pong did not complete"); return 1; }

    if (std::strcmp(sendbuf.data(), recvbuf.data()) == 0) {
        LOG("M0 PASS: SEND/RECV round-trip OK: '%s'", recvbuf.data());
    } else {
        ERR("M0 FAIL: data mismatch");
        return 1;
    }

    ibv_dereg_mr(mr_send);
    ibv_dereg_mr(mr_recv);
    ibv_destroy_qp(qa.qp);
    ibv_destroy_qp(qb.qp);
    ibv_destroy_cq(qa.cq);
    ibv_destroy_cq(qb.cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return 0;
}
