#pragma once

// NVMe-oF command construction helpers (pure, unit-testable). All functions write
// into a caller-provided 64-byte SQE buffer using little-endian fields.
#include "cm.hpp"

#include <algorithm>
#include <cstring>

namespace nof {
namespace nvme {

// ---- NVMe command constants (subset) ----
constexpr uint8_t  OPC_FABRIC          = 0x7f;
constexpr uint8_t  OPC_IDENTIFY        = 0x06;
constexpr uint8_t  OPC_KEEP_ALIVE      = 0x18;
constexpr uint8_t  OPC_ASYNC_EVENT     = 0x0c;
constexpr uint8_t  OPC_ABORT           = 0x08;
constexpr uint8_t  OPC_SET_FEATURES    = 0x09;
constexpr uint8_t  OPC_GET_FEATURES    = 0x0a;
constexpr uint8_t  OPC_READ            = 0x02;  // I/O command set
constexpr uint8_t  OPC_WRITE           = 0x01;  // I/O command set

// Feature identifiers
constexpr uint8_t  FID_NUM_QUEUES      = 0x07;

// Fabric command types
constexpr uint8_t  FCTYPE_PROPERTY_SET = 0x00;
constexpr uint8_t  FCTYPE_CONNECT      = 0x01;
constexpr uint8_t  FCTYPE_PROPERTY_GET = 0x04;

// PSDT value (placed in SQE flags byte bits 6-7 => 0x40)
constexpr uint8_t  PSDT_SGL            = 0x1;

// SGL descriptor type/subtype
constexpr uint8_t  SGL_TYPE_DATA_BLOCK = 0x0;
constexpr uint8_t  SGL_TYPE_KEYED      = 0x4;
constexpr uint8_t  SGL_SUBTYPE_OFFSET  = 0x1;
constexpr uint8_t  SGL_SUBTYPE_ADDRESS = 0x0;

// Controller Configuration (CC) register offset + enable value
constexpr uint32_t CC_REG              = 0x14;
constexpr uint32_t CC_ENABLE           = 0x00460001;  // EN=1, IOSQES=6, IOCQES=4
constexpr uint32_t CC_SHN_NORMAL       = 0x00004000;  // SHN=01 (normal shutdown), bit 14

// Other controller registers (Property Get/Set offsets)
constexpr uint32_t CAP_REG             = 0x00;
constexpr uint32_t VS_REG              = 0x08;
constexpr uint32_t CSTS_REG            = 0x1c;

// CSTS register bits
constexpr uint8_t  CSTS_SHST_SHIFT     = 2;    // Shutdown Status (bits 2-3)
constexpr uint8_t  CSTS_SHST_COMPLETE  = 0x2;  // 10b = shutdown complete

constexpr size_t   CONNECT_DATA_LEN    = 1024;

// Write a Data Block SGL (in-capsule data, subtype=offset) at SQE[24..40].
inline void sgl_data_block(uint8_t* sqe, uint32_t len) {
    put_le64(sqe + 24, 0);
    put_le32(sqe + 32, len);
    sqe[39] = (SGL_TYPE_DATA_BLOCK << 4) | SGL_SUBTYPE_OFFSET;
}

// Write a Keyed Data Block SGL (remote RDMA data, subtype=address) at SQE[24..40].
inline void sgl_keyed(uint8_t* sqe, uint64_t addr, uint32_t rkey, uint32_t len) {
    put_le64(sqe + 24, addr);
    put_le24(sqe + 32, len);
    put_le32(sqe + 35, rkey);
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;
}

// Fill the 1024-byte Fabric Connect data buffer (hostid/cntlid/subnqn/hostnqn).
inline void build_connect_data(uint8_t* data, uint16_t cntlid,
                               const std::string& subnqn, const std::string& hostnqn) {
    std::memset(data, 0, CONNECT_DATA_LEN);
    put_le16(data + 16, cntlid);
    std::memcpy(data + 256, subnqn.data(), std::min(subnqn.size(), (size_t)256));
    std::memcpy(data + 512, hostnqn.data(), std::min(hostnqn.size(), (size_t)256));
}

// Build the 64-byte Fabric Connect SQE. The connect data lives in a registered
// buffer described by a Keyed SGL (addr + rkey); the target RDMA READs it.
// This is required by the kernel nvmet target, whose admin RQ has no inline data.
inline void build_connect_sqe(uint8_t* sqe, uint16_t qid, uint16_t sqsize,
                              uint64_t data_addr, uint32_t data_rkey,
                              uint32_t kato_ms = 0) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_FABRIC;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    sqe[4] = FCTYPE_CONNECT;
    sgl_keyed(sqe, data_addr, data_rkey, CONNECT_DATA_LEN);
    put_le16(sqe + 40, 0);                  // recfmt
    put_le16(sqe + 42, qid);
    put_le16(sqe + 44, sqsize);
    put_le32(sqe + 48, kato_ms);            // keep-alive timeout (ms); 0 = disabled
}

// Build a Fabric Property Set command (enable controller). Writes into sqe[64].
inline void build_prop_set(uint8_t* sqe, uint32_t ofst, uint32_t value) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_FABRIC;
    sqe[1] = PSDT_SGL << 6;                 // 0x40: fabrics 要求 PSDT=SGL_METABUF
    sqe[4] = FCTYPE_PROPERTY_SET;
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;  // 0x40: null keyed SGL (len=0)
    put_le32(sqe + 44, ofst);
    put_le32(sqe + 48, value);              // value.u32.low
}

