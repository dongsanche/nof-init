// Auto-reconnect after target outage test.
//   Phase 1: connect (admin + IO), verify I/O, teardown.
//   Phase 2: sleep (operator kills + restarts the target).
//   Phase 3: reconnect with retry, verify I/O again.
//
// Orchestration: run this with the target up; kill+restart nvmet on the target
// during Phase 2. Usage: sudo ./reconnect_test
#include "initiator.hpp"

#include <cstring>
#include <vector>
#include <unistd.h>
#include <chrono>

using namespace nof;

static const uint32_t LBA_BYTES = 4096;
static const uint32_t NSID      = 1;
static int g_fail = 0;
#define REQUIRE(cond, msg) do { if (!(cond)) { g_fail++; ERR("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); } } while (0)

// Full bring-up: admin connect + CC.EN + IO connect + one write/read round-trip.
static bool bring_up_and_io(ibv_context* ctx, ibv_pd* pd, ibv_gid lgid, int sgid_idx,
                            ibv_gid rgid, uint64_t guid, Conn& admin, Conn& io) {
    if (!establish_conn(ctx, pd, 0, 0, QD, lgid, sgid_idx, rgid, guid, admin, 3000)) return false;
    uint16_t cntlid = 0;
    if (!fabric_connect(admin, pd, 0, 0xffff, QD, cntlid)) { teardown_conn(admin); return false; }
    if (!enable_controller(admin, pd)) { teardown_conn(admin); return false; }
    if (!establish_conn(ctx, pd, 1, cntlid, QD, lgid, sgid_idx, rgid, guid, io, 3000)) { teardown_conn(admin); return false; }
    uint16_t io_cntlid = 0;
    if (!fabric_connect(io, pd, 1, cntlid, QD, io_cntlid)) { teardown_conn(io); teardown_conn(admin); return false; }

    std::vector<uint8_t> wbuf(LBA_BYTES, 0), rbuf(LBA_BYTES, 0);
    for (uint32_t b = 0; b < LBA_BYTES; b++) wbuf[b] = (uint8_t)((b * 3 + 1) & 0xff);
    ibv_mr* wmr = ibv_reg_mr(pd, wbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    ibv_mr* rmr = ibv_reg_mr(pd, rbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    auto one = [&](uint8_t op, uint8_t* data, ibv_mr* mr) -> bool {
        uint8_t sqe[64] = {0};
        nvme::build_rw(sqe, op, 0, NSID, 0, 0, (uint64_t)data, mr->rkey, LBA_BYTES);
        memset(io.rsp.data(), 0, io.rsp.size());
        ibv_sge rs{ (uintptr_t)io.rsp.data(), (uint32_t)io.rsp.size(), io.rsp_mr->lkey };
        ibv_recv_wr rwr{}; rwr.wr_id = 1; rwr.sg_list = &rs; rwr.num_sge = 1;
        ibv_recv_wr* rbad = nullptr;
        ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
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
    };
    bool ok = one(nvme::OPC_WRITE, wbuf.data(), wmr) && one(nvme::OPC_READ, rbuf.data(), rmr) &&
              std::memcmp(wbuf.data(), rbuf.data(), LBA_BYTES) == 0;
    ibv_dereg_mr(wmr); ibv_dereg_mr(rmr);
    return ok;
}

int main() {
    auto t0 = std::chrono::steady_clock::now();
    auto el = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    ibv_context* ctx = ib_open_device(nullptr);
    if (!ctx) return 1;
    ibv_device_attr dev_attr{}; ibv_query_device(ctx, &dev_attr);
    ibv_gid local_gid{}; int sgid_idx = 0;
    if (ib_pick_gid(ctx, 1, &local_gid, &sgid_idx)) return 1;
    ibv_gid remote_gid = ip_to_gid(DST_IP);
    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) return 1;
    LOG("t=%.3fs start", el());

    // ---- Phase 1: initial connect + I/O ----
    {
        Conn admin, io;
        if (!bring_up_and_io(ctx, pd, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin, io)) { REQUIRE(false, "phase1 bring-up"); }
        LOG("t=%.3fs PHASE1_OK: initial connect + I/O verified", el());
        teardown_conn(io);
        teardown_conn(admin);
    }

    // ---- Phase 2: operator kills + restarts the target ----
    LOG("t=%.3fs PHASE2: sleeping 6s (operator kills+restarts nvmet now)...", el());
    usleep(6 * 1000 * 1000);

    // ---- Phase 3: reconnect with retry ----
    LOG("t=%.3fs PHASE3: reconnecting with retry (up to 30s)...", el());
    {
        Conn admin;
        uint16_t cntlid = 0;
        if (!connect_admin_with_retry(ctx, pd, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin, cntlid, 60, 500)) { REQUIRE(false, "phase3 reconnect"); }
        LOG("t=%.3fs reconnected: cntlid=%u", el(), cntlid);
        // verify I/O on a fresh IO queue against the reconnected controller
        Conn io;
        if (!establish_conn(ctx, pd, 1, cntlid, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, io, 3000)) { REQUIRE(false, "phase3 io CM"); }
        uint16_t io_cntlid = 0;
        if (!fabric_connect(io, pd, 1, cntlid, QD, io_cntlid)) { REQUIRE(false, "phase3 io connect"); }
        std::vector<uint8_t> wbuf(LBA_BYTES, 0), rbuf(LBA_BYTES, 0);
        for (uint32_t b = 0; b < LBA_BYTES; b++) wbuf[b] = (uint8_t)((b * 5 + 7) & 0xff);
        ibv_mr* wmr = ibv_reg_mr(pd, wbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
        ibv_mr* rmr = ibv_reg_mr(pd, rbuf.data(), LBA_BYTES, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        // (reuse a simple write/read inline)
        auto one = [&](uint8_t op, uint8_t* d, ibv_mr* mr) {
            uint8_t sqe[64] = {0}; nvme::build_rw(sqe, op, 0, NSID, 0, 0, (uint64_t)d, mr->rkey, LBA_BYTES);
            ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
            memset(io.rsp.data(), 0, io.rsp.size());
            ibv_sge rs{ (uintptr_t)io.rsp.data(), (uint32_t)io.rsp.size(), io.rsp_mr->lkey };
            ibv_recv_wr rwr{}; rwr.wr_id = 1; rwr.sg_list = &rs; rwr.num_sge = 1;
            ibv_recv_wr* rb = nullptr; ibv_post_recv(io.qp, &rwr, &rb);
            ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
            ibv_send_wr wr{}; wr.wr_id = 0; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
            ibv_send_wr* bd = nullptr; ibv_post_send(io.qp, &wr, &bd);
            bool ok = false;
            for (int i = 0; i < 3000; i++) { ibv_wc wc{}; int n = ibv_poll_cq(io.cq, 1, &wc); if (n <= 0) { usleep(1000); continue; } if (wc.status != IBV_WC_SUCCESS) break; if (wc.opcode == IBV_WC_RECV) { uint16_t st = io.rsp[14] | (io.rsp[15] << 8); ok = (st == 0); break; } }
            ibv_dereg_mr(sm); return ok;
        };
        bool ok = one(nvme::OPC_WRITE, wbuf.data(), wmr) && one(nvme::OPC_READ, rbuf.data(), rmr) && std::memcmp(wbuf.data(), rbuf.data(), LBA_BYTES) == 0;
        ibv_dereg_mr(wmr); ibv_dereg_mr(rmr);
        REQUIRE(ok, "phase3 post-reconnect I/O");
        teardown_conn(io);
        teardown_conn(admin);
    }

    ibv_dealloc_pd(pd); ibv_close_device(ctx);

    if (g_fail == 0) { LOG("t=%.3fs RECONNECT PASS", el()); return 0; }
    ERR("RECONNECT FAIL: %d", g_fail);
    return 1;
}