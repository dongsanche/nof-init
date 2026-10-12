// M1: self-written IB CM handshake (REQ/REP) against SPDK target over RoCE.
#include "cm.hpp"
#include "verbs.hpp"
#include "common.hpp"

#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace nof;

static const char*  DST_IP   = "192.168.255.10";
static const uint16_t DST_PORT = 4420;
static const char*  IFNAME    = "ens33";
static const uint16_t QD       = 64;   // queue depth

// RoCEv2 IPv4-mapped GID: ::ffff:a.b.c.d
static ibv_gid ip_to_gid(const char* ipstr) {
    ibv_gid g{};
    struct in_addr a;
    inet_pton(AF_INET, ipstr, &a);
    g.raw[10] = 0xff; g.raw[11] = 0xff;
    std::memcpy(g.raw + 12, &a.s_addr, 4);
    return g;
}

// Build the 92-byte REQ private data = cma_hdr(36) + spdk req(32) + zeros(24)
static std::vector<uint8_t> build_req_private(uint16_t qid, uint16_t hrqsize,
                                              uint16_t hsqsize, uint16_t cntlid) {
    std::vector<uint8_t> pd(92, 0);
    // cma_hdr (36 bytes)
    pd[0] = 0x00;          // cma_version
    pd[1] = 0x40;          // ip_version = 4 << 4
    put_be16(pd.data() + 2, 0xddd4);  // port (transient)
    struct in_addr src, dst;
    inet_pton(AF_INET, "192.168.255.20", &src);   // local (rxe0 ens33)
    inet_pton(AF_INET, DST_IP, &dst);
    std::memcpy(pd.data() + 16, &src.s_addr, 4);  // src_addr.ip4.addr
    std::memcpy(pd.data() + 32, &dst.s_addr, 4);  // dst_addr.ip4.addr

    // spdk_nvmf_rdma_request_private_data (32B) at +36 (little-endian uint16 host order)
    uint8_t* s = pd.data() + 36;
    put_be16(s + 0, 0);       // recfmt
    put_be16(s + 2, qid);     // qid (admin=0)
    put_be16(s + 4, hrqsize); // little-endian would differ; use LE for these fields
    put_be16(s + 6, hsqsize);
    put_be16(s + 8, cntlid);
    return pd;
}

