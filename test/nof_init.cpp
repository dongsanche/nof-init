// Full self-written NVMe-oF/RDMA initiator: admin queue + one IO queue + read/write.
#include "initiator.hpp"

#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

using namespace nof;

// send a 64-byte SQE (no data) and wait for response; returns response status
static bool send_admin(Conn& c, ibv_pd* pd, uint8_t sqe[64]) {
    ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    if (!sm) return false;
    memset(c.rsp.data(), 0, c.rsp.size());
    ibv_sge rs{ (uintptr_t)c.rsp.data(), (uint32_t)c.rsp.size(), c.rsp_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id=1; rwr.sg_list=&rs; rwr.num_sge=1;
    ibv_recv_wr* rbad=nullptr; if (ibv_post_recv(c.qp,&rwr,&rbad)) { ibv_dereg_mr(sm); return false; }
    ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
    ibv_send_wr wr{}; wr.wr_id=0; wr.sg_list=&s; wr.num_sge=1; wr.opcode=IBV_WR_SEND; wr.send_flags=IBV_SEND_SIGNALED;
    ibv_send_wr* bad=nullptr; if (ibv_post_send(c.qp,&wr,&bad)) { ibv_dereg_mr(sm); return false; }
    bool ok=false;
    for (int i=0;i<2000;i++) {
        ibv_wc wc{}; int n=ibv_poll_cq(c.cq,1,&wc);
        if (n<=0) { usleep(1000); continue; }
        if (wc.status != IBV_WC_SUCCESS) break;
        if (wc.opcode==IBV_WC_RECV) { uint16_t st=c.rsp[14]|(c.rsp[15]<<8);
            if (st) {
                LOG("  admin rsp status=0x%04x sc=0x%x sct=0x%x", st, st&0xff, (st>>8)&0x7);
                LOG("  cqe: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                    c.rsp[0],c.rsp[1],c.rsp[2],c.rsp[3],c.rsp[4],c.rsp[5],c.rsp[6],c.rsp[7],
                    c.rsp[8],c.rsp[9],c.rsp[10],c.rsp[11],c.rsp[12],c.rsp[13],c.rsp[14],c.rsp[15]);
            }
            ok=(st==0); break; }
    }
    ibv_dereg_mr(sm);
    return ok;
}

// debug: read the Error Information log (LID=0x01) and dump param_error_location
static void dump_error_log(Conn& c, ibv_pd* pd) {
    std::vector<uint8_t> buf(64, 0);
    ibv_mr* mr = ibv_reg_mr(pd, buf.data(), buf.size(), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) return;
    uint8_t sqe[64];
    nvme::build_get_log_page(sqe, 0, 0x01, 16, (uint64_t)buf.data(), mr->rkey, buf.size());

    ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
    memset(c.rsp.data(), 0, c.rsp.size());
    ibv_sge rs{ (uintptr_t)c.rsp.data(), (uint32_t)c.rsp.size(), c.rsp_mr->lkey };
    ibv_recv_wr rwr{}; rwr.wr_id=1; rwr.sg_list=&rs; rwr.num_sge=1;
    ibv_recv_wr* rbad=nullptr;
    if (ibv_post_recv(c.qp,&rwr,&rbad)) { ibv_dereg_mr(sm); ibv_dereg_mr(mr); return; }
    ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
    ibv_send_wr wr{}; wr.wr_id=0; wr.sg_list=&s; wr.num_sge=1; wr.opcode=IBV_WR_SEND; wr.send_flags=IBV_SEND_SIGNALED;
    ibv_send_wr* bad=nullptr;
    if (ibv_post_send(c.qp,&wr,&bad)) { ibv_dereg_mr(sm); ibv_dereg_mr(mr); return; }
    for (int i=0;i<2000;i++) {
        ibv_wc wc{}; int n=ibv_poll_cq(c.cq,1,&wc);
        if (n<=0) { usleep(1000); continue; }
        if (wc.opcode==IBV_WC_RECV) {
            uint16_t st=c.rsp[14]|(c.rsp[15]<<8);
            LOG("get_log_page status=0x%04x sc=0x%x", st, (st>>1)&0xff);
            LOG("error slot: errcnt=%llu sqid=%u cmdid=%u status_field=0x%x param_error_location=%u",
                (unsigned long long)get_le64(buf.data()+0), get_le16(buf.data()+8), get_le16(buf.data()+10),
                get_le16(buf.data()+12), get_le16(buf.data()+14));
            break;
        }
    }
    ibv_dereg_mr(sm);
    ibv_dereg_mr(mr);
}

int main() {
    ibv_context* ctx = ib_open_device(nullptr);
    if (!ctx) return 1;
    ibv_device_attr dev_attr{}; ibv_query_device(ctx,&dev_attr);
    ibv_gid local_gid{}; int sgid_idx=0;
    if (ib_pick_gid(ctx,1,&local_gid,&sgid_idx)) return 1;
    ibv_gid remote_gid = ip_to_gid(DST_IP);
    LOG("local GID[%d]=%s remote GID=%s", sgid_idx, gid_to_string(local_gid).c_str(), gid_to_string(remote_gid).c_str());
    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) return 1;

    // ---- admin queue ----
    Conn admin;
    if (!establish_conn(ctx,pd,0,0,QD,local_gid,sgid_idx,remote_gid,dev_attr.sys_image_guid,admin)) { ERR("admin CM failed"); return 1; }
    LOG("admin qpair established");

    uint16_t cntlid=0;
    if (!fabric_connect(admin,pd,0,0xffff,QD,cntlid)) { ERR("admin Fabric Connect failed"); return 1; }
    LOG("admin connected: cntlid=%u", cntlid);

    // enable controller (CC.EN)
    {
        uint8_t ps[64]={0};
        nvme::build_prop_set(ps, nvme::CC_REG, nvme::CC_ENABLE);
        LOG("prop_set sqe: opc=%02x flags=%02x fctype=%02x attrib=%02x ofst=%08x value=%016llx",
            ps[0], ps[1], ps[4], ps[40], get_le32(ps+44), (unsigned long long)get_le64(ps+48));
        if (!send_admin(admin,pd,ps)) { ERR("Property Set failed"); dump_error_log(admin,pd); return 1; }
        usleep(100000);
        LOG("controller enabled (CC.EN)");
    }

    // ---- Identify Controller + Namespace (learn capabilities, drop hardcoded nsid/block size) ----
    std::vector<uint8_t> idc, idns;
    uint32_t block_size = 4096;
    if (identify(admin, pd, 1, 0x01, 0, idc)) {
        LOG("Identify Controller: mn='%s' sn='%s' fr='%s' mdts=%u nn=%u",
            nvme::id_ctrl_mn(idc.data()).c_str(), nvme::id_ctrl_sn(idc.data()).c_str(),
            nvme::id_ctrl_fr(idc.data()).c_str(), nvme::id_ctrl_mdts(idc.data()),
            nvme::id_ctrl_nn(idc.data()));
    } else { ERR("Identify Controller failed"); return 1; }
    if (identify(admin, pd, 2, 0x00, 1, idns)) {
        block_size = nvme::id_ns_block_size(idns.data());
        LOG("Identify Namespace 1: nsze=%llu ncap=%llu nuse=%llu block_size=%u",
            (unsigned long long)nvme::id_ns_nsze(idns.data()),
            (unsigned long long)nvme::id_ns_ncap(idns.data()),
            (unsigned long long)nvme::id_ns_nuse(idns.data()), block_size);
    } else { ERR("Identify Namespace failed"); return 1; }

    // ---- IO queue (no Create IO CQ/SQ in NVMe-oF; the RDMA qpair IS the queue) ----
    Conn io;
    if (!establish_conn(ctx,pd,1,cntlid,QD,local_gid,sgid_idx,remote_gid,dev_attr.sys_image_guid,io)) { ERR("IO CM failed"); return 1; }
    LOG("io qpair established");
    uint16_t io_cntlid=0;
    if (!fabric_connect(io,pd,1,cntlid,QD,io_cntlid)) { ERR("IO Fabric Connect failed"); return 1; }
    LOG("io connected");

    // ---- read/write test on namespace 1 (block size from Identify) ----
    const size_t DATA_LEN = block_size;
    std::vector<uint8_t> wbuf(DATA_LEN, 0), rbuf(DATA_LEN, 0);
    for (size_t i=0;i<DATA_LEN;i++) wbuf[i] = (uint8_t)(i * 7 + 1);
    ibv_mr* wmr = ibv_reg_mr(pd, wbuf.data(), DATA_LEN, IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ);
    ibv_mr* rmr = ibv_reg_mr(pd, rbuf.data(), DATA_LEN, IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ);

    auto io_cmd = [&](uint8_t opcode, void* data, ibv_mr* mr) -> bool {
        uint8_t sqe[64]={0};
        nvme::build_rw(sqe, opcode, (uint16_t)(10+opcode), 1, 0, 0, (uint64_t)data, mr->rkey, DATA_LEN);
        memset(io.rsp.data(), 0, io.rsp.size());
        ibv_sge rs{ (uintptr_t)io.rsp.data(), (uint32_t)io.rsp.size(), io.rsp_mr->lkey };
        ibv_recv_wr rwr{}; rwr.wr_id=1; rwr.sg_list=&rs; rwr.num_sge=1;
        ibv_recv_wr* rbad=nullptr;
        ibv_mr* sm = ibv_reg_mr(pd, sqe, 64, IBV_ACCESS_LOCAL_WRITE);
        if (ibv_post_recv(io.qp,&rwr,&rbad)) { ibv_dereg_mr(sm); return false; }
        ibv_sge s{ (uintptr_t)sqe, 64, sm->lkey };
        ibv_send_wr wr{}; wr.wr_id=0; wr.sg_list=&s; wr.num_sge=1; wr.opcode=IBV_WR_SEND; wr.send_flags=IBV_SEND_SIGNALED;
        ibv_send_wr* bad=nullptr;
        if (ibv_post_send(io.qp,&wr,&bad)) { ibv_dereg_mr(sm); return false; }
        bool ok=false;
        for (int i=0;i<3000;i++) {
            ibv_wc wc{}; int n=ibv_poll_cq(io.cq,1,&wc);
            if (n<=0) { usleep(1000); continue; }
            if (wc.status != IBV_WC_SUCCESS) break;
            if (wc.opcode==IBV_WC_RECV) { uint16_t st=io.rsp[14]|(io.rsp[15]<<8);
                if (st) LOG("  io rsp status=0x%04x sc=0x%x sct=0x%x", st, st&0xff, (st>>8)&0x7);
                ok=(st==0); break; }
        }
        ibv_dereg_mr(sm);
        return ok;
    };

    bool wok = io_cmd(0x01, wbuf.data(), wmr);
    bool rok = io_cmd(0x02, rbuf.data(), rmr);
    if (!wok) { ERR("WRITE failed"); return 1; }
    if (!rok) { ERR("READ failed"); return 1; }
    LOG("write + read complete");

    if (memcmp(wbuf.data(), rbuf.data(), DATA_LEN)==0) {
        LOG("M4/M5 PASS: %zu-byte write+read data integrity OK", DATA_LEN);
    } else {
        ERR("M4/M5 FAIL: data mismatch");
        return 1;
    }

    ibv_dereg_mr(wmr); ibv_dereg_mr(rmr);
    teardown_conn(io);
    teardown_conn(admin);
    ibv_dealloc_pd(pd); ibv_close_device(ctx);
    return 0;
}