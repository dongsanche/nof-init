// Unit tests for the pure (no-live-target) parts of the NVMe-oF/RDMA initiator.
#include "cm.hpp"
#include "nvme.hpp"
#include "verbs.hpp"
#include "common.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace nof;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; ERR("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

static void test_crc32() {
    LOG("test_crc32");
    CHECK(crc32_raw("", 0, 0) == 0x0u);
    CHECK(crc32_raw("", 0, 0xDEBB20E3) == 0xDEBB20E3u);
    const char* s = "123456789";
    CHECK(crc32_raw(s, 9, 0) == 0x2dfd2d88u);
    CHECK(crc32_raw(s, 9, 0xFFFFFFFF) == 0x340bc6d9u);
}

// Captured CM REQ packet (from a real kernel nvme-rdma initiator); ICRC = 0xbd2fb0b3.
static const uint8_t k_iph[20] = {
    0x45, 0x00, 0x01, 0x34, 0xb4, 0xd9, 0x40, 0x00, 0x40, 0x11, 0x05, 0x6f, 0xc0, 0xa8, 0xff, 0x14,
    0xc0, 0xa8, 0xff, 0x0a,
};
static const uint8_t k_udph[8] = {
    0xd8, 0x72, 0x12, 0xb7, 0x01, 0x20, 0x00, 0x00,
};
static const uint8_t k_pkt[276] = {
    0x64, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x01, 0x07, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0xc2, 0xd7, 0x34, 0xbb, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbb, 0x34, 0xd7, 0xc2,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x06, 0x11, 0x44, 0x02, 0x0c, 0x29, 0xff,
    0xfe, 0x1d, 0x98, 0x95, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa1, 0x81, 0xbc, 0x2c, 0xa7, 0xff, 0xff, 0x37, 0xf0,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
    0xc0, 0xa8, 0xff, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
    0xc0, 0xa8, 0xff, 0x0a, 0xae, 0xdf, 0x00, 0x00, 0x00, 0x40, 0x00, 0x98, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xdd, 0xd4, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0xff, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0xff, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

static void test_icrc() {
    LOG("test_icrc");
    uint32_t icrc = cm_icrc(k_iph, k_udph, k_pkt, 276);
    CHECK(icrc == 0xb3b02fbdu);  // host-order ~crc; stored LE it's bd 2f b0 b3
    uint8_t le[4] = { (uint8_t)(icrc & 0xff), (uint8_t)((icrc >> 8) & 0xff),
                      (uint8_t)((icrc >> 16) & 0xff), (uint8_t)((icrc >> 24) & 0xff) };
    CHECK(le[0] == 0xbd && le[1] == 0x2f && le[2] == 0xb0 && le[3] == 0xb3);
}

static void test_field_helpers() {
    LOG("test_field_helpers");
    uint8_t b[8] = {0};
    put_be16(b, 0x1234); CHECK(get_be16(b) == 0x1234);
    put_be16(b, 0xffff); CHECK(get_be16(b) == 0xffff);
    put_be24(b, 0x123456); CHECK(get_be24(b) == 0x123456);
    put_be24(b, 0xffffff); CHECK(get_be24(b) == 0xffffff);
    put_be32(b, 0x12345678); CHECK(get_be32(b) == 0x12345678);
    put_be32(b, 0xffffffff); CHECK(get_be32(b) == 0xffffffff);
    put_be64(b, 0x123456789abcdef0ull); CHECK(get_be64(b) == 0x123456789abcdef0ull);

    put_le16(b, 0x1234); CHECK(get_le16(b) == 0x1234);
    put_le24(b, 0x123456); CHECK(get_le24(b) == 0x123456);
    put_le32(b, 0x12345678); CHECK(get_le32(b) == 0x12345678);
    put_le64(b, 0x123456789abcdef0ull); CHECK(get_le64(b) == 0x123456789abcdef0ull);

    // byte order cross-check: put_be16(0x1234) => 12 34; put_le16(0x1234) => 34 12
    put_be16(b, 0x1234); CHECK(b[0] == 0x12 && b[1] == 0x34);
    put_le16(b, 0x1234); CHECK(b[0] == 0x34 && b[1] == 0x12);
    put_be32(b, 0x12345678); CHECK(b[0] == 0x12 && b[1] == 0x34 && b[2] == 0x56 && b[3] == 0x78);
    put_le32(b, 0x12345678); CHECK(b[0] == 0x78 && b[1] == 0x56 && b[2] == 0x34 && b[3] == 0x12);
}

static void test_cm_packet_roundtrip() {
    LOG("test_cm_packet_roundtrip");
    std::vector<uint8_t> body(MAD_BODY_LEN, 0);
    for (size_t i = 0; i < body.size(); i++) body[i] = (uint8_t)(i * 3 + 1);
    auto pkt = build_cm_packet(CM_REQ_ATTR, 0x123456789abcdef0ull, body);
    CHECK(pkt.size() == CM_PACKET_LEN);
    // BTH opcode + MAD header sanity
    CHECK(pkt[0] == CM_BTH_OPCODE);
    const uint8_t* mad = pkt.data() + BTH_LEN + DETH_LEN;
    CHECK(mad[0] == MAD_BASE_VER && mad[1] == MAD_MGMT_CLASS && mad[2] == MAD_CLASS_VER && mad[3] == MAD_METHOD_SEND);
    CHECK(get_be16(mad + 16) == CM_REQ_ATTR);

    ParsedMad out;
    CHECK(parse_cm_packet(pkt, out));
    CHECK(out.attr_id == CM_REQ_ATTR);
    CHECK(out.tid == 0x123456789abcdef0ull);
    CHECK(out.body == body);
    // bad size rejected
    ParsedMad bad;
    std::vector<uint8_t> shortpkt(10, 0);
    CHECK(!parse_cm_packet(shortpkt, bad));
}

static void test_sgl_builders() {
    LOG("test_sgl_builders");
    uint8_t sqe[64] = {0};

    nvme::sgl_keyed(sqe, 0x1122334455667788ull, 0x0a0b0c0du, 0x1000);
    CHECK(get_le64(sqe + 24) == 0x1122334455667788ull);
    CHECK(get_le24(sqe + 32) == 0x1000);
    CHECK(get_le32(sqe + 35) == 0x0a0b0c0du);
    CHECK(sqe[39] == 0x40);  // type=keyed(4) subtype=address(0)

    std::memset(sqe, 0, sizeof(sqe));
    nvme::sgl_data_block(sqe, 0x400);
    CHECK(get_le64(sqe + 24) == 0);
    CHECK(get_le32(sqe + 32) == 0x400);
    CHECK(sqe[39] == 0x01);  // type=data_block(0) subtype=offset(1)
}

static void test_connect_capsule() {
    LOG("test_connect_capsule");
    uint8_t data[nvme::CONNECT_DATA_LEN];
    nvme::build_connect_data(data, 0xffff, "nqn.2016-06.io.spdk:cnode1",
                             "nqn.2014-08.org.nvmexpress:uuid:x");
    CHECK(get_le16(data + 16) == 0xffff);  // cntlid
    CHECK(std::memcmp(data + 256, "nqn.2016-06.io.spdk:cnode1", 27) == 0);  // subnqn
    static const char* hostnqn = "nqn.2014-08.org.nvmexpress:uuid:x";
    CHECK(std::memcmp(data + 512, hostnqn, std::strlen(hostnqn)) == 0);  // hostnqn

    uint8_t sqe[64] = {0};
    nvme::build_connect_sqe(sqe, 0, 63, 0x1122334455667788ull, 0x0a0b0c0du);
    CHECK(sqe[0] == nvme::OPC_FABRIC);
    CHECK(sqe[1] == 0x40);                 // psdt = SGL
    CHECK(sqe[4] == nvme::FCTYPE_CONNECT);
    CHECK(get_le16(sqe + 42) == 0);        // qid = 0 (admin)
    CHECK(get_le16(sqe + 44) == 63);       // sqsize
    // Keyed SGL: addr + rkey + len
    CHECK(get_le64(sqe + 24) == 0x1122334455667788ull);
    CHECK(get_le32(sqe + 35) == 0x0a0b0c0du);
    CHECK(get_le24(sqe + 32) == nvme::CONNECT_DATA_LEN);
    CHECK(sqe[39] == 0x40);                // keyed data block
}

static void test_prop_set() {
    LOG("test_prop_set");
    uint8_t sqe[64] = {0};
    nvme::build_prop_set(sqe, nvme::CC_REG, nvme::CC_ENABLE);
    CHECK(sqe[0] == nvme::OPC_FABRIC);
    CHECK(sqe[1] == 0x40);                    // psdt = SGL
    CHECK(sqe[4] == nvme::FCTYPE_PROPERTY_SET);
    CHECK(sqe[39] == 0x40);                   // null keyed SGL (len=0), kernel nvmet requires this
    CHECK(get_le24(sqe + 32) == 0);           // SGL length = 0
    CHECK(get_le32(sqe + 44) == nvme::CC_REG);
    CHECK(get_le32(sqe + 48) == nvme::CC_ENABLE);
    CHECK(sqe[40] == 0);                      // attrib = 0
}

static void test_keepalive_aer_builders() {
    LOG("test_keepalive_aer_builders");
    uint8_t sqe[64] = {0};
    nvme::build_keep_alive(sqe, 7);
    CHECK(sqe[0] == nvme::OPC_KEEP_ALIVE);
    CHECK(sqe[1] == 0x40);                    // psdt = SGL
    CHECK(get_le16(sqe + 2) == 7);
    CHECK(sqe[39] == 0x40);                   // null keyed SGL
    CHECK(get_le24(sqe + 32) == 0);

    nvme::build_async_event(sqe, 9);
    CHECK(sqe[0] == nvme::OPC_ASYNC_EVENT);
    CHECK(sqe[1] == 0x40);
    CHECK(get_le16(sqe + 2) == 9);
    CHECK(sqe[39] == 0x40);

    nvme::build_connect_sqe(sqe, 0, 31, 0x1000, 0x1234, 5000);
    CHECK(get_le32(sqe + 48) == 5000);        // kato_ms field
}

static void test_identify_parsers() {
    LOG("test_identify_parsers");
    std::vector<uint8_t> idc(nvme::IDENTIFY_DATA_SIZE, 0);
    memcpy(idc.data() + 24, "TEST-MODEL", 10);   // mn
    memcpy(idc.data() + 4, "SN123", 5);          // sn
    idc[77] = 5;                                  // mdts
    put_le32(idc.data() + 516, 4);                // nn
    CHECK(nvme::id_ctrl_mn(idc.data()) == "TEST-MODEL");
    CHECK(nvme::id_ctrl_sn(idc.data()) == "SN123");
    CHECK(nvme::id_ctrl_mdts(idc.data()) == 5);
    CHECK(nvme::id_ctrl_nn(idc.data()) == 4);

    std::vector<uint8_t> idns(nvme::IDENTIFY_DATA_SIZE, 0);
    put_le64(idns.data() + 0, 4096);              // nsze
    put_le64(idns.data() + 16, 4096);             // nuse
    idns[26] = 0;                                  // flbas = format 0
    idns[128 + 0 * 4 + 2] = 12;                    // lbaf[0].ds = 12 -> 4096
    CHECK(nvme::id_ns_nsze(idns.data()) == 4096);
    CHECK(nvme::id_ns_nuse(idns.data()) == 4096);
    CHECK(nvme::id_ns_block_size(idns.data()) == 4096);
    idns[128 + 0 * 4 + 2] = 9;                     // ds = 9 -> 512
    CHECK(nvme::id_ns_block_size(idns.data()) == 512);
}

static void test_set_get_features() {
    LOG("test_set_get_features");
    uint8_t sqe[64] = {0};
    nvme::build_set_features(sqe, 5, nvme::FID_NUM_QUEUES, nvme::nqr_request(4, 4));
    CHECK(sqe[0] == nvme::OPC_SET_FEATURES);
    CHECK(sqe[1] == 0x40);                        // psdt = SGL
    CHECK(get_le16(sqe + 2) == 5);
    CHECK(sqe[39] == 0x40);                       // null keyed SGL
    CHECK(get_le32(sqe + 40) == nvme::FID_NUM_QUEUES);
    CHECK(get_le32(sqe + 44) == 0x00040004u);     // ncqr=4, nsqr=4

    nvme::build_get_features(sqe, 6, nvme::FID_NUM_QUEUES);
    CHECK(sqe[0] == nvme::OPC_GET_FEATURES);
    CHECK(get_le16(sqe + 2) == 6);
    CHECK(sqe[39] == 0x40);
    CHECK(get_le32(sqe + 40) == nvme::FID_NUM_QUEUES);

    uint32_t r = nvme::nqr_request(7, 3);
    CHECK(r == ((7u << 16) | 3u));
    CHECK(nvme::nqr_allocated(0x00030003u) == 4); // low 16 = 3 -> +1 = 4
}

static void test_abort_builder() {
    LOG("test_abort_builder");
    uint8_t sqe[64] = {0};
    nvme::build_abort(sqe, 7, 2, 0xdead);
    CHECK(sqe[0] == nvme::OPC_ABORT);
    CHECK(sqe[1] == 0x40);                    // psdt = SGL
    CHECK(get_le16(sqe + 2) == 7);
    CHECK(sqe[39] == 0x40);                   // null keyed SGL
    CHECK(get_le32(sqe + 40) == ((2u << 16) | 0xdead));  // cdw10 = SQID<<16 | CID
}

static void test_prop_get_builder() {
    LOG("test_prop_get_builder");
    uint8_t sqe[64] = {0};
    nvme::build_prop_get(sqe, 0, nvme::CSTS_REG);
    CHECK(sqe[0] == nvme::OPC_FABRIC);
    CHECK(sqe[1] == 0x40);                    // psdt = SGL
    CHECK(sqe[4] == nvme::FCTYPE_PROPERTY_GET);
    CHECK(sqe[39] == 0x40);                   // null keyed SGL
    CHECK(sqe[40] == 0);                      // attrib = 0 (normal reg)
    CHECK(get_le32(sqe + 44) == nvme::CSTS_REG);

    nvme::build_prop_get(sqe, 1, nvme::CAP_REG);
    CHECK(sqe[40] == 1);                      // attrib = 1 (CAP)
    CHECK(get_le32(sqe + 44) == nvme::CAP_REG);
}

static void test_wc_status() {
    LOG("test_wc_status");
    CHECK(std::strcmp(wc_status_name(IBV_WC_SUCCESS), "SUCCESS") == 0);
    CHECK(std::strcmp(wc_status_name(IBV_WC_WR_FLUSH_ERR), "WR_FLUSH_ERR") == 0);
    CHECK(std::strcmp(wc_status_name(IBV_WC_REM_ACCESS_ERR), "REM_ACCESS_ERR") == 0);
    CHECK(std::strcmp(wc_status_name(IBV_WC_RETRY_EXC_ERR), "RETRY_EXC_ERR") == 0);
    CHECK(!wc_is_fatal(IBV_WC_SUCCESS));
    CHECK(wc_is_fatal(IBV_WC_WR_FLUSH_ERR));
    CHECK(wc_is_fatal(IBV_WC_REM_OP_ERR));
}

int main() {
    test_crc32();
    test_icrc();
    test_field_helpers();
    test_cm_packet_roundtrip();
    test_sgl_builders();
    test_connect_capsule();
    test_prop_set();
    test_keepalive_aer_builders();
    test_identify_parsers();
    test_set_get_features();
    test_abort_builder();
    test_prop_get_builder();
    test_wc_status();
    if (g_failures == 0) {
        LOG("ALL UNIT TESTS PASSED (%d checks)", g_checks);
        return 0;
    }
    ERR("UNIT TESTS FAILED: %d/%d", g_failures, g_checks);
    return 1;
}