// Build a Keep Alive command (admin, no data). Writes into sqe[64].
inline void build_keep_alive(uint8_t* sqe, uint16_t cid) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_KEEP_ALIVE;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    put_le16(sqe + 2, cid);
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;  // null keyed SGL (len=0)
}

// Build an Abort command (admin, no data). sqid/cid identify the command to abort
// (cdw10 = SQID<<16 | CID). Writes into sqe[64].
inline void build_abort(uint8_t* sqe, uint16_t cid, uint16_t sqid, uint16_t abort_cid) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_ABORT;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    put_le16(sqe + 2, cid);
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;  // null keyed SGL (len=0)
    put_le32(sqe + 40, ((uint32_t)sqid << 16) | abort_cid); // cdw10
}

// Build an Asynchronous Event Request command (admin, no data). Writes into sqe[64].
inline void build_async_event(uint8_t* sqe, uint16_t cid) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_ASYNC_EVENT;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    put_le16(sqe + 2, cid);
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;  // null keyed SGL (len=0)
}

// Build an Identify command with a Keyed SGL data buffer. Writes into sqe[64].
inline void build_identify(uint8_t* sqe, uint16_t cid, uint32_t cns, uint32_t nsid,
                           uint64_t data_addr, uint32_t rkey, uint32_t data_len) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_IDENTIFY;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    put_le16(sqe + 2, cid);
    put_le32(sqe + 4, nsid);
    sgl_keyed(sqe, data_addr, rkey, data_len);
    put_le32(sqe + 40, cns);                // cdw10 = CNS
}

// Build a Fabric Property Get command. attrib: 0 = normal regs (VS/CC/CSTS), 1 = CAP.
inline void build_prop_get(uint8_t* sqe, uint8_t attrib, uint32_t ofst) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_FABRIC;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    sqe[4] = FCTYPE_PROPERTY_GET;
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;  // null keyed SGL (len=0)
    sqe[40] = attrib;
    put_le32(sqe + 44, ofst);
}

// Build an admin Get Log Page command (log page id + numd) with a Keyed SGL. Writes into sqe[64].
inline void build_get_log_page(uint8_t* sqe, uint16_t cid, uint8_t lid, uint16_t numd,
                               uint64_t data_addr, uint32_t rkey, uint32_t data_len) {
    std::memset(sqe, 0, 64);
    sqe[0] = 0x02;                          // Get Log Page
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    put_le16(sqe + 2, cid);
    put_le32(sqe + 4, 0xffffffffu);         // nsid = controller-scoped
    sgl_keyed(sqe, data_addr, rkey, data_len);
    put_le32(sqe + 40, (uint32_t)lid | ((uint32_t)numd << 16));  // cdw10
}

