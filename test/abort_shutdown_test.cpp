// Abort + graceful shutdown test.
//   - Abort: send an Abort for a (non-existent) command; nvmet's abort is a no-op
//     that always completes with result bit 0 = 1 (aborted).
//   - Shutdown: CC.SHN (normal) -> poll CSTS.SHST -> CC.EN=0, then reconnect cleanly.
//
// Usage: sudo ./abort_shutdown_test  (target must be running kernel nvmet)
#include "initiator.hpp"

#include <cstring>
#include <vector>
#include <unistd.h>

using namespace nof;

static int g_fail = 0;
#define REQUIRE(cond, msg) do { if (!(cond)) { g_fail++; ERR("FAIL: %s (%s:%d)", msg, __FILE__, __LINE__); } } while (0)

int main() {
    ibv_context* ctx = ib_open_device(nullptr);
    if (!ctx) return 1;
    ibv_device_attr dev_attr{}; ibv_query_device(ctx, &dev_attr);
    ibv_gid local_gid{}; int sgid_idx = 0;
    if (ib_pick_gid(ctx, 1, &local_gid, &sgid_idx)) return 1;
    ibv_gid remote_gid = ip_to_gid(DST_IP);
    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) return 1;

    // ---- bring up controller ----
    Conn admin;
    if (!establish_conn(ctx, pd, 0, 0, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin)) { REQUIRE(false, "admin CM"); return 1; }
    uint16_t cntlid = 0;
    if (!fabric_connect(admin, pd, 0, 0xffff, QD, cntlid)) { REQUIRE(false, "admin connect"); return 1; }
    if (!enable_controller(admin, pd)) { REQUIRE(false, "CC.EN"); return 1; }

    // ---- 1) Abort (nvmet no-op: result bit 0 = 1) ----
    {
        uint32_t result = 0;
        if (!abort_cmd(admin, pd, 1, 0x1234, result)) { REQUIRE(false, "abort cmd"); }
        REQUIRE((result & 1) == 1, "abort result bit 0 should be 1");
        LOG("Abort accepted, result=0x%x (bit0=1)", result);
    }

    // ---- 2) probe CCTS before shutdown ----
    {
        uint64_t csts = 0;
        if (prop_get(admin, pd, 0, nvme::CSTS_REG, csts)) {
            REQUIRE((csts & 1) == 1, "CSTS.RDY should be 1 while enabled");
            LOG("CSTS before shutdown: 0x%llx (RDY=%d)", (unsigned long long)csts, (int)(csts & 1));
        }
    }

    // ---- 3) graceful shutdown ----
    if (!shutdown_controller(admin, pd)) { REQUIRE(false, "graceful shutdown"); }
    LOG("graceful shutdown done (CC.SHN -> CC.EN=0)");

    // ---- 4) after shutdown, CSTS.RDY should be 0 ----
    {
        uint64_t csts = 0;
        if (prop_get(admin, pd, 0, nvme::CSTS_REG, csts)) {
            REQUIRE((csts & 1) == 0, "CSTS.RDY should be 0 after shutdown");
            LOG("CSTS after shutdown: 0x%llx (RDY=%d)", (unsigned long long)csts, (int)(csts & 1));
        }
    }

    teardown_conn(admin);

    // ---- 5) reconnect cleanly after shutdown (no orphaned controller) ----
    Conn admin2;
    if (!establish_conn(ctx, pd, 0, 0, QD, local_gid, sgid_idx, remote_gid, dev_attr.sys_image_guid, admin2)) { REQUIRE(false, "reconnect CM"); return 1; }
    uint16_t cntlid2 = 0;
    if (!fabric_connect(admin2, pd, 0, 0xffff, QD, cntlid2)) { REQUIRE(false, "reconnect"); return 1; }
    if (!enable_controller(admin2, pd)) { REQUIRE(false, "reconnect CC.EN"); return 1; }
    LOG("reconnected after shutdown: cntlid=%u (reused)", cntlid2);
    teardown_conn(admin2);

    ibv_dealloc_pd(pd); ibv_close_device(ctx);

    if (g_fail == 0) { LOG("ABORT+SHUTDOWN PASS"); return 0; }
    ERR("ABORT+SHUTDOWN FAIL: %d", g_fail);
    return 1;
}