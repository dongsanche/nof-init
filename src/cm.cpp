#include "cm.hpp"

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <memory>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.hpp"

namespace nof {

// ================= CRC =================
uint32_t crc32_raw(const void* data, size_t len, uint32_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1));
        }
    }
    return crc;
}

// ================= field helpers =================
void put_be16(void* p, uint16_t v) {
    uint8_t* b = static_cast<uint8_t*>(p);
    b[0] = (v >> 8) & 0xff; b[1] = v & 0xff;
}
void put_be24(void* p, uint32_t v) {
    uint8_t* b = static_cast<uint8_t*>(p);
    b[0] = (v >> 16) & 0xff; b[1] = (v >> 8) & 0xff; b[2] = v & 0xff;
}
void put_be32(void* p, uint32_t v) {
    uint8_t* b = static_cast<uint8_t*>(p);
    b[0] = (v >> 24) & 0xff; b[1] = (v >> 16) & 0xff; b[2] = (v >> 8) & 0xff; b[3] = v & 0xff;
}
void put_be64(void* p, uint64_t v) {
    uint8_t* b = static_cast<uint8_t*>(p);
    for (int i = 0; i < 8; i++) b[i] = (v >> (56 - 8 * i)) & 0xff;
}
uint16_t get_be16(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return (b[0] << 8) | b[1];
}
uint32_t get_be24(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return (b[0] << 16) | (b[1] << 8) | b[2];
}
uint32_t get_be32(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}
uint64_t get_be64(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

void put_le16(void* p, uint16_t v) { uint8_t* b = static_cast<uint8_t*>(p); b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; }
void put_le24(void* p, uint32_t v) { uint8_t* b = static_cast<uint8_t*>(p); b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; b[2] = (v >> 16) & 0xff; }
void put_le32(void* p, uint32_t v) { uint8_t* b = static_cast<uint8_t*>(p); b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; b[2] = (v >> 16) & 0xff; b[3] = (v >> 24) & 0xff; }
void put_le64(void* p, uint64_t v) { uint8_t* b = static_cast<uint8_t*>(p); for (int i = 0; i < 8; i++) b[i] = (v >> (8 * i)) & 0xff; }
uint16_t get_le16(const void* p) { const uint8_t* b = static_cast<const uint8_t*>(p); return b[0] | (b[1] << 8); }
uint32_t get_le24(const void* p) { const uint8_t* b = static_cast<const uint8_t*>(p); return b[0] | (b[1] << 8) | (b[2] << 16); }
uint32_t get_le32(const void* p) { const uint8_t* b = static_cast<const uint8_t*>(p); return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
uint64_t get_le64(const void* p) { const uint8_t* b = static_cast<const uint8_t*>(p); uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | b[i]; return v; }

// ================= ICRC =================
uint32_t cm_icrc(const uint8_t iph[20], const uint8_t udph[8],
                 const uint8_t* pkt, size_t pkt_len) {
    // The ICRC covers: mangled IPv4 header + mangled UDP header + mangled BTH,
    // then DETH + MAD. Mangling zeroes out "variant" fields (rxe_icrc_hdr).
    if (pkt_len < BTH_LEN) return 0;
    uint8_t ipm[20], udpm[8], bthm[BTH_LEN];
    std::memcpy(ipm, iph, 20);
    ipm[1] = 0xff;   // tos
    ipm[8] = 0xff;   // ttl
    ipm[10] = 0xff; ipm[11] = 0xff;  // ip checksum
    std::memcpy(udpm, udph, 8);
    udpm[6] = 0xff; udpm[7] = 0xff;  // udp checksum
    std::memcpy(bthm, pkt, BTH_LEN);
    bthm[4] |= 0xff;  // BTH qpn high byte (variant)

    uint32_t crc = 0xDEBB20E3;
    crc = crc32_raw(ipm, 20, crc);
    crc = crc32_raw(udpm, 8, crc);
    crc = crc32_raw(bthm, BTH_LEN, crc);
    crc = crc32_raw(pkt + BTH_LEN, pkt_len - BTH_LEN, crc);  // DETH + MAD
    return ~crc;
}

// ================= packet build =================
std::vector<uint8_t> build_cm_packet(uint16_t attr_id, uint64_t tid,
                                     const std::vector<uint8_t>& body) {
    std::vector<uint8_t> mad(MAD_LEN, 0);
    uint8_t* h = mad.data();
    h[0] = MAD_BASE_VER;
    h[1] = MAD_MGMT_CLASS;
    h[2] = MAD_CLASS_VER;
    h[3] = MAD_METHOD_SEND;
    put_be64(h + 8, tid);
    put_be16(h + 16, attr_id);

    if (!body.empty()) {
        std::memcpy(mad.data() + MAD_HDR_LEN, body.data(),
                    std::min(body.size(), MAD_BODY_LEN));
    }

    std::vector<uint8_t> pkt(CM_PACKET_LEN, 0);
    uint8_t* bth = pkt.data();
    bth[0] = CM_BTH_OPCODE;
    bth[1] = 0;
    put_be16(bth + 2, CM_BTH_PKEY);
    put_be32(bth + 4, 1);              // dest QPN = 1 (GSI)
    put_be32(bth + 8, 0x80000000u);    // apsn

    uint8_t* deth = pkt.data() + BTH_LEN;
    put_be32(deth, CM_DETH_QKEY);      // Q_Key
    put_be24(deth + 4, 0);             // src QPN
    deth[7] = 0;

    std::memcpy(pkt.data() + BTH_LEN + DETH_LEN, mad.data(), MAD_LEN);
    return pkt;
}

bool parse_cm_packet(const std::vector<uint8_t>& payload, ParsedMad& out) {
    if (payload.size() < CM_PACKET_LEN) return false;
    const uint8_t* mad = payload.data() + BTH_LEN + DETH_LEN;
    const uint8_t* h = mad;
    out.attr_id = get_be16(h + 16);
    out.tid = get_be64(h + 8);
    out.body.assign(mad + MAD_HDR_LEN, mad + MAD_LEN);
    return true;
}

// ================= CM disconnect (DREQ/DREP) =================
bool cm_disconnect(const std::string& ifname, const std::string& src_ip,
                   const std::string& dst_ip, uint32_t local_comm_id,
                   uint32_t remote_comm_id, uint32_t remote_qpn, uint64_t tid) {
    std::vector<uint8_t> body(MAD_BODY_LEN, 0);
    put_be32(body.data() + dreq_off::LOCAL_COMM_ID, local_comm_id);
    put_be32(body.data() + dreq_off::REMOTE_COMM_ID, remote_comm_id);
    put_be24(body.data() + dreq_off::REMOTE_QPN, remote_qpn);
    auto pkt = build_cm_packet(CM_DREQ_ATTR, tid, body);
    CmChannel ch;
    if (!ch.open(ifname, src_ip, dst_ip, 4791)) return false;
    if (!ch.send(pkt)) { ch.close(); return false; }
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> pl;
        if (ch.recv(pl, 200)) {
            ParsedMad mad;
            if (parse_cm_packet(pl, mad) && mad.attr_id == CM_DREP_ATTR) {
                ch.close();
                return true;
            }
        }
    }
    ch.close();
    return false;
}

// ================= CM channel (AF_PACKET send + sniff) =================
static bool get_if_mac(const std::string& ifname, uint8_t mac[6]) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) { ::close(fd); return false; }
    std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    ::close(fd);
    return true;
}