// Build an I/O Read/Write command with a Keyed SGL. Writes into sqe[64].
inline void build_rw(uint8_t* sqe, uint8_t opcode, uint16_t cid, uint32_t nsid,
                     uint64_t slba, uint16_t nlb,
                     uint64_t data_addr, uint32_t rkey, uint32_t data_len) {
    std::memset(sqe, 0, 64);
    sqe[0] = opcode;
    sqe[1] = PSDT_SGL << 6;
    put_le16(sqe + 2, cid);
    put_le32(sqe + 4, nsid);
    sgl_keyed(sqe, data_addr, rkey, data_len);
    put_le64(sqe + 40, slba);               // cdw10-11
    put_le16(sqe + 48, nlb);                // cdw12 (0-based)
}

// ---- Identify structure field accessors (offsets from struct nvme_id_ctrl / nvme_id_ns) ----
constexpr size_t IDENTIFY_DATA_SIZE = 4096;

// Return a space/null-padded C string as a trimmed std::string.
inline std::string trim_field(const uint8_t* p, size_t len) {
    std::string s(reinterpret_cast<const char*>(p), len);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

// Set Features (admin command, no data for Number of Queues). fid -> cdw10, value -> cdw11.
inline void build_set_features(uint8_t* sqe, uint16_t cid, uint8_t fid, uint32_t value) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_SET_FEATURES;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    put_le16(sqe + 2, cid);
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;  // null keyed SGL
    put_le32(sqe + 40, fid);                // cdw10 = FID
    put_le32(sqe + 44, value);              // cdw11 = value
}

// Get Features (admin command). fid -> cdw10; result returned in CQE DW0.
inline void build_get_features(uint8_t* sqe, uint16_t cid, uint8_t fid) {
    std::memset(sqe, 0, 64);
    sqe[0] = OPC_GET_FEATURES;
    sqe[1] = PSDT_SGL << 6;                 // 0x40
    put_le16(sqe + 2, cid);
    sqe[39] = (SGL_TYPE_KEYED << 4) | SGL_SUBTYPE_ADDRESS;  // null keyed SGL
    put_le32(sqe + 40, fid);                // cdw10 = FID
}

// Number of Queues feature value encode/decode.
// Request: NCQR (bits 31:16) | NSQR (bits 15:0), both 1-based.
inline uint32_t nqr_request(uint16_t ncqr, uint16_t nsqr) { return ((uint32_t)ncqr << 16) | nsqr; }
// Allocated: result DW0 carries NCQA/NSQA as 0-based values.
inline uint16_t nqr_allocated(uint32_t result) { return (uint16_t)(result & 0xffff) + 1; }

// Identify Controller (struct nvme_id_ctrl)
inline std::string id_ctrl_mn(const uint8_t* d)     { return trim_field(d + 24, 40); }  // Model Number
inline std::string id_ctrl_sn(const uint8_t* d)     { return trim_field(d + 4, 20); }   // Serial Number
inline std::string id_ctrl_fr(const uint8_t* d)     { return trim_field(d + 64, 8); }   // Firmware Rev
inline uint8_t     id_ctrl_mdts(const uint8_t* d)   { return d[77]; }                    // Max Data Transfer Size
inline uint32_t    id_ctrl_nn(const uint8_t* d)     { return get_le32(d + 516); }        // Number of Namespaces

// Identify Namespace (struct nvme_id_ns)
inline uint64_t id_ns_nsze(const uint8_t* d) { return get_le64(d + 0); }   // total size (LBA)
inline uint64_t id_ns_ncap(const uint8_t* d) { return get_le64(d + 8); }   // capacity (LBA)
inline uint64_t id_ns_nuse(const uint8_t* d) { return get_le64(d + 16); }  // used (LBA)
inline uint8_t  id_ns_flbas(const uint8_t* d) { return d[26]; }            // formatted LBA size
inline uint8_t  id_ns_lbaf_ds(const uint8_t* d, int idx) { return d[128 + idx * 4 + 2]; }
// Block size in bytes for the currently formatted LBA format = 2^lbaf[flbas].ds
inline uint32_t id_ns_block_size(const uint8_t* d) {
    return 1u << id_ns_lbaf_ds(d, d[26] & 0xF);
}

} // namespace nvme
} // namespace nof
