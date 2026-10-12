// Keep Alive + Async Event Request (AER) test for the NVMe-oF/RDMA initiator.
//
// Positive  — negotiate KATO, idle longer than KATO while sending keep-alives,
//             prove the controller stays alive.
// AER       — post an Async Event Request, prove it is accepted (parked, no
//             immediate completion), then tear down.
// Negative  — negotiate a short KATO, idle past it WITHOUT keep-alives, prove the
//             target fatal-errors the controller.
//
// Usage: sudo ./keepalive_test  (target must be running kernel nvmet)
#include "initiator.hpp"

#include <cstring>
#include <vector>
#include <unistd.h>

using namespace nof;

static int g_fail = 0;
#define REQUIRE(cond, msg) do { if (!(cond)) { g_fail++; ERR("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); } } while (0)

// Establish a fresh admin controller + CC.EN with a specific KATO (ms).
static bool bring_up(ibv_context* ctx, ibv_pd* pd, ibv_gid lgid, int sgid_idx,
                     ibv_gid rgid, uint64_t guid, uint32_t kato_ms, Conn& admin) {
    if (!establish_conn(ctx, pd, 0, 0, QD, lgid, sgid_idx, rgid, guid, admin)) return false;
    uint16_t cntlid = 0;
    if (!fabric_connect(admin, pd, 0, 0xffff, QD, cntlid, kato_ms)) return false;
    if (!enable_controller(admin, pd)) return false;
    return true;
}

// Post an AER and leave it pending. Returns false on transport error only.
// The response lands in rsp_aer (dedicated buffer) when an async event fires.
static bool post_aer(Conn& admin, ibv_pd* pd, uint16_t cid,
                     std::vector<uint8_t>& rsp_aer, ibv_mr*& aer_mr, ibv_mr*& sq_mr) {
    rsp_aer.assign(64, 0);
    aer_mr = ibv_reg_mr(pd, rsp_aer.data(), rsp_aer.size(), IBV_ACCESS_LOCAL_WRITE);
    if (!aer_mr) return false;
    ibv_sge rs{ (uintptr_t)rsp_aer.data(), (uint32_t)rsp_aer.size(), aer_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id = 0xA000 | cid; rwr.sg_list = &rs; rwr.num_sge = 1;
    ibv_recv_wr* rbad = nullptr;
    if (ibv_post_recv(admin.qp, &rwr, &rbad)) return false;

    uint8_t sqe[64] = {0};
    nvme::build_async_event(sqe, cid);
    sq_mr = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    if (!sq_mr) return false;
    ibv_sge s{ (uintptr_t)sqe, 64, sq_mr->lkey };
    ibv_send_wr wr{}; wr.wr_id = cid; wr.sg_list = &s; wr.num_sge = 1; wr.opcode = IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(admin.qp, &wr, &bad)) return false;
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

    // ===== Part 1: positive keep-alive =====
    {
        LOG("=== positive keep-alive (KATO=2000ms) ===");
        Conn admin;
        if (!bring_up(ctx, pd, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, 2000, admin)) { REQUIRE(false, "positive bring-up"); }
        int ok = 0;
        for (int t = 0; t < 12; t++) {          // 6 s idle total, 3x KATO
            usleep(500000);
            if (send_keep_alive(admin, pd, (uint16_t)t)) ok++;
            else ERR("keep-alive #%d failed", t);
        }
        REQUIRE(ok == 12, "positive keep-alive: some failed");
        LOG("positive keep-alive: %d/12 ok over 6 s (alive past 2 s KATO)", ok);
        teardown_conn(admin);
    }

    // ===== Part 2: AER posting (parked, accepted) =====
    {
        LOG("=== AER post ===");
        Conn admin;
        if (!bring_up(ctx, pd, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, 2000, admin)) { REQUIRE(false, "AER bring-up"); }
        std::vector<uint8_t> rsp_aer;
        ibv_mr* aer_mr = nullptr, *sq_mr = nullptr;
        if (!post_aer(admin, pd, 1, rsp_aer, aer_mr, sq_mr)) { REQUIRE(false, "AER post transport"); }
        // poll briefly: the AER should be parked (NO recv completion yet)
        int recv_seen = 0;
        for (int i = 0; i < 300; i++) {         // 300 ms
            ibv_wc wc{}; int n = ibv_poll_cq(admin.cq, 1, &wc);
            if (n > 0 && wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) recv_seen++;
            if (recv_seen == 0) usleep(1000);
        }
        REQUIRE(recv_seen == 0, "AER should be parked with no completion");
        LOG("AER posted and parked (no completion within 300 ms)");
        if (aer_mr) ibv_dereg_mr(aer_mr);
        if (sq_mr) ibv_dereg_mr(sq_mr);
        teardown_conn(admin);
    }

    // ===== Part 3: negative (no keep-alive -> controller fatal error) =====
    {
        LOG("=== negative (KATO=1000ms, no keep-alive) ===");
        Conn admin;
        if (!bring_up(ctx, pd, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, 1000, admin)) { REQUIRE(false, "negative bring-up"); }
        usleep(2500000);                        // 2.5 s idle, past 1 s KATO
        bool alive = send_keep_alive(admin, pd, 99);
        REQUIRE(!alive, "expected controller to be torn down after KATO expiry");
        LOG("controller torn down as expected (keep-alive now fails)");
        teardown_conn(admin);
    }

    ibv_dealloc_pd(pd); ibv_close_device(ctx);

    if (g_fail == 0) {
        LOG("KEEPALIVE/AER PASS");
        return 0;
    }
    ERR("KEEPALIVE/AER FAIL: %d failures", g_fail);
    return 1;
}