static bool get_dest_mac(const std::string& ip, uint8_t mac[6]) {
    FILE* f = ::fopen("/proc/net/arp", "r");
    if (!f) return false;
    char line[256], iface[32], ipstr[32], hw[32], flags[8];
    bool found = false;
    while (::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line, "%31s %*s %*s %31s %7s %31s", ipstr, hw, flags, iface) != 4)
            continue;
        if (ipstr == ip && hw != std::string("00:00:00:00:00:00")) {
            unsigned m[6];
            if (std::sscanf(hw, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
                found = true;
                break;
            }
        }
    }
    ::fclose(f);
    return found;
}

bool CmChannel::open(const std::string& ifname, const std::string& src_ip,
                     const std::string& dst_ip, uint16_t dst_port) {
    ifname_ = ifname;
    dst_ip_ = dst_ip;
    dst_port_ = dst_port;
    inet_pton(AF_INET, src_ip.c_str(), &src_addr_);
    inet_pton(AF_INET, dst_ip_.c_str(), &dst_addr_);

    pkt_fd_ = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (pkt_fd_ < 0) { ERRNO("af_packet socket"); return false; }
    struct sockaddr_ll sll;
    std::memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = if_nametoindex(ifname.c_str());
    if (sll.sll_ifindex == 0) { ERR("if_nametoindex %s", strerror(errno)); return false; }
    if (::bind(pkt_fd_, (struct sockaddr*)&sll, sizeof(sll)) < 0) { ERRNO("af_packet bind"); return false; }

    struct timeval tv{0, 100000};  // 100ms per-recv timeout
    ::setsockopt(pkt_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!get_if_mac(ifname, src_mac_)) { ERR("get_if_mac"); return false; }
    if (!get_dest_mac(dst_ip, dst_mac_)) {
        ERR("no ARP entry for %s; ping it first", dst_ip.c_str());
        return false;
    }

    LOG("cm channel: if=%s src_mac=%02x:%02x:%02x:%02x:%02x:%02x dst=%s (%02x:%02x:%02x:%02x:%02x:%02x):%u",
        ifname.c_str(), src_mac_[0], src_mac_[1], src_mac_[2], src_mac_[3], src_mac_[4], src_mac_[5],
        dst_ip.c_str(), dst_mac_[0], dst_mac_[1], dst_mac_[2], dst_mac_[3], dst_mac_[4], dst_mac_[5], dst_port);
    return true;
}

