// Robustness tests: corner cases + stress for the pure (no-live-target) parts.
#include "cm.hpp"
#include "nvme.hpp"
#include "common.hpp"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace nof;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; ERR("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

// ---- deterministic PRNG (xorshift64) ----
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t xrand() { uint64_t x = g_rng; x ^= x << 13; x ^= x >> 7; x ^= x << 17; g_rng = x; return x; }
static uint32_t xrand32() { return (uint32_t)(xrand() & 0xffffffffu); }
static void fill_rand(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(xrand() & 0xff); }

// ---- independent reference CRC-32 (table-driven, same reflected poly) ----
static uint32_t crc_table[256];
static void crc_init_table() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
}
static uint32_t crc_ref(const uint8_t* p, size_t len, uint32_t seed) {
    uint32_t c = seed;
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}

// ================= CRC =================
static void test_crc_boundary() {
    LOG("test_crc_boundary");
    // null + 0 length must not dereference
    CHECK(crc32_raw(nullptr, 0, 0) == 0u);
    CHECK(crc32_raw(nullptr, 0, 0xDEBB20E3) == 0xDEBB20E3u);
    uint8_t b = 0x00;
    CHECK(crc32_raw(&b, 1, 0) == crc_ref(&b, 1, 0));
    b = 0xff;
    CHECK(crc32_raw(&b, 1, 0) == crc_ref(&b, 1, 0));
    uint8_t d[4] = {0xde, 0xad, 0xbe, 0xef};
    CHECK(crc32_raw(d, 4, 0xDEBB20E3) == crc_ref(d, 4, 0xDEBB20E3));
}

static void test_crc_fuzz() {
    LOG("test_crc_fuzz");
    for (int r = 0; r < 20000; r++) {
        size_t len = xrand32() % 1024;
        std::vector<uint8_t> buf(len);
        fill_rand(buf.data(), len);
        uint32_t seed = xrand32();
        CHECK(crc32_raw(buf.data(), len, seed) == crc_ref(buf.data(), len, seed));
    }
}

// ================= field helpers =================
static void test_field_exhaustive16() {
    LOG("test_field_exhaustive16");
    uint8_t b[2];
    for (uint32_t v = 0; v < 0x10000; v++) {
        put_be16(b, (uint16_t)v); CHECK(get_be16(b) == (uint16_t)v);
        put_le16(b, (uint16_t)v); CHECK(get_le16(b) == (uint16_t)v);
    }
}

static void test_field_random_roundtrip() {
    LOG("test_field_random_roundtrip");
    uint8_t b[9];
    for (int i = 0; i < 100000; i++) {
        uint8_t* u = b + 1;  // unaligned base exercises byte-copy safety
        uint32_t v24 = xrand32() & 0xffffff;
        put_be24(u, v24); CHECK(get_be24(u) == v24);
        put_le24(u, v24); CHECK(get_le24(u) == v24);
        uint32_t v32 = xrand32();
        put_be32(u, v32); CHECK(get_be32(u) == v32);
        put_le32(u, v32); CHECK(get_le32(u) == v32);
        uint64_t v64 = xrand();
        put_be64(u, v64); CHECK(get_be64(u) == v64);
        put_le64(u, v64); CHECK(get_le64(u) == v64);
    }
}

static void test_field_truncation() {
    LOG("test_field_truncation");
    uint8_t b[8] = {0};
    put_be24(b, 0x12345678u); CHECK(get_be24(b) == 0x345678u);
    put_le24(b, 0x12345678u); CHECK(get_le24(b) == 0x345678u);
    put_be16(b, (uint16_t)0x12345); CHECK(get_be16(b) == 0x2345);
    put_le16(b, (uint16_t)0x12345); CHECK(get_le16(b) == 0x2345);
}

