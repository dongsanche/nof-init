// Multiple I/O queues test: Set/Get Features (Number of Queues), establish N
// I/O queues, do concurrent I/O across them, and verify data integrity.
//
// Usage: sudo ./multi_queue_test  (target must be running kernel nvmet)
#include "initiator.hpp"

#include <cstring>
#include <vector>
#include <unistd.h>

using namespace nof;

static const uint32_t LBA_BYTES = 4096;
static const uint32_t NSID      = 1;
static const int NQ             = 4;   // number of I/O queues to create

static int g_fail = 0;
#define REQUIRE(cond, msg) do { if (!(cond)) { g_fail++; ERR("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); } } while (0)

// single-shot I/O write/read of one block on `io`.
static bool io_one(Conn& io, ibv_pd* pd, uint8_t opcode, uint32_t lba, uint8_t* data, ibv_mr* mr) {
    uint8_t sqe[64] = {0};
    nvme::build_rw(sqe, opcode, 0, NSID, lba, 0, (uint64_t)data, mr->rkey, LBA_BYTES);
    ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    if (!sm) return false;
    memset(io.rsp.data(), 0, io.rsp.size());
    ibv_sge rs{ (uintptr_t)io.rsp.data(), (uint32_t)io.rsp.size(), io.rsp_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id = 1; rwr.sg_list = &rs; rwr.num_sge = 1;
    ibv_recv_wr* rbad = nullptr;
    if (ibv_post_recv(io.qp, &rwr, &rbad)) { ibv_dereg_mr(sm); return false; }
    ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
    ibv_send_wr wr{}; wr.wr_id = 0; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(io.qp, &wr, &bad)) { ibv_dereg_mr(sm); return false; }
    bool ok = false;
    for (int i = 0; i < 3000; i++) {
        ibv_wc wc{}; int n = ibv_poll_cq(io.cq, 1, &wc);
        if (n <= 0) { usleep(1000); continue; }
        if (wc.status != IBV_WC_SUCCESS) break;
        if (wc.opcode == IBV_WC_RECV) { uint16_t st = io.rsp[14] | (io.rsp[15] << 8); ok = (st == 0); break; }
    }
    ibv_dereg_mr(sm);
    return ok;
}

// Post a write without waiting (for concurrency). Caller polls the CQ later.
static bool post_write(Conn& io, ibv_pd* pd, uint32_t lba, uint8_t* data, ibv_mr* mr, ibv_mr*& sm_out) {
    uint8_t* sqe = new uint8_t[64];
    std::memset(sqe, 0, 64);
    nvme::build_rw(sqe, nvme::OPC_WRITE, 0, NSID, lba, 0, (uint64_t)data, mr->rkey, LBA_BYTES);
    sm_out = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    if (!sm_out) { delete[] sqe; return false; }
    memset(io.rsp.data(), 0, io.rsp.size());
    ibv_sge rs{ (uintptr_t)io.rsp.data(), (uint32_t)io.rsp.size(), io.rsp_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id = 1; rwr.sg_list = &rs; rwr.num_sge = 1;
    ibv_recv_wr* rbad = nullptr;
    if (ibv_post_recv(io.qp, &rwr, &rbad)) { ibv_dereg_mr(sm_out); delete[] sqe; return false; }
    ibv_sge s{ (uintptr_t)sqe, 64, sm_out->lkey };
    ibv_send_wr wr{}; wr.wr_id = 0; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(io.qp, &wr, &bad)) { ibv_dereg_mr(sm_out); delete[] sqe; return false; }
    return true;
}

