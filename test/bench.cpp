// Throughput benchmark: compare single I/O queue vs multiple I/O queues.
// Posts all queues' commands before waiting on any (true overlap), and keeps the
// total in-flight command count constant (depth * nqueues) for a fair comparison.
//
// Usage: sudo ./bench   (target must be running kernel nvmet)
#include "initiator.hpp"

#include <cstring>
#include <vector>
#include <unistd.h>
#include <chrono>

using namespace nof;
using Clock = std::chrono::steady_clock;

static const uint32_t NSID = 1;
static const int MAXQ = 4;

static int g_fail = 0;
#define REQUIRE(cond, msg) do { if (!(cond)) { g_fail++; ERR("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); } } while (0)

struct QBench {
    Conn* io;
    int depth;
    uint32_t block;
    std::vector<uint8_t> rsp;                 // depth*64
    std::vector<std::vector<uint8_t>> sqe;    // depth × 64 (separate MR each)
    std::vector<ibv_mr*> sqe_mr;
    std::vector<uint8_t> data;                // depth*block
    ibv_mr *rsp_mr, *data_mr;
};

static bool qbench_init(QBench& b, Conn* io, ibv_pd* pd, int depth, uint32_t block) {
    b.io = io; b.depth = depth; b.block = block;
    b.rsp.assign((size_t)depth * 64, 0);
    b.sqe.resize(depth);
    b.sqe_mr.resize(depth, nullptr);
    b.data.assign((size_t)depth * block, 0);
    b.rsp_mr = ibv_reg_mr(pd, b.rsp.data(), b.rsp.size(), IBV_ACCESS_LOCAL_WRITE);
    b.data_mr = ibv_reg_mr(pd, b.data.data(), b.data.size(), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!b.rsp_mr || !b.data_mr) return false;
    for (int i = 0; i < depth; i++) {
        b.sqe[i].assign(64, 0);
        b.sqe_mr[i] = ibv_reg_mr(pd, b.sqe[i].data(), 64, IBV_ACCESS_LOCAL_WRITE);
        if (!b.sqe_mr[i]) return false;
    }
    return true;
}
static void qbench_free(QBench& b) {
    for (auto m : b.sqe_mr) if (m) ibv_dereg_mr(m);
    ibv_dereg_mr(b.rsp_mr);
    ibv_dereg_mr(b.data_mr);
}

// post `depth` reads on one queue (all recv first, then all send)
static bool qbench_post(QBench& b) {
    uint16_t nlb = (uint16_t)(b.block / 4096 - 1);
    for (int i = 0; i < b.depth; i++) {
        nvme::build_rw(b.sqe[i].data(), nvme::OPC_READ, (uint16_t)i, NSID, 0, nlb,
                       (uint64_t)(b.data.data() + (size_t)i * b.block), b.data_mr->rkey, b.block);
        ibv_sge rs{ (uintptr_t)(b.rsp.data() + (size_t)i * 64), 64, b.rsp_mr->lkey };
        ibv_recv_wr rwr{}; rwr.wr_id = i; rwr.sg_list = &rs; rwr.num_sge = 1;
        ibv_recv_wr* rbad = nullptr;
        if (ibv_post_recv(b.io->qp, &rwr, &rbad)) return false;
    }
    for (int i = 0; i < b.depth; i++) {
        ibv_sge s{ (uintptr_t)b.sqe[i].data(), 64, b.sqe_mr[i]->lkey };
        ibv_send_wr wr{}; wr.wr_id = i; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
        ibv_send_wr* bad = nullptr;
        if (ibv_post_send(b.io->qp, &wr, &bad)) return false;
    }
    return true;
}
static bool qbench_wait(QBench& b) {
    int done = 0;
    for (int iter = 0; iter < 100000 && done < b.depth; iter++) {
        ibv_wc wc{}; int n = ibv_poll_cq(b.io->cq, 1, &wc);
        if (n <= 0) { usleep(1000); continue; }
        if (wc.opcode == IBV_WC_RECV) { if (wc.status != IBV_WC_SUCCESS) return false; done++; }
    }
    return done == b.depth;
}

// run `total` reads across qs (each depth `depth`), posting all queues before waiting.
static double run_bench(ibv_pd* pd, std::vector<Conn*>& qs, int depth, uint64_t total, uint32_t block) {
    int nq = (int)qs.size();
    uint64_t per_round = (uint64_t)nq * depth;
    std::vector<QBench> b(nq);
    for (int q = 0; q < nq; q++) if (!qbench_init(b[q], qs[q], pd, depth, block)) return -1;
    uint64_t done = 0;
    auto t0 = Clock::now();
    while (done < total) {
        for (int q = 0; q < nq; q++) if (!qbench_post(b[q])) { for (auto& x : b) qbench_free(x); return -1; }
        for (int q = 0; q < nq; q++) if (!qbench_wait(b[q])) { for (auto& x : b) qbench_free(x); return -1; }
        done += per_round;
    }
    double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    for (auto& x : b) qbench_free(x);
    return sec;
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

    Conn admin;
    if (!establish_conn(ctx, pd, 0, 0, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin)) { REQUIRE(false, "admin CM"); return 1; }
    uint16_t cntlid = 0;
    if (!fabric_connect(admin, pd, 0, 0xffff, QD, cntlid)) { REQUIRE(false, "admin connect"); return 1; }
    if (!enable_controller(admin, pd)) { REQUIRE(false, "CC.EN"); return 1; }
    std::vector<Conn> ios(MAXQ);
    for (int q = 0; q < MAXQ; q++) {
        if (!establish_conn(ctx, pd, q + 1, cntlid, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, ios[q])) { REQUIRE(false, "io CM"); return 1; }
        uint16_t io_cntlid = 0;
        if (!fabric_connect(ios[q], pd, q + 1, cntlid, QD, io_cntlid)) { REQUIRE(false, "io connect"); return 1; }
    }
    LOG("established %d I/O queues", MAXQ);

    for (int rep = 0; rep < 3; rep++) {
        {
            const uint32_t block = 4096;
            const uint64_t total = 16384;
            std::vector<Conn*> q1{ &ios[0] };
            std::vector<Conn*> q4{ &ios[0], &ios[1], &ios[2], &ios[3] };
            double t1 = run_bench(pd, q1, 32, total, block);
            double t4 = run_bench(pd, q4, 8, total, block);
            LOG("[4K read] 1q: %7.0f IOPS (%6.1f MB/s)   4q: %7.0f IOPS (%6.1f MB/s)   speedup %.2fx",
                total / t1, total * block / t1 / 1e6, total / t4, total * block / t4 / 1e6, t1 / t4);
        }
        {
            const uint32_t block = 64 * 1024;
            const uint64_t total = 2048;
            std::vector<Conn*> q1{ &ios[0] };
            std::vector<Conn*> q4{ &ios[0], &ios[1], &ios[2], &ios[3] };
            double t1 = run_bench(pd, q1, 8, total, block);
            double t4 = run_bench(pd, q4, 2, total, block);
            LOG("[64K read] 1q: %6.1f MB/s         4q: %6.1f MB/s          speedup %.2fx",
                total * block / t1 / 1e6, total * block / t4 / 1e6, t1 / t4);
        }
    }

    for (int q = 0; q < MAXQ; q++) teardown_conn(ios[q]);
    teardown_conn(admin);
    ibv_dealloc_pd(pd); ibv_close_device(ctx);

    if (g_fail == 0) { LOG("BENCH DONE"); return 0; }
    ERR("BENCH FAIL: %d", g_fail);
    return 1;
}