#pragma once

#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <cstdint>
#include <string>
#include <vector>

namespace nof {

// ---- RoCE/CM transport constants (verified) ----
constexpr uint8_t  CM_BTH_OPCODE   = 0x64;   // UD SEND ONLY
constexpr uint16_t CM_BTH_PKEY     = 0xffff;
constexpr uint32_t CM_DETH_QKEY    = 0x80010000;

// MAD header
constexpr uint8_t  MAD_BASE_VER    = 0x01;
constexpr uint8_t  MAD_MGMT_CLASS  = 0x07;   // CM
constexpr uint8_t  MAD_CLASS_VER   = 0x02;
constexpr uint8_t  MAD_METHOD_SEND = 0x03;

// attr ids
constexpr uint16_t CM_REQ_ATTR  = 0x0010;
constexpr uint16_t CM_REP_ATTR  = 0x0013;
constexpr uint16_t CM_RTU_ATTR  = 0x0014;
constexpr uint16_t CM_DREQ_ATTR = 0x0015;
constexpr uint16_t CM_DREP_ATTR = 0x0016;

constexpr size_t MAD_HDR_LEN   = 24;
constexpr size_t MAD_BODY_LEN  = 232;
constexpr size_t MAD_LEN       = MAD_HDR_LEN + MAD_BODY_LEN;  // 256
constexpr size_t BTH_LEN       = 12;
constexpr size_t DETH_LEN      = 8;
constexpr size_t ICRC_LEN      = 4;
constexpr size_t CM_PACKET_LEN = BTH_LEN + DETH_LEN + MAD_LEN + ICRC_LEN; // 280

// CM message body field offsets (relative to MAD body start = after 24B header)
// from include/rdma/ibta_vol1_c12.h
namespace req_off {
constexpr int LOCAL_COMM_ID        = 0;
constexpr int SERVICE_ID           = 8;
constexpr int LOCAL_CA_GUID        = 16;
constexpr int LOCAL_Q_KEY          = 28;
constexpr int LOCAL_QPN            = 32;
constexpr int RESPONDER_RESOURCES  = 35;
constexpr int INITIATOR_DEPTH      = 39;
constexpr int STARTING_PSN         = 44;
constexpr int RETRY_COUNT          = 47; // 3 bits @ bit5
constexpr int PARTITION_KEY        = 48;
constexpr int PATH_MTU             = 50; // 4 bits
constexpr int RNR_RETRY_COUNT      = 50; // 3 bits @ bit5
constexpr int PRIMARY_LOCAL_GID    = 56;
constexpr int PRIMARY_REMOTE_GID   = 72;
constexpr int PRIMARY_TRAFFIC_CLASS= 92;
constexpr int PRIMARY_HOP_LIMIT    = 93;
constexpr int PRIMARY_SL           = 94;
constexpr int PRIVATE_DATA         = 140;
}

namespace rep_off {
constexpr int LOCAL_COMM_ID   = 0;
constexpr int REMOTE_COMM_ID  = 4;
constexpr int LOCAL_Q_KEY     = 8;
constexpr int LOCAL_QPN       = 12;
constexpr int STARTING_PSN    = 20;
constexpr int RESPONDER_RESOURCES = 24;
constexpr int INITIATOR_DEPTH = 25;
constexpr int LOCAL_CA_GUID   = 28;
constexpr int PRIVATE_DATA    = 36;
}

namespace rtu_off {
constexpr int LOCAL_COMM_ID  = 0;
constexpr int REMOTE_COMM_ID = 4;
constexpr int PRIVATE_DATA   = 8;
}

namespace dreq_off {
constexpr int LOCAL_COMM_ID  = 0;
constexpr int REMOTE_COMM_ID = 4;
constexpr int REMOTE_QPN     = 8;   // 24 bits (Remote QPN/EECN)
constexpr int PRIVATE_DATA   = 12;
}

namespace drep_off {
constexpr int LOCAL_COMM_ID  = 0;
constexpr int REMOTE_COMM_ID = 4;
constexpr int PRIVATE_DATA   = 8;
}

// ---- SPDK private data (32B), from include/spdk/nvmf_spec.h ----
struct RdmaRequestPrivateData {
    uint16_t recfmt;   // 0
    uint16_t qid;      // admin=0, io=allocated
    uint16_t hrqsize;  // host recv queue size
    uint16_t hsqsize;  // host send queue size
    uint16_t cntlid;
    uint8_t  reserved[22];
}; // 32 bytes

struct RdmaAcceptPrivateData {
    uint16_t recfmt;   // 0
    uint16_t crqsize;  // controller recv queue size
    uint8_t  reserved[28];
}; // 32 bytes

// ---- CRC ----
uint32_t crc32_raw(const void* data, size_t len, uint32_t seed = 0);

// Compute the RDMA CM Invariant CRC (ICRC). Input: the *unmangled* IPv4 header (20B),
// UDP header (8B), and the CM packet bytes (BTH+DETH+MAD = CM_PACKET_LEN - ICRC_LEN bytes).
// Returns the 32-bit ICRC value to be stored LITTLE-ENDIAN in the last 4 bytes of the packet.
uint32_t cm_icrc(const uint8_t iph[20], const uint8_t udph[8],
                 const uint8_t* pkt, size_t pkt_len);

// ---- packet build/parse ----
// Build a full CM UDP payload (BTH+DETH+MAD+ICRC) for the given attr/message body.
// `body` is the 232-byte MAD message body (caller fills via helpers below).
std::vector<uint8_t> build_cm_packet(uint16_t attr_id, uint64_t tid,
                                     const std::vector<uint8_t>& body);

// Parse an inbound CM packet payload (280B). Fills attr_id, tid, and returns the
// 232-byte body. Returns false on bad size/magic.
struct ParsedMad {
    uint16_t attr_id = 0;
    uint64_t tid = 0;
    std::vector<uint8_t> body;
};
bool parse_cm_packet(const std::vector<uint8_t>& payload, ParsedMad& out);

// Field get/set helpers (network byte order)
void put_be16(void* p, uint16_t v);
void put_be24(void* p, uint32_t v);   // low 24 bits
void put_be32(void* p, uint32_t v);
void put_be64(void* p, uint64_t v);
uint16_t get_be16(const void* p);
uint32_t get_be24(const void* p);
uint32_t get_be32(const void* p);
uint64_t get_be64(const void* p);

// Field get/set helpers (little-endian, used by NVMe SQE/CQE)
void put_le16(void* p, uint16_t v);
void put_le24(void* p, uint32_t v);
void put_le32(void* p, uint32_t v);
void put_le64(void* p, uint64_t v);
uint16_t get_le16(const void* p);
uint32_t get_le24(const void* p);
uint32_t get_le32(const void* p);
uint64_t get_le64(const void* p);

// Send a CM Disconnect Request (DREQ) and wait (best-effort) for the DREP.
// Returns true if the DREP was received. Must be called while the CM connection
// is still established (comm ids are still valid on the target).
bool cm_disconnect(const std::string& ifname, const std::string& src_ip,
                   const std::string& dst_ip, uint32_t local_comm_id,
                   uint32_t remote_comm_id, uint32_t remote_qpn, uint64_t tid);

// ---- CM channel (UDP send + AF_PACKET sniff) ----
class CmChannel {
public:
    bool open(const std::string& ifname, const std::string& src_ip,
              const std::string& dst_ip, uint16_t dst_port);
    void close();
    // send raw UDP payload (the 280B CM packet) to dst_ip:dst_port
    bool send(const std::vector<uint8_t>& payload);
    // sniff inbound UDP dst_port packet, return its UDP payload; timeout_ms
    bool recv(std::vector<uint8_t>& payload, int timeout_ms);

private:
    int pkt_fd_ = -1;
    std::string ifname_;
    std::string dst_ip_;
    uint16_t dst_port_ = 0;
    struct in_addr src_addr_{};
    struct in_addr dst_addr_{};
    uint8_t src_mac_[6] = {0};
    uint8_t dst_mac_[6] = {0};
};

} // namespace nof