int main() {
    ibv_context* ctx = ib_open_device(nullptr);
    if (!ctx) return 1;
    ibv_device_attr dev_attr{}; ibv_query_device(ctx, &dev_attr);
    ibv_gid local_gid{}; int sgid_idx = 0;
    if (ib_pick_gid(ctx, 1, &local_gid, &sgid_idx)) return 1;
    ibv_gid remote_gid = ip_to_gid(DST_IP);
    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) return 1;

    // ---- admin + enable ----
    Conn admin;
    if (!establish_conn(ctx, pd, 0, 0, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin)) { REQUIRE(false, "admin CM"); return 1; }
    uint16_t cntlid = 0;
    if (!fabric_connect(admin, pd, 0, 0xffff, QD, cntlid)) { REQUIRE(false, "admin connect"); return 1; }
    if (!enable_controller(admin, pd)) { REQUIRE(false, "CC.EN"); return 1; }

    // ---- Set/Get Features (Number of Queues) ----
    uint16_t allocated = 0;
    if (!set_num_queues(admin, pd, NQ, NQ, allocated)) { REQUIRE(false, "set num queues"); }
    LOG("Set Features NumQueues: requested %d, allocated %d", NQ, allocated);
    REQUIRE(allocated >= NQ, "allocated < requested");
    uint16_t maxq = 0;
    if (!get_num_queues(admin, pd, maxq)) { REQUIRE(false, "get num queues"); }
    LOG("Get Features NumQueues: %d submission queues supported", maxq);

    // ---- establish NQ I/O queues (qid = 1..NQ) ----
    std::vector<Conn> ios(NQ);
    for (int q = 0; q < NQ; q++) {
        if (!establish_conn(ctx, pd, q + 1, cntlid, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, ios[q])) { REQUIRE(false, "io CM"); return 1; }
        uint16_t io_cntlid = 0;
        if (!fabric_connect(ios[q], pd, q + 1, cntlid, QD, io_cntlid)) { REQUIRE(false, "io connect"); return 1; }
    }
    LOG("established %d I/O queues", NQ);

    // ---- sequential write/read per queue ----
    std::vector<uint8_t> wbuf(LBA_BYTES, 0), rbuf(LBA_BYTES, 0);
    ibv_mr* wmr = ibv_reg_mr(pd, wbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    ibv_mr* rmr = ibv_reg_mr(pd, rbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    for (int q = 0; q < NQ; q++) {
        for (uint32_t b = 0; b < LBA_BYTES; b++) wbuf[b] = (uint8_t)((q * 53 + b * 3) & 0xff);
        memset(rbuf.data(), 0, LBA_BYTES);
        if (!io_one(ios[q], pd, nvme::OPC_WRITE, q, wbuf.data(), wmr)) { REQUIRE(false, "per-queue write"); }
        if (!io_one(ios[q], pd, nvme::OPC_READ, q, rbuf.data(), rmr)) { REQUIRE(false, "per-queue read"); }
        if (std::memcmp(wbuf.data(), rbuf.data(), LBA_BYTES) != 0) { REQUIRE(false, "per-queue data mismatch"); }
    }

    // ---- concurrency: fire one write on every queue before polling any, then read back ----
    {
        // per-queue distinct buffers (no cross-queue aliasing)
        std::vector<uint8_t> cwbuf(NQ * LBA_BYTES, 0), crbuf(NQ * LBA_BYTES, 0);
        ibv_mr* cwmr = ibv_reg_mr(pd, cwbuf.data(), cwbuf.size(), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
        ibv_mr* crmr = ibv_reg_mr(pd, crbuf.data(), crbuf.size(), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        for (int q = 0; q < NQ; q++)
            for (uint32_t b = 0; b < LBA_BYTES; b++)
                cwbuf[(size_t)q * LBA_BYTES + b] = (uint8_t)((0x90 + q * 7 + b) & 0xff);

        std::vector<ibv_mr*> sm(NQ, nullptr);
        for (int q = 0; q < NQ; q++) {
            uint32_t lba = 100 + q;
            if (!post_write(ios[q], pd, lba, cwbuf.data() + (size_t)q * LBA_BYTES, cwmr, sm[q])) { REQUIRE(false, "concurrent post write"); }
        }
        for (int q = 0; q < NQ; q++) {
            bool done = false;
            for (int i = 0; i < 3000 && !done; i++) {
                ibv_wc wc{}; int n = ibv_poll_cq(ios[q].cq, 1, &wc);
                if (n <= 0) { usleep(1000); continue; }
                if (wc.status != IBV_WC_SUCCESS) break;
                if (wc.opcode == IBV_WC_RECV) { uint16_t st = ios[q].rsp[14] | (ios[q].rsp[15] << 8); REQUIRE(st == 0, "concurrent write status"); done = true; }
            }
            if (sm[q]) ibv_dereg_mr(sm[q]);
        }
        // read back each queue's LBA and verify data actually landed
        for (int q = 0; q < NQ; q++) {
            uint32_t lba = 100 + q;
            if (!io_one(ios[q], pd, nvme::OPC_READ, lba, crbuf.data() + (size_t)q * LBA_BYTES, crmr)) { REQUIRE(false, "concurrent read"); }
        }
        for (int q = 0; q < NQ; q++) {
            if (std::memcmp(crbuf.data() + (size_t)q * LBA_BYTES, cwbuf.data() + (size_t)q * LBA_BYTES, LBA_BYTES) != 0)
                REQUIRE(false, "concurrent data mismatch");
        }
        ibv_dereg_mr(cwmr); ibv_dereg_mr(crmr);
    }
    LOG("concurrent writes+reads across %d queues verified", NQ);

    for (int q = 0; q < NQ; q++) teardown_conn(ios[q]);
    teardown_conn(admin);
    ibv_dereg_mr(wmr); ibv_dereg_mr(rmr);
    ibv_dealloc_pd(pd); ibv_close_device(ctx);

    if (g_fail == 0) {
        LOG("MULTI-QUEUE PASS: %d I/O queues", NQ);
        return 0;
    }
    ERR("MULTI-QUEUE FAIL: %d failures", g_fail);
    return 1;
}