// ================= CM packet =================
static void test_cm_packet_body_size() {
    LOG("test_cm_packet_body_size");
    auto p0 = build_cm_packet(CM_REQ_ATTR, 0, {});
    CHECK(p0.size() == CM_PACKET_LEN);

    std::vector<uint8_t> big(1000);
    fill_rand(big.data(), big.size());
    auto p1 = build_cm_packet(CM_REQ_ATTR, 0x1234, big);
    CHECK(p1.size() == CM_PACKET_LEN);
    const uint8_t* mad1 = p1.data() + BTH_LEN + DETH_LEN;
    CHECK(std::memcmp(mad1 + MAD_HDR_LEN, big.data(), MAD_BODY_LEN) == 0);

    std::vector<uint8_t> small(10, 0xab);
    auto p2 = build_cm_packet(CM_REQ_ATTR, 0, small);
    const uint8_t* mad2 = p2.data() + BTH_LEN + DETH_LEN;
    CHECK(std::memcmp(mad2 + MAD_HDR_LEN, small.data(), 10) == 0);
    CHECK(mad2[MAD_HDR_LEN + 10] == 0);
}

static void test_parse_packet_boundary() {
    LOG("test_parse_packet_boundary");
    ParsedMad out;
    CHECK(!parse_cm_packet({}, out));
    std::vector<uint8_t> p279(CM_PACKET_LEN - 1, 0);
    CHECK(!parse_cm_packet(p279, out));
    std::vector<uint8_t> p280(CM_PACKET_LEN, 0);
    CHECK(parse_cm_packet(p280, out));
    std::vector<uint8_t> p281(CM_PACKET_LEN + 1, 0);
    CHECK(parse_cm_packet(p281, out));
}

static void test_packet_roundtrip_fuzz() {
    LOG("test_packet_roundtrip_fuzz");
    for (int r = 0; r < 2000; r++) {
        std::vector<uint8_t> body(MAD_BODY_LEN);
        fill_rand(body.data(), body.size());
        uint16_t attr = (uint16_t)(xrand32() & 0xffff);
        uint64_t tid = xrand();
        auto pkt = build_cm_packet(attr, tid, body);
        ParsedMad out;
        CHECK(parse_cm_packet(pkt, out));
        CHECK(out.attr_id == attr);
        CHECK(out.tid == tid);
        CHECK(out.body == body);
    }
}

// ================= ICRC =================
static void test_icrc_determinism() {
    LOG("test_icrc_determinism");
    std::vector<uint8_t> pkt(CM_PACKET_LEN - ICRC_LEN);
    fill_rand(pkt.data(), pkt.size());
    uint8_t iph[20], udph[8];
    fill_rand(iph, 20); fill_rand(udph, 8);
    uint32_t a = cm_icrc(iph, udph, pkt.data(), pkt.size());
    uint32_t b = cm_icrc(iph, udph, pkt.data(), pkt.size());
    CHECK(a == b);
}

static void test_icrc_short_packet() {
    LOG("test_icrc_short_packet");
    uint8_t iph[20] = {0}, udph[8] = {0};
    uint8_t tiny[4] = {0};
    CHECK(cm_icrc(iph, udph, tiny, 4) == 0);   // pkt_len < BTH_LEN guarded, no OOB
}

// ================= SGL =================
static void test_sgl_boundary() {
    LOG("test_sgl_boundary");
    uint8_t sqe[64] = {0};
    nvme::sgl_keyed(sqe, 0, 0, 0);
    CHECK(sqe[39] == 0x40);
    CHECK(get_le24(sqe + 32) == 0);
    nvme::sgl_keyed(sqe, 0xffffffffffffffffull, 0xffffffffu, 0xffffff);
    CHECK(get_le64(sqe + 24) == 0xffffffffffffffffull);
    CHECK(get_le24(sqe + 32) == 0xffffff);
    CHECK(get_le32(sqe + 35) == 0xffffffffu);
    nvme::sgl_keyed(sqe, 0, 0, 0x1ffffffu);  // 2^25-1 -> truncated to 24 bits
    CHECK(get_le24(sqe + 32) == 0xffffff);
    nvme::sgl_data_block(sqe, 0x400);
    CHECK(sqe[39] == 0x01);
    CHECK(get_le32(sqe + 32) == 0x400);
}

