# IB CM / RoCE 线格式（已逆向验证，实现 cm.cpp 的唯一依据）

## 1. 传输

### 1.1 端口模型（重点：源端口任意，目的端口恒为 4791）

CM 报文走 UDP，抓包实测：

```
REQ:  .20.<临时端口 55410>  →  .10:4791
REP:  .10.<55410 回显>      →  .20:4791   ← 回给 4791，不是回临时端口 55410
RTU:  .20.<55410>           →  .10:4791
```

- **源端口**：发起端任意（临时端口 55410），目标端把 REQ 源端口回显到 REP 的 src。
- **目的端口**：所有 CM 报文（REQ/REP/RTU，双向）恒为 **4791**。
- 原因：4791 是 RoCE CM 的 well-known 汇聚端口；真正的「服务」(如 NVMe-oF 的 4420) 编码在
  CM 报文内部的 `SERVICE_ID` 字段里，而不是用 UDP 目的端口区分。
- 推论：发起端**必须能收 4791**（收 REP），但内核 `rdma_cm` + `rxe`(`udp_tunnel`) 已 `bind` 4791，
  普通 UDP `bind()` 会 `EADDRINUSE`。因此用 **AF_PACKET** 在二层嗅探入站 4791 报文副本
  （不抢 bind，又能拿到 REP 字节）；发送也走 AF_PACKET 组完整以太网帧，控制每个字节（IP id=0 才能对上 ICRC）。

### 1.2 数据面
- 数据面走 rxe verbs（RC QP），与 CM 无关；数据包也走 UDP 4791（RoCEv2），由 rxe 的 `udp_tunnel` 处理，不来抢占普通 socket。

## 2. UDP payload 结构（280 字节）
```
[0..11]   BTH  (12B)
[12..19]  DETH (8B)
[20..275] MAD  (256B = 24B MAD头 + 232B 消息体)
[276..279] ICRC (4B)
```

## 3. BTH (12B) — GSI(QP1) 固定值
```
struct rxe_bth { u8 opcode; u8 flags; __be16 pkey; __be32 qpn; __be32 apsn; }
```
- opcode = 0x64 (UD SEND ONLY)
- pkey = 0xffff
- qpn (bytes 4-7) = dest QPN = 1 (GSI/QP1)，低 24 位是 QPN
- DETH (8B): Q_Key = 0x80010000, src QPN = 0, rsvd

## 4. MAD 头 (24B)  `struct ib_mad_hdr`
```
base_version=0x01, mgmt_class=0x07(CM), class_version=0x02, method=0x03(SEND),
status=0, class_specific=0, tid(8B 事务ID), attr_id(2B), resv(2B), attr_mod(4B)
```
attr_id: REQ=0x0010 REP=0x0013 RTU=0x0014 DREQ=0x0015 DREP=0x0016

## 5. 消息体字段偏移（相对 232B 消息体起点，即 MAD+24B 后）
来源：`include/rdma/ibta_vol1_c12.h`

### CM_REQ (Table 106)
| 字段 | 字节偏移 | 位宽 |
|------|---------|------|
| LOCAL_COMM_ID | 0 | 32 |
| VENDOR_ID | 5 | 24 |
| SERVICE_ID | 8 | 64 |
| LOCAL_CA_GUID | 16 | 64 |
| LOCAL_Q_KEY | 28 | 32 |
| LOCAL_QPN | 32 | 24 |
| RESPONDER_RESOURCES | 35 | 8 |
| LOCAL_EECN | 36 | 24 |
| INITIATOR_DEPTH | 39 | 8 |
| REMOTE_EECN | 40 | 24 |
| REMOTE_CM_RESPONSE_TIMEOUT | 43 | 5 |
| TRANSPORT_SERVICE_TYPE | 43 | 2(bit5) |
| END_TO_END_FLOW_CONTROL | 43 | 1(bit7) |
| STARTING_PSN | 44 | 24 |
| LOCAL_CM_RESPONSE_TIMEOUT | 47 | 5 |
| RETRY_COUNT | 47 | 3(bit5) |
| PARTITION_KEY | 48 | 16 |
| PATH_PACKET_PAYLOAD_MTU | 50 | 4 |
| RDC_EXISTS | 50 | 1(bit4) |
| RNR_RETRY_COUNT | 50 | 3(bit5) |
| MAX_CM_RETRIES | 51 | 4 |
| SRQ | 51 | 1(bit4) |
| EXTENDED_TRANSPORT_TYPE | 51 | 3(bit5) |
| PRIMARY_LOCAL_PORT_LID | 52 | 16 |
| PRIMARY_REMOTE_PORT_LID | 54 | 16 |
| PRIMARY_LOCAL_PORT_GID | 56 | 128 |
| PRIMARY_REMOTE_PORT_GID | 72 | 128 |
| PRIMARY_FLOW_LABEL | 88 | 20 |
| PRIMARY_PACKET_RATE | 91 | 6(bit2) |
| PRIMARY_TRAFFIC_CLASS | 92 | 8 |
| PRIMARY_HOP_LIMIT | 93 | 8 |
| PRIMARY_SL | 94 | 4 |
| PRIMARY_SUBNET_LOCAL | 94 | 1(bit4) |
| PRIMARY_LOCAL_ACK_TIMEOUT | 95 | 5 |
| (alternate 字段 96..139，RoCE 置 0) | | |
| PRIVATE_DATA | 140 | 736(92B) |