int main() {
    ibv_context* ctx = ib_open_device(nullptr);
    if (!ctx) return 1;
    ibv_device_attr dev_attr{};
    ibv_query_device(ctx, &dev_attr);

    ibv_gid local_gid{};
    int sgid_idx = 0;
    if (ib_pick_gid(ctx, 1, &local_gid, &sgid_idx)) return 1;
    LOG("local GID[%d] = %s", sgid_idx, gid_to_string(local_gid).c_str());
    LOG("CA GUID = %016llx", (unsigned long long)dev_attr.sys_image_guid);

    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) return 1;
    ibv_cq* cq = ibv_create_cq(ctx, QD * 2, nullptr, nullptr, 0);
    ibv_qp_init_attr ia{};
    ia.send_cq = cq; ia.recv_cq = cq;
    ia.cap.max_send_wr = QD; ia.cap.max_recv_wr = QD;
    ia.cap.max_send_sge = 2; ia.cap.max_recv_sge = 2;
    ia.qp_type = IBV_QPT_RC; ia.sq_sig_all = 1;
    ibv_qp* qp = ibv_create_qp(pd, &ia);
    if (!qp) return 1;
    uint32_t local_qpn = qp->qp_num;
    uint32_t local_psn = 0x123456;
    LOG("local QPN=%u PSN=0x%x", local_qpn, local_psn);

    // ---- build REQ body (232 bytes) ----
    std::vector<uint8_t> body(MAD_BODY_LEN, 0);
    uint64_t local_comm_id = (uint64_t)rand() << 32 | (rand() & 0xffffffff);
    put_be32(body.data() + req_off::LOCAL_COMM_ID, (uint32_t)local_comm_id);
    put_be64(body.data() + req_off::SERVICE_ID, 0x01061144ull);  // RDMA_PS_TCP<<16 | 4420
    put_be64(body.data() + req_off::LOCAL_CA_GUID, dev_attr.sys_image_guid);
    put_be24(body.data() + req_off::LOCAL_QPN, local_qpn);
    body[req_off::RESPONDER_RESOURCES] = 128;
    body[req_off::INITIATOR_DEPTH] = 0;
    body[43] = 0xa1;
    put_be24(body.data() + req_off::STARTING_PSN, local_psn);
    body[47] = 0xa7;
    put_be16(body.data() + req_off::PARTITION_KEY, 0xffff);
    body[50] = 0x37;
    body[51] = 0xf0;
    put_be16(body.data() + 52, 0xffff); // local lid
    put_be16(body.data() + 54, 0xffff); // remote lid
    std::memcpy(body.data() + req_off::PRIMARY_LOCAL_GID, local_gid.raw, 16);
    ibv_gid remote_gid = ip_to_gid(DST_IP);
    std::memcpy(body.data() + req_off::PRIMARY_REMOTE_GID, remote_gid.raw, 16);
    body[req_off::PRIMARY_TRAFFIC_CLASS] = 0;
    body[req_off::PRIMARY_HOP_LIMIT] = 64;
    body[94] = 0x00;
    body[95] = 0x98;

    auto pdata = build_req_private(0 /*qid*/, QD + 1, QD, 0 /*cntlid*/);
    std::memcpy(body.data() + req_off::PRIVATE_DATA, pdata.data(), pdata.size());

    uint64_t tid = local_comm_id;
    auto pkt = build_cm_packet(CM_REQ_ATTR, tid, body);

    CmChannel ch;
    if (!ch.open(IFNAME, "192.168.255.20", DST_IP, 4791)) return 1;
    if (!ch.send(pkt)) { ERR("send REQ failed"); return 1; }
    LOG("REQ sent (%zu bytes)", pkt.size());

    // ---- wait for REP ----
    for (int i = 0; i < 50; i++) {
        std::vector<uint8_t> pl;
        if (ch.recv(pl, 1000)) {
            LOG("recv payload len=%zu first=%02x%02x%02x%02x", pl.size(),
                pl.size() > 0 ? pl[0] : 0, pl.size() > 1 ? pl[1] : 0,
                pl.size() > 2 ? pl[2] : 0, pl.size() > 3 ? pl[3] : 0);
            ParsedMad mad;
            if (!parse_cm_packet(pl, mad)) { ERR("parse failed"); return 1; }
            LOG("got MAD attr=0x%04x tid=%llx", mad.attr_id, (unsigned long long)mad.tid);
            if (mad.attr_id == CM_REP_ATTR) {
                uint32_t rqpn = get_be24(mad.body.data() + rep_off::LOCAL_QPN);
                uint32_t rpsn = get_be24(mad.body.data() + rep_off::STARTING_PSN);
                uint32_t remote_comm_id = get_be32(mad.body.data() + rep_off::LOCAL_COMM_ID);
                LOG("REP: remote QPN=%u PSN=0x%x comm_id=0x%x", rqpn, rpsn, remote_comm_id);

                // bring local QP to RTS using target's QPN/PSN/GID
                QpBringUpParams bp;
                bp.qp = qp; bp.port = 1;
                bp.remote_qpn = rqpn;
                // NOTE: kernel rdma_cm swaps sq_psn/rq_psn (cma.c:1124, cm.c:2093):
                //   target sq_psn = our PSN, target rq_psn = target's own PSN.
                // So we must send FROM the target's PSN and receive FROM our own PSN.
                bp.local_psn = rpsn;         // -> sq_psn = target's PSN
                bp.remote_psn = local_psn;   // -> rq_psn = our PSN
                bp.sgid_index = sgid_idx;
                bp.dgid = remote_gid;
                if (ib_bring_qp_rts(bp)) { ERR("bring QP to RTS failed"); return 1; }
                LOG("local QP in RTS");

                // build + send RTU
                std::vector<uint8_t> rtu(MAD_BODY_LEN, 0);
                put_be32(rtu.data() + rtu_off::LOCAL_COMM_ID, (uint32_t)local_comm_id);
                put_be32(rtu.data() + rtu_off::REMOTE_COMM_ID, remote_comm_id);
                auto rtu_pkt = build_cm_packet(CM_RTU_ATTR, tid, rtu);
                if (!ch.send(rtu_pkt)) { ERR("send RTU failed"); return 1; }
                LOG("RTU sent");

                LOG("M1 PASS: CM handshake complete (REQ->REP->RTU), QP in RTS");
                ch.close();

                // let the target finish QP RTS + post RECV
                usleep(1000000);

                // ---- M2: post RECV + send Fabric Connect ----
                std::vector<uint8_t> rsp_buf(256, 0);
                ibv_mr* rsp_mr = ibv_reg_mr(pd, rsp_buf.data(), rsp_buf.size(), IBV_ACCESS_LOCAL_WRITE);
                if (!rsp_mr) { ERR("reg rsp mr"); return 1; }
                {
                    struct ibv_sge sge{};
                    sge.addr = (uintptr_t)rsp_buf.data();
                    sge.length = rsp_buf.size();
                    sge.lkey = rsp_mr->lkey;
                    struct ibv_recv_wr wr{};
                    wr.wr_id = 1;
                    wr.sg_list = &sge;
                    wr.num_sge = 1;
                    struct ibv_recv_wr* bad = nullptr;
                    if (ibv_post_recv(qp, &wr, &bad)) { ERRNO("post_recv"); return 1; }
                }
                LOG("posted RECV");

                // build connect command capsule: SQE(64) + connect data(1024)
                std::vector<uint8_t> cap(64 + 1024, 0);
                uint8_t* sqe = cap.data();
                sqe[0] = 0x7f;                    // opcode = fabric
                sqe[1] = 0x40;                    // psdt = SGL_MPTR_CONTIG (bits 6-7)
                put_le16(sqe + 2, 0);             // cid
                sqe[4] = 0x01;                    // fctype = CONNECT
                // sgl1 at SQE[24..40]: data block (type0, subtype1=offset), len=1024
                put_le32(sqe + 32, 1024);         // unkeyed.length (bytes 8..12 of sgl)
                sqe[39] = 0x01;                   // (type<<4)|subtype = (0<<4)|1
                put_le16(sqe + 40, 0);            // recfmt
                put_le16(sqe + 42, 0);            // qid
                put_le16(sqe + 44, QD - 1);       // sqsize
                // connect data at cap[64..1088)
                uint8_t* data = cap.data() + 64;
                data[16] = 0xff; data[17] = 0xff; // cntlid = 0xffff (admin)
                const char* subnqn = "nqn.2016-06.io.spdk:cnode1";
                const char* hostnqn = "nqn.2014-08.org.nvmexpress:uuid:00000000-0000-0000-0000-000000000000";
                std::memcpy(data + 256, subnqn, std::strlen(subnqn));
                std::memcpy(data + 512, hostnqn, std::strlen(hostnqn));

                ibv_mr* cap_mr = ibv_reg_mr(pd, cap.data(), cap.size(), IBV_ACCESS_LOCAL_WRITE);
                if (!cap_mr) { ERR("reg cap mr"); return 1; }
                {
                    struct ibv_sge sges[2]{};
                    sges[0].addr = (uintptr_t)cap.data(); sges[0].length = 64; sges[0].lkey = cap_mr->lkey;
                    sges[1].addr = (uintptr_t)(cap.data() + 64); sges[1].length = 1024; sges[1].lkey = cap_mr->lkey;
                    struct ibv_send_wr wr{};
                    wr.wr_id = 0;
                    wr.sg_list = sges;
                    wr.num_sge = 2;
                    wr.opcode = IBV_WR_SEND;
                    wr.send_flags = IBV_SEND_SIGNALED;
                    struct ibv_send_wr* bad = nullptr;
                    if (ibv_post_send(qp, &wr, &bad)) { ERRNO("post_send connect"); return 1; }
                }
                LOG("sent Fabric Connect");

                uint16_t cntlid = 0;
                bool got_rsp = false;
                for (int i = 0; i < 2000 && !got_rsp; i++) {
                    struct ibv_wc wc{};
                    int n = ibv_poll_cq(cq, 1, &wc);
                    if (n <= 0) { usleep(1000); continue; }
                    LOG("WC: opcode=%d status=%d wr_id=%llu", wc.opcode, wc.status, (unsigned long long)wc.wr_id);
                    if (wc.status != IBV_WC_SUCCESS) { ERR("WC status %d", wc.status); return 1; }
                    if (wc.opcode == IBV_WC_RECV) {
                        cntlid = rsp_buf[0] | (rsp_buf[1] << 8);
                        uint16_t status = rsp_buf[14] | (rsp_buf[15] << 8);
                        LOG("Connect rsp: cntlid=%u status=0x%04x", cntlid, status);
                        got_rsp = true;
                    }
                }
                if (!got_rsp) { ERR("no connect response"); return 1; }

                LOG("M2 PASS: Fabric Connect done, cntlid=%u", cntlid);

                // ---- M2b: Identify Controller + Namespace ----
                const size_t ID_DATA_LEN = 4096;
                std::vector<uint8_t> id_data(ID_DATA_LEN, 0);
                ibv_mr* id_mr = ibv_reg_mr(pd, id_data.data(), ID_DATA_LEN,
                                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                                           IBV_ACCESS_REMOTE_READ);
                if (!id_mr) { ERR("reg identify mr"); return 1; }

                auto post_recv = [&](uint64_t wr_id) {
                    struct ibv_sge sge{};
                    sge.addr = (uintptr_t)rsp_buf.data(); sge.length = rsp_buf.size(); sge.lkey = rsp_mr->lkey;
                    struct ibv_recv_wr wr{};
                    wr.wr_id = wr_id; wr.sg_list = &sge; wr.num_sge = 1;
                    struct ibv_recv_wr* bad = nullptr;
                    return ibv_post_recv(qp, &wr, &bad) == 0;
                };

                auto do_identify = [&](uint16_t cid, uint32_t cns, uint32_t nsid) -> bool {
                    uint8_t sqe[64] = {0};
                    sqe[0] = 0x06;                    // opcode = Identify
                    sqe[1] = 0x40;                    // psdt = SGL
                    put_le16(sqe + 2, cid);
                    put_le32(sqe + 4, nsid);
                    // Keyed SGL at offset 24 (addr + rkey + len)
                    put_le64(sqe + 24, (uint64_t)id_data.data());
                    put_le24(sqe + 32, ID_DATA_LEN);
                    put_le32(sqe + 35, id_mr->rkey);
                    sqe[39] = 0x40;                   // (type 0x4 << 4) | subtype 0x0
                    put_le32(sqe + 40, cns);          // cdw10 = CNS

                    ibv_mr* sqe_mr = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
                    if (!sqe_mr) { ERR("reg sqe mr"); return false; }
                    std::memset(rsp_buf.data(), 0, rsp_buf.size());
                    if (!post_recv(1)) { ERR("post_recv"); return false; }
                    struct ibv_sge sge{};
                    sge.addr = (uintptr_t)sqe; sge.length = 64; sge.lkey = sqe_mr->lkey;
                    struct ibv_send_wr wr{};
                    wr.wr_id = 0; wr.sg_list = &sge; wr.num_sge = 1;
                    wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
                    struct ibv_send_wr* bad = nullptr;
                    if (ibv_post_send(qp, &wr, &bad)) { ERRNO("post_send identify"); ibv_dereg_mr(sqe_mr); return false; }
                    bool ok = false;
                    for (int i = 0; i < 2000; i++) {
                        struct ibv_wc wc{};
                        int n = ibv_poll_cq(cq, 1, &wc);
                        if (n <= 0) { usleep(1000); continue; }
                        if (wc.status != IBV_WC_SUCCESS) { ERR("WC status %d", wc.status); break; }
                        if (wc.opcode == IBV_WC_RECV) {
                            uint16_t status = rsp_buf[14] | (rsp_buf[15] << 8);
                            LOG("  identify cid=%u response status=0x%04x (sc=0x%x sct=0x%x)",
                                cid, status, status & 0xff, (status >> 8) & 0x7);
                            ok = (status == 0);
                            break;
                        }
                    }
                    ibv_dereg_mr(sqe_mr);
                    return ok;
                };

                // Property Set CC.EN=1 to enable the controller
                {
                    uint8_t ps[64] = {0};
                    ps[0] = 0x7f; ps[1] = 0x00; put_le16(ps + 2, 0); ps[4] = 0x00; // fctype = PROPERTY_SET
                    put_le32(ps + 44, 0x14);          // ofst = CC register (0x14)
                    put_le32(ps + 48, 0x00460001);    // value = EN=1, IOSQES=6, IOCQES=4
                    ibv_mr* ps_mr = ibv_reg_mr(pd, ps, 64, IBV_ACCESS_LOCAL_WRITE);
                    std::memset(rsp_buf.data(), 0, rsp_buf.size());
                    if (ps_mr && post_recv(1)) {
                        struct ibv_sge sge{};
                        sge.addr = (uintptr_t)ps; sge.length = 64; sge.lkey = ps_mr->lkey;
                        struct ibv_send_wr wr{};
                        wr.wr_id = 0; wr.sg_list = &sge; wr.num_sge = 1;
                        wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
                        struct ibv_send_wr* bad = nullptr;
                        if (ibv_post_send(qp, &wr, &bad)) { ERRNO("post_send prop set"); }
                        for (int i = 0; i < 2000; i++) {
                            struct ibv_wc wc{};
                            int n = ibv_poll_cq(cq, 1, &wc);
                            if (n <= 0) { usleep(1000); continue; }
                            if (wc.opcode == IBV_WC_RECV) {
                                uint16_t st = rsp_buf[14] | (rsp_buf[15] << 8);
                                LOG("Property Set CC: status=0x%04x", st);
                                break;
                            }
                        }
                    }
                    if (ps_mr) ibv_dereg_mr(ps_mr);
                    usleep(100000);  // wait for controller ready
                }

                if (do_identify(1, 0x01, 0)) {
                    char sn[21] = {0}, mn[41] = {0};
                    std::memcpy(sn, id_data.data() + 4, 20);
                    std::memcpy(mn, id_data.data() + 24, 40);
                    LOG("Identify Controller: model='%s' serial='%s'", mn, sn);
                } else { ERR("Identify Controller failed"); }

                if (do_identify(2, 0x00, 1)) {
                    uint64_t nsze = 0;
                    std::memcpy(&nsze, id_data.data(), 8);
                    LOG("Identify Namespace 1: nsze=%llu blocks", (unsigned long long)nsze);
                } else { ERR("Identify Namespace failed"); }

                ibv_dereg_mr(id_mr);

                // send DREQ before teardown (reopen CM channel)
                {
                    CmChannel dch;
                    if (dch.open(IFNAME, "192.168.255.20", DST_IP, 4791)) {
                        std::vector<uint8_t> dreq(MAD_BODY_LEN, 0);
                        put_be32(dreq.data() + rtu_off::LOCAL_COMM_ID, (uint32_t)local_comm_id);
                        put_be32(dreq.data() + rtu_off::REMOTE_COMM_ID, remote_comm_id);
                        auto dpkt = build_cm_packet(CM_DREQ_ATTR, tid, dreq);
                        dch.send(dpkt);
                        dch.close();
                    }
                }
                LOG("DREQ sent");
                // QP -> ERR, drain, then destroy
                ib_mark_qp_error(qp);
                {
                    struct ibv_wc wc{};
                    while (ibv_poll_cq(cq, 16, &wc) > 0) {}
                }
                ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dealloc_pd(pd);
                ibv_close_device(ctx);
                return 0;
            }
        }
    }
    ERR("no REP received");
    ch.close();
    return 1;
}