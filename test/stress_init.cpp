// Integration stress test for the NVMe-oF/RDMA initiator (needs a live target).
//
// Scenarios (in order):
//   1. pipeline   — fill the I/O queue (QD) with concurrent writes, then reads,
//                   verifying per-command completion demux by command id.
//   2. large      — one 256 KiB write + read (multi-LBA single command).
//   3. sweep      — write a pseudorandom pattern across many LBAs, read back,
//                   and compare a CRC32 of every block (data integrity).
//   4. loops      — repeat the above ITERS times to catch flakiness/resource leaks.
//
// Usage: sudo ./stress_init  (target must be running kernel nvmet or SPDK)
#include "initiator.hpp"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

using namespace nof;

static const uint32_t LBA_BYTES = 4096;    // 4 KiB namespace block size
static const uint32_t NSID      = 1;

static int g_fail = 0;
#define REQUIRE(cond, msg) do { if (!(cond)) { g_fail++; ERR("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); } } while (0)

// Submit `n` completed SQEs (bytes of each in sqes[i]) concurrently on the I/O queue.
// Each command's completion CQE lands in a 64-byte slot; wr_id==slot. Fills cqe_status[slot]
// and cqe_cid[slot]. Returns true if all n recv completions arrived.
static bool io_batch(Conn& io, ibv_pd* pd, int n,
                     const std::vector<std::array<uint8_t, 64>>& sqes,
                     std::vector<uint16_t>& cqe_status, std::vector<uint16_t>& cqe_cid,
                     std::vector<uint8_t>& slots) {
    slots.assign(n * 64, 0);
    ibv_mr* rsp_mr = ibv_reg_mr(pd, slots.data(), slots.size(), IBV_ACCESS_LOCAL_WRITE);
    if (!rsp_mr) return false;

    for (int i = 0; i < n; i++) {
        ibv_sge rs{ (uintptr_t)(slots.data() + i * 64), 64, rsp_mr->lkey };
        ibv_recv_wr rwr{}; rwr.wr_id = i; rwr.sg_list = &rs; rwr.num_sge = 1;
        ibv_recv_wr* rbad = nullptr;
        if (ibv_post_recv(io.qp, &rwr, &rbad)) { ibv_dereg_mr(rsp_mr); return false; }
    }

    std::vector<ibv_mr*> sms(n, nullptr);
    for (int i = 0; i < n; i++) {
        sms[i] = ibv_reg_mr(pd, (void*)sqes[i].data(), 64, IBV_ACCESS_LOCAL_WRITE);
        if (!sms[i]) { for (auto m : sms) if (m) ibv_dereg_mr(m); ibv_dereg_mr(rsp_mr); return false; }
        ibv_sge s{ (uintptr_t)sqes[i].data(), 64, sms[i]->lkey };
        ibv_send_wr wr{}; wr.wr_id = i; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
        ibv_send_wr* bad = nullptr;
        if (ibv_post_send(io.qp, &wr, &bad)) { for (auto m : sms) if (m) ibv_dereg_mr(m); ibv_dereg_mr(rsp_mr); return false; }
    }

    cqe_status.assign(n, 0xffff);
    cqe_cid.assign(n, 0xffff);
    int done = 0;
    for (int iter = 0; iter < 10000 && done < n; iter++) {
        ibv_wc wc{}; int k = ibv_poll_cq(io.cq, 1, &wc);
        if (k <= 0) { usleep(1000); continue; }
        if (wc.opcode == IBV_WC_RECV) {
            int slot = (int)(wc.wr_id & 0xffff);
            if (wc.status != IBV_WC_SUCCESS) { cqe_status[slot] = 0xffff; done++; continue; }
            const uint8_t* cqe = slots.data() + slot * 64;
            cqe_cid[slot] = cqe[12] | (cqe[13] << 8);
            cqe_status[slot] = cqe[14] | (cqe[15] << 8);
            done++;
        }
    }
    for (auto m : sms) ibv_dereg_mr(m);
    ibv_dereg_mr(rsp_mr);
    return done == n;
}

static uint8_t pat(uint32_t seed, uint32_t off) { return (uint8_t)((seed * 131 + off * 17) & 0xff); }