### CM_REP (Table 110)
| 字段 | 字节偏移 | 位宽 |
|------|---------|------|
| LOCAL_COMM_ID | 0 | 32 |
| REMOTE_COMM_ID | 4 | 32 |
| LOCAL_Q_KEY | 8 | 32 |
| LOCAL_QPN | 12 | 24 |
| STARTING_PSN | 20 | 24 |
| RESPONDER_RESOURCES | 24 | 8 |
| INITIATOR_DEPTH | 25 | 8 |
| TARGET_ACK_DELAY | 26 | 5 |
| RNR_RETRY_COUNT | 27 | 3 |
| LOCAL_CA_GUID | 28 | 64 |
| PRIVATE_DATA | 36 | 1568(196B) |

### CM_RTU (Table 111): LOCAL_COMM_ID@0, REMOTE_COMM_ID@4, PRIVATE_DATA@8(224B)
### CM_DREQ (Table 112): LOCAL_COMM_ID@0, REMOTE_COMM_ID@4, REMOTE_QPN_EECN@8(24), PRIVATE_DATA@12(220B)
### CM_DREP (Table 113): LOCAL_COMM_ID@0, REMOTE_COMM_ID@4, PRIVATE_DATA@8(224B)

## 6. ICRC 算法（已用抓包向量验证）
```
raw = CRC32_reflected(poly 0xEDB88320, 无首尾 XOR, 种子 0xDEBB20E3,
      [ mangled_IP(20) + mangled_UDP(8) + mangled_BTH(12) + DETH(8) + MAD(256) ])
ICRC = ~raw，按【小端】写入包尾 4 字节
```
mangling：IP tos=0xff, ttl=0xff, check=0xffff；UDP check=0xffff；BTH 的 qpn 字段高字节 |= 0xff。
注意：不能用 zlib.crc32()（它会做 ^0xFFFFFFFF），要自己写裸反射 CRC。

## 7. 私有数据
- REQ 私有数据(92B 内)填 `spdk_nvmf_rdma_request_private_data`(32B)：recfmt=0, qid, hrqsize, hsqsize, cntlid。
- REP 私有数据(196B 内)含 `spdk_nvmf_rdma_accept_private_data`(32B)：recfmt=0, crqsize。

## 8. 建链时序
```
发起端 .20                                  目标端 SPDK .10
  send CM_REQ(QPN/PSN/GID/私有数据) ──────────────>
  <──────────── CM_REP(QPN/PSN/GID/crqsize) ──────
  ibv_modify_qp: INIT→RTR→RTS
  send CM_RTU ──────────────────────────────────>
  (之后 post RECV, send Fabric Connect …)
```
- REQ 里 LOCAL_QPN/STARTING_PSN/PRIMARY_LOCAL_PORT_GID 填发起端自己的 QP 参数（GID = ::ffff:192.168.255.20 型 RoCEv2 GID）。
- REP 里取出目标端 LOCAL_QPN / STARTING_PSN / PRIMARY_LOCAL_PORT_GID，用于 ibv_modify_qp 的 RTR（dest_qpn/rq_psn/dgid）。
