// Reconnect lifecycle stress test: repeatedly connect -> I/O -> teardown -> reconnect.
// Exercises the graceful teardown path (CM DREQ/DREP + QP -> ERR) and verifies the
// target cleans up the controller so the next connect succeeds without a reset.
//
// Usage: sudo ./lifecycle_test   (target must be running kernel nvmet or SPDK)
#include "initiator.hpp"

#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <vector>

using namespace nof;

static const uint32_t LBA_BYTES = 4096;
static const uint32_t NSID      = 1;
static const int CYCLES         = 10;

static int g_fail = 0;
#define REQUIRE(cond, msg) do { if (!(cond)) { g_fail++; ERR("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); } } while (0)

// single-shot I/O write/read of one 4 KiB block on `io`, using its scratch rsp.
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

int main() {
    ibv_context* ctx = ib_open_device(nullptr);
    if (!ctx) return 1;
    ibv_device_attr dev_attr{}; ibv_query_device(ctx, &dev_attr);
    ibv_gid local_gid{}; int sgid_idx = 0;
    if (ib_pick_gid(ctx, 1, &local_gid, &sgid_idx)) return 1;
    ibv_gid remote_gid = ip_to_gid(DST_IP);
    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) return 1;

    // data buffers registered once (PD persists across cycles)
    std::vector<uint8_t> wbuf(LBA_BYTES, 0), rbuf(LBA_BYTES, 0);
    ibv_mr* wmr = ibv_reg_mr(pd, wbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    ibv_mr* rmr = ibv_reg_mr(pd, rbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!wmr || !rmr) { ERR("reg mr"); return 1; }

    for (int cycle = 0; cycle < CYCLES; cycle++) {
        LOG("=== cycle %d ===", cycle);

        Conn admin;
        if (!establish_conn(ctx, pd, 0, 0, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin)) { REQUIRE(false, "admin CM"); break; }
        uint16_t cntlid = 0;
        if (!fabric_connect(admin, pd, 0, 0xffff, QD, cntlid)) { REQUIRE(false, "admin connect"); teardown_conn(admin); break; }
        if (!enable_controller(admin, pd)) { REQUIRE(false, "CC.EN"); teardown_conn(admin); break; }

        Conn io;
        if (!establish_conn(ctx, pd, 1, cntlid, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, io)) { REQUIRE(false, "io CM"); teardown_conn(admin); break; }
        uint16_t io_cntlid = 0;
        if (!fabric_connect(io, pd, 1, cntlid, QD, io_cntlid)) { REQUIRE(false, "io connect"); teardown_conn(io); teardown_conn(admin); break; }

        // one write + one read, verify data round-trips
        uint32_t lba = (uint32_t)(cycle % 256);
        for (uint32_t b = 0; b < LBA_BYTES; b++) wbuf[b] = (uint8_t)((cycle * 37 + b * 7) & 0xff);
        memset(rbuf.data(), 0, LBA_BYTES);
        if (!io_one(io, pd, nvme::OPC_WRITE, lba, wbuf.data(), wmr)) { REQUIRE(false, "cycle write"); }
        if (!io_one(io, pd, nvme::OPC_READ, lba, rbuf.data(), rmr)) { REQUIRE(false, "cycle read"); }
        if (std::memcmp(wbuf.data(), rbuf.data(), LBA_BYTES) != 0) REQUIRE(false, "cycle data mismatch");

        // graceful teardown (DREQ/DREP + QP ERR), then reconnect next cycle
        teardown_conn(io);
        teardown_conn(admin);
        LOG("cycle %d ok (cntlid=%u, io_cntlid=%u)", cycle, cntlid, io_cntlid);
        usleep(200000);
    }

    ibv_dereg_mr(wmr); ibv_dereg_mr(rmr);
    ibv_dealloc_pd(pd); ibv_close_device(ctx);

    if (g_fail == 0) {
        LOG("LIFECYCLE PASS: %d connect/teardown cycles", CYCLES);
        return 0;
    }
    ERR("LIFECYCLE FAIL: %d failures", g_fail);
    return 1;
}