int main() {
    ibv_context* ctx = ib_open_device(nullptr);
    if (!ctx) return 1;
    ibv_device_attr dev_attr{}; ibv_query_device(ctx, &dev_attr);
    ibv_gid local_gid{}; int sgid_idx = 0;
    if (ib_pick_gid(ctx, 1, &local_gid, &sgid_idx)) return 1;
    ibv_gid remote_gid = ip_to_gid(DST_IP);
    LOG("device %s gid=%s", ibv_get_device_name(ctx->device), gid_to_string(local_gid).c_str());
    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) return 1;

    // ---- admin queue + enable ----
    Conn admin;
    if (!establish_conn(ctx, pd, 0, 0, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin)) { ERR("admin CM"); return 1; }
    uint16_t cntlid = 0;
    if (!fabric_connect(admin, pd, 0, 0xffff, QD, cntlid)) { ERR("admin connect"); return 1; }
    if (!enable_controller(admin, pd)) { ERR("CC.EN"); return 1; }
    LOG("controller enabled cntlid=%u", cntlid);

    // ---- IO queue ----
    Conn io;
    if (!establish_conn(ctx, pd, 1, cntlid, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, io)) { ERR("io CM"); return 1; }
    uint16_t io_cntlid = 0;
    if (!fabric_connect(io, pd, 1, cntlid, QD, io_cntlid)) { ERR("io connect"); return 1; }
    LOG("io queue connected cntlid=%u", io_cntlid);

    // ---- shared data buffers (write + read) registered once ----
    const uint32_t SWEEP_LBAS = 256;                 // 1 MiB sweep
    const size_t BUF_LEN = (size_t)SWEEP_LBAS * LBA_BYTES;
    std::vector<uint8_t> wbuf(BUF_LEN, 0), rbuf(BUF_LEN, 0);
    ibv_mr* wmr = ibv_reg_mr(pd, wbuf.data(), BUF_LEN, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    ibv_mr* rmr = ibv_reg_mr(pd, rbuf.data(), BUF_LEN, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    // dedicated 256 KiB buffers for the large single-command transfer
    const size_t LARGE_LEN = 256 * 1024;
    std::vector<uint8_t> lwbuf(LARGE_LEN, 0), lrbuf(LARGE_LEN, 0);
    ibv_mr* lwmr = ibv_reg_mr(pd, lwbuf.data(), LARGE_LEN, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    ibv_mr* lrmr = ibv_reg_mr(pd, lrbuf.data(), LARGE_LEN, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!wmr || !rmr || !lwmr || !lrmr) { ERR("reg mr"); return 1; }

    const int ITERS = 20;
    uint64_t total_cmds = 0;
    for (int it = 0; it < ITERS; it++) {
        // ===== 1) pipeline: fill QD with concurrent 4KiB writes/reads =====
        {
            const int n = QD;
            uint32_t base = (uint32_t)(it % 2) * QD;   // alternate LBA regions each iteration
            std::vector<std::array<uint8_t, 64>> sqes(n);
            for (int i = 0; i < n; i++) {
                uint32_t lba = base + i;
                uint32_t seed = 0x5100 + it;
                for (uint32_t b = 0; b < LBA_BYTES; b++) wbuf[(size_t)lba * LBA_BYTES + b] = pat(seed, i * LBA_BYTES + b);
                nvme::build_rw(sqes[i].data(), nvme::OPC_WRITE, (uint16_t)i, NSID, lba, 0,
                               (uint64_t)(wbuf.data() + (size_t)lba * LBA_BYTES), wmr->lkey, LBA_BYTES);
            }
            std::vector<uint16_t> st, cid; std::vector<uint8_t> slots;
            if (!io_batch(io, pd, n, sqes, st, cid, slots)) { REQUIRE(false, "pipeline write transport"); }
            for (int i = 0; i < n; i++) {
                if (st[i] != 0) { ERR("pipeline write slot %d status 0x%04x", i, st[i]); REQUIRE(false, "pipeline write status"); }
                if (cid[i] != i) REQUIRE(false, "pipeline write cid mismatch");
            }
            std::memset(rbuf.data(), 0, BUF_LEN);
            for (int i = 0; i < n; i++) {
                uint32_t lba = base + i;
                nvme::build_rw(sqes[i].data(), nvme::OPC_READ, (uint16_t)n + i, NSID, lba, 0,
                               (uint64_t)(rbuf.data() + (size_t)lba * LBA_BYTES), rmr->rkey, LBA_BYTES);
            }
            if (!io_batch(io, pd, n, sqes, st, cid, slots)) { REQUIRE(false, "pipeline read transport"); }
            for (int i = 0; i < n; i++) {
                uint32_t lba = base + i;
                if (st[i] != 0) { REQUIRE(false, "pipeline read status"); }
                if (std::memcmp(rbuf.data() + (size_t)lba * LBA_BYTES, wbuf.data() + (size_t)lba * LBA_BYTES, LBA_BYTES) != 0)
                    REQUIRE(false, "pipeline read data mismatch");
            }
            total_cmds += 2 * n;
        }

        // ===== 2) large: single 256 KiB write + read (64 LBAs) =====
        {
            const uint32_t nlb = 63;                 // 64 blocks
            const size_t L = LARGE_LEN;
            uint32_t start_lba = 512 + (uint32_t)(it % 2) * 64;   // well within the 4096-LBA namespace
            for (size_t b = 0; b < L; b++) lwbuf[b] = pat(0xbeef + it, (uint32_t)b);
            std::memset(lrbuf.data(), 0, L);
            std::array<uint8_t, 64> wsqe, rsqe;
            nvme::build_rw(wsqe.data(), nvme::OPC_WRITE, 0, NSID, start_lba, nlb, (uint64_t)lwbuf.data(), lwmr->lkey, (uint32_t)L);
            nvme::build_rw(rsqe.data(), nvme::OPC_READ, 0, NSID, start_lba, nlb, (uint64_t)lrbuf.data(), lrmr->rkey, (uint32_t)L);
            std::vector<std::array<uint8_t, 64>> sqes1{wsqe}, sqes2{rsqe};
            std::vector<uint16_t> st, cid; std::vector<uint8_t> slots;
            if (!io_batch(io, pd, 1, sqes1, st, cid, slots) || st[0] != 0) REQUIRE(false, "large write");
            if (!io_batch(io, pd, 1, sqes2, st, cid, slots) || st[0] != 0) REQUIRE(false, "large read");
            if (std::memcmp(lrbuf.data(), lwbuf.data(), L) != 0) REQUIRE(false, "large read data mismatch");
            total_cmds += 2;
        }
    }

    // ===== 3) sweep: CRC32 over all SWEEP_LBAS after a full write pass =====
    for (uint32_t lba = 0; lba < SWEEP_LBAS; lba++)
        for (uint32_t b = 0; b < LBA_BYTES; b++)
            wbuf[(size_t)lba * LBA_BYTES + b] = pat(0x1337, lba * LBA_BYTES + b);
    {
        std::vector<std::array<uint8_t, 64>> sqes(QD);
        for (uint32_t base = 0; base < SWEEP_LBAS; base += QD) {
            int n = (int)std::min<uint32_t>(QD, SWEEP_LBAS - base);
            for (int i = 0; i < n; i++) {
                uint32_t lba = base + i;
                nvme::build_rw(sqes[i].data(), nvme::OPC_WRITE, (uint16_t)(lba & 0xffff), NSID, lba, 0,
                               (uint64_t)(wbuf.data() + (size_t)lba * LBA_BYTES), wmr->lkey, LBA_BYTES);
            }
            std::vector<uint16_t> st, cid; std::vector<uint8_t> slots;
            if (!io_batch(io, pd, n, sqes, st, cid, slots)) REQUIRE(false, "sweep write transport");
            for (int i = 0; i < n; i++) if (st[i] != 0) { ERR("sweep w lba %u status 0x%04x", base + i, st[i]); REQUIRE(false, "sweep write status"); }
            total_cmds += n;
        }
    }
    std::memset(rbuf.data(), 0, BUF_LEN);
    {
        std::vector<std::array<uint8_t, 64>> sqes(QD);
        for (uint32_t base = 0; base < SWEEP_LBAS; base += QD) {
            int n = (int)std::min<uint32_t>(QD, SWEEP_LBAS - base);
            for (int i = 0; i < n; i++) {
                uint32_t lba = base + i;
                nvme::build_rw(sqes[i].data(), nvme::OPC_READ, (uint16_t)((lba + 1) & 0xffff), NSID, lba, 0,
                               (uint64_t)(rbuf.data() + (size_t)lba * LBA_BYTES), rmr->rkey, LBA_BYTES);
            }
            std::vector<uint16_t> st, cid; std::vector<uint8_t> slots;
            if (!io_batch(io, pd, n, sqes, st, cid, slots)) REQUIRE(false, "sweep read transport");
            for (int i = 0; i < n; i++) if (st[i] != 0) REQUIRE(false, "sweep read status");
            total_cmds += n;
        }
    }
    uint32_t crc_w = crc32_raw(wbuf.data(), BUF_LEN);
    uint32_t crc_r = crc32_raw(rbuf.data(), BUF_LEN);
    LOG("sweep CRC write=0x%08x read=0x%08x", crc_w, crc_r);
    if (crc_w != crc_r) REQUIRE(false, "sweep CRC mismatch");

    ibv_dereg_mr(wmr); ibv_dereg_mr(rmr);
    ibv_dereg_mr(lwmr); ibv_dereg_mr(lrmr);
    teardown_conn(io);
    teardown_conn(admin);
    ibv_dealloc_pd(pd); ibv_close_device(ctx);

    if (g_fail == 0) {
        LOG("STRESS PASS: %u commands over %d iterations, QD=%u", (unsigned)total_cmds, ITERS, (unsigned)QD);
        return 0;
    }
    ERR("STRESS FAIL: %d failures", g_fail);
    return 1;
}