// ================= connect data =================
static void test_connect_data_truncation() {
    LOG("test_connect_data_truncation");
    uint8_t data[nvme::CONNECT_DATA_LEN];
    std::string longsub(300, 'S'), longhost(300, 'H');
    nvme::build_connect_data(data, 0x1234, longsub, longhost);
    CHECK(std::memcmp(data + 256, longsub.data(), 256) == 0);
    CHECK(std::memcmp(data + 512, longhost.data(), 256) == 0);
    nvme::build_connect_data(data, 0, "", "");
    CHECK(data[256] == 0 && data[512] == 0);
}

// ================= SQE builders (self-zeroing) =================
static void test_sqe_self_zeroing() {
    LOG("test_sqe_self_zeroing");
    // Builders must not leak stale bytes from a dirty buffer.
    uint8_t dirty[64], clean[64];
    std::memset(dirty, 0xff, sizeof(dirty));
    std::memset(clean, 0, sizeof(clean));
    nvme::build_prop_set(dirty, nvme::CC_REG, nvme::CC_ENABLE);
    nvme::build_prop_set(clean, nvme::CC_REG, nvme::CC_ENABLE);
    CHECK(std::memcmp(dirty, clean, 64) == 0);

    std::memset(dirty, 0xff, sizeof(dirty));
    std::memset(clean, 0, sizeof(clean));
    nvme::build_rw(dirty, 0x01, 7, 1, 0, 0, 0x1000, 0x1234, 4096);
    nvme::build_rw(clean, 0x01, 7, 1, 0, 0, 0x1000, 0x1234, 4096);
    CHECK(std::memcmp(dirty, clean, 64) == 0);

    std::memset(dirty, 0xff, sizeof(dirty));
    std::memset(clean, 0, sizeof(clean));
    nvme::build_identify(dirty, 3, 1, 1, 0x2000, 0x1234, 4096);
    nvme::build_identify(clean, 3, 1, 1, 0x2000, 0x1234, 4096);
    CHECK(std::memcmp(dirty, clean, 64) == 0);
}

static void test_rw_boundary() {
    LOG("test_rw_boundary");
    uint8_t sqe[64] = {0};
    nvme::build_rw(sqe, 0x01, 0xffff, 0xffffffffu, 0xffffffffffffffffull, 0xffff,
                   0xffffffffffffffffull, 0xffffffffu, 0x1000);
    CHECK(sqe[0] == 0x01);
    CHECK(get_le16(sqe + 2) == 0xffff);
    CHECK(get_le32(sqe + 4) == 0xffffffffu);
    CHECK(get_le64(sqe + 40) == 0xffffffffffffffffull);
    CHECK(get_le16(sqe + 48) == 0xffff);
    CHECK(sqe[39] == 0x40);
    nvme::build_rw(sqe, 0x02, 0, 1, 0, 0, 0x1000, 0x1234, 4096);
    CHECK(get_le16(sqe + 48) == 0);
    CHECK(get_le64(sqe + 40) == 0);
}

static void test_propset_boundary() {
    LOG("test_propset_boundary");
    uint8_t sqe[64] = {0};
    nvme::build_prop_set(sqe, 0xffffffffu, 0xffffffffu);
    CHECK(sqe[39] == 0x40);
    CHECK(get_le32(sqe + 44) == 0xffffffffu);
    CHECK(get_le32(sqe + 48) == 0xffffffffu);
    CHECK(sqe[40] == 0);
}

int main() {
    crc_init_table();
    test_crc_boundary();
    test_crc_fuzz();
    test_field_exhaustive16();
    test_field_random_roundtrip();
    test_field_truncation();
    test_cm_packet_body_size();
    test_parse_packet_boundary();
    test_packet_roundtrip_fuzz();
    test_icrc_determinism();
    test_icrc_short_packet();
    test_sgl_boundary();
    test_connect_data_truncation();
    test_sqe_self_zeroing();
    test_rw_boundary();
    test_propset_boundary();
    if (g_failures == 0) {
        LOG("ALL ROBUSTNESS TESTS PASSED (%d checks)", g_checks);
        return 0;
    }
    ERR("ROBUSTNESS TESTS FAILED: %d/%d", g_failures, g_checks);
    return 1;
}