void CmChannel::close() {
    if (pkt_fd_ >= 0) ::close(pkt_fd_);
    pkt_fd_ = -1;
}

// standard IPv4 header checksum (1's complement)
static uint16_t ip_checksum(const uint8_t* iph, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2) {
        sum += (iph[i] << 8) + iph[i + 1];
    }
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

bool CmChannel::send(const std::vector<uint8_t>& payload) {
    if (payload.size() != CM_PACKET_LEN) return false;

    // full IPv4 header (id=0 so ICRC matches; we control every byte)
    uint8_t iph[20] = {0};
    iph[0] = 0x45;
    put_be16(iph + 2, 20 + 8 + CM_PACKET_LEN);
    iph[8] = 0x40;                    // ttl
    iph[9] = IPPROTO_UDP;
    std::memcpy(iph + 12, &src_addr_.s_addr, 4);
    std::memcpy(iph + 16, &dst_addr_.s_addr, 4);
    put_be16(iph + 10, ip_checksum(iph, 20));

    uint8_t udph[8] = {0};
    put_be16(udph + 2, dst_port_);
    put_be16(udph + 4, 8 + CM_PACKET_LEN);
    // udph checksum = 0 (no checksum, matches captured kernel initiator)

    // copy payload and fill ICRC
    std::vector<uint8_t> buf = payload;
    uint32_t icrc = cm_icrc(iph, udph, buf.data(), CM_PACKET_LEN - ICRC_LEN);
    uint8_t* o = buf.data() + CM_PACKET_LEN - ICRC_LEN;
    o[0] = icrc & 0xff; o[1] = (icrc >> 8) & 0xff; o[2] = (icrc >> 16) & 0xff; o[3] = (icrc >> 24) & 0xff;

    // full Ethernet frame: eth(14) + ip(20) + udp(8) + payload(280)
    std::vector<uint8_t> frame(14 + 20 + 8 + CM_PACKET_LEN);
    uint8_t* eth = frame.data();
    std::memcpy(eth, dst_mac_, 6);
    std::memcpy(eth + 6, src_mac_, 6);
    put_be16(eth + 12, ETHERTYPE_IP);
    std::memcpy(eth + 14, iph, 20);
    std::memcpy(eth + 34, udph, 8);
    std::memcpy(eth + 42, buf.data(), CM_PACKET_LEN);

    struct sockaddr_ll dst_ll;
    std::memset(&dst_ll, 0, sizeof(dst_ll));
    dst_ll.sll_family = AF_PACKET;
    dst_ll.sll_protocol = htons(ETH_P_ALL);
    dst_ll.sll_ifindex = if_nametoindex(ifname_.c_str());
    dst_ll.sll_halen = 6;
    std::memcpy(dst_ll.sll_addr, dst_mac_, 6);

    ssize_t n = ::sendto(pkt_fd_, frame.data(), frame.size(), 0,
                         (struct sockaddr*)&dst_ll, sizeof(dst_ll));
    if (n < 0) { ERRNO("sendto"); return false; }
    return n == (ssize_t)frame.size();
}

bool CmChannel::recv(std::vector<uint8_t>& payload, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t frame[65536];
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::recvfrom(pkt_fd_, frame, sizeof(frame), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (n < 14 + 20 + 8) continue;
        struct ether_header* eth = (struct ether_header*)frame;
        if (ntohs(eth->ether_type) != ETHERTYPE_IP) continue;
        struct iphdr* ip = (struct iphdr*)(frame + 14);
        int ihlen = ip->ihl * 4;
        if (ip->protocol != IPPROTO_UDP) continue;
        if (ip->saddr != dst_addr_.s_addr) continue;   // only from target
        struct udphdr* udp = (struct udphdr*)(frame + 14 + ihlen);
        if (ntohs(udp->dest) != dst_port_) continue;
        int udplen = ntohs(udp->len);
        if (udplen < 8) continue;  // malformed UDP header, avoid underflow
        int plen = udplen - 8;
        const uint8_t* p = frame + 14 + ihlen + 8;
        payload.assign(p, p + plen);
        return true;
    }
    return false;
}

} // namespace nof