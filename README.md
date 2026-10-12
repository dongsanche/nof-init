# nof-init — 自研 NVMe-oF/RDMA 发起端（不依赖 librdmacm）

从零手写、只用裸 `libibverbs` 的 NVMe-oF/RDMA 发起端，对端为 SPDK `nvmf_tgt`。
建链使用自研 IB CM 线协议（REQ/REP/RTU + 自算 ICRC），完全不用 `rdma_cm`。

## 目录结构

```
nof-init/
├── src/
│   ├── cm.hpp / cm.cpp       CM 线协议 + 字段 helper + CmChannel
│   ├── verbs.hpp / verbs.cpp 裸 verbs 封装 + QP INIT→RTR→RTS
│   ├── nvme.hpp              NVMe-oF 命令/SGL 构造（纯函数）
│   ├── initiator.hpp         共享发起端连接 helper（建链/Connect/CC.EN/拆除）
│   └── common.hpp            日志宏
├── test/
│   ├── unit_test.cpp         单元测试（无需真实目标端）
│   ├── robust_test.cpp       健壮性测试（corner case + 压力，无需真实目标端）
│   ├── stress_init.cpp       集成压力测试（需真实目标端：并发/大块/整盘 CRC/多轮）
│   ├── lifecycle_test.cpp    重连生命周期测试（需目标端：反复 connect/teardown）
│   ├── keepalive_test.cpp    Keep Alive + AER 测试（需目标端）
│   ├── multi_queue_test.cpp  多 I/O 队列测试（需目标端：Set/Get Features + 4 队列并发）
│   ├── bench.cpp             吞吐对照基准（需目标端：1 队列 vs 4 队列 IOPS/带宽）
│   ├── abort_shutdown_test.cpp Abort + 优雅关机测试（需目标端）
│   ├── reconnect_test.cpp    目标端 kill 后自动重连测试（需目标端）
│   ├── nof_init.cpp          完整发起端（admin + IO + 读写，验收程序）
│   ├── m1_cm.cpp             里程碑：CM 建链 + Connect + Identify
│   └── m0_selfpair.cpp       里程碑：裸 verbs 自配对 QP 建链
├── ref/
│   ├── cm_wire_format.md     CM 线格式逆向笔记
│   ├── ibta_vol1_c12.h       内核 CM 字段定义（参考）
│   └── cm.c / cma.c          内核 rdma_cm/ib_cm 源码（参考）
└── Makefile
```

## 功能分解（可测试单元）

| 单元 | 位置 | 说明 | 是否纯函数 |
|------|------|------|-----------|
| `crc32_raw(data, len, seed)` | `cm.cpp` | 裸反射 CRC-32（poly 0xEDB88320，无首尾 XOR）| ✅ |
| `cm_icrc(iph, udph, pkt, len)` | `cm.cpp` | CM 报文 ICRC（种子 0xDEBB20E3，掩码变体字段，取反）| ✅ |
| `put_be16/24/32/64` / `get_be16/24/32/64` | `cm.cpp` | 大端字段读写 | ✅ |
| `put_le16/24/32/64` / `get_le16/24/32/64` | `cm.cpp` | 小端字段读写（NVMe SQE/CQE 用）| ✅ |
| `build_cm_packet(attr, tid, body)` | `cm.cpp` | 组 BTH+DETH+MAD（不含 ICRC）| ✅ |
| `parse_cm_packet(payload, out)` | `cm.cpp` | 解析 CM 报文，取 attr/tid/body | ✅ |
| `sgl_data_block(sqe, len)` | `nvme.hpp` | Data Block SGL（内嵌数据）| ✅ |
| `sgl_keyed(sqe, addr, rkey, len)` | `nvme.hpp` | Keyed SGL（RDMA 数据缓冲）| ✅ |
| `build_connect_data` / `build_connect_sqe` | `nvme.hpp` | Fabric Connect 数据 + SQE（含 `kato`） | ✅ |
| `build_prop_set(sqe, ofst, value)` | `nvme.hpp` | Property Set 命令（null SGL）| ✅ |
| `build_keep_alive` / `build_async_event` | `nvme.hpp` | Keep Alive / AER 命令（null SGL）| ✅ |
| `build_set/get_features` + `nqr_*` | `nvme.hpp` | Features 命令 + Number of Queues 编解码 | ✅ |
| `build_identify` + `id_ctrl_*` / `id_ns_*` | `nvme.hpp` | Identify 命令 + 结构字段解析 | ✅ |
| `build_rw(sqe, ...)` | `nvme.hpp` | I/O Read/Write 命令（带 Keyed SGL）| ✅ |
| `cm_disconnect(...)` | `cm.cpp` | CM DREQ/DREP 拆除 | ⚠️ 需网络 |
| `ib_bring_qp_rts(params)` | `verbs.cpp` | QP INIT→RTR→RTS 状态机 | ⚠️ 需 verbs 设备 |
| `CmChannel::open/send/recv` | `cm.cpp` | CM 通道（AF_PACKET 收发 4791）| ⚠️ 需网络 |

> 标注 ✅ 的函数为纯逻辑，无需真实目标端即可单元测试。

## 单元测试

`test/unit_test.cpp`（**87 个断言全部通过**，纯函数；另 `robust_test` 有 759118 项 corner/压力断言），覆盖：

- `test_crc32`：CRC-32 裸反射算法已知向量。
- `test_icrc`：用真实抓包向量（内核发起端的 CM REQ）验证 `cm_icrc` 输出 `0xb3b02fbd`（LE 存储后为 `bd 2f b0 b3`）。
- `test_field_helpers`：大端/小端字段读写 round-trip 与字节序交叉验证。
- `test_cm_packet_roundtrip`：`build_cm_packet`/`parse_cm_packet` 往返一致性 + 坏包拒绝。
- `test_sgl_builders`：Keyed/Data Block SGL 的精确字节布局。
- `test_connect_capsule`：Fabric Connect capsule 的关键字段偏移。
- `test_prop_set`：Property Set 字节（含 null Keyed SGL）。
- `test_keepalive_aer_builders`：Keep Alive / AER / Connect `kato` 字段布局。
- `test_identify_parsers`：Identify Controller/Namespace 字段解析 + 块大小计算。
- `test_set_get_features`：Set/Get Features 字节 + NCQR/NSQR 编解码。

`test/robust_test.cpp`（759118 项）覆盖 corner case + 压力：CRC 微分 fuzz、字段穷举/随机往返、报文
fuzz、SGL/Connect/PropSet/RW 边界、builder 自净（脏缓冲区不残留）、ICRC 短报文守卫等。

运行：
```bash
cd ~/nof-init && make && ./unit_test && ./robust_test
```

## 构建与运行

```bash
make                    # 编译所有目标
./unit_test             # 单元测试（无需目标端）
./robust_test           # 健壮性测试（corner case + 压力，无需目标端）
sudo ./nof_init         # 完整发起端（需 .10 目标端 + soft-RoCE）
sudo ./stress_init      # 集成压力测试（需目标端）
sudo ./lifecycle_test   # 重连生命周期测试（需目标端）
sudo ./keepalive_test   # Keep Alive + AER 测试（需目标端）
sudo ./multi_queue_test # 多 I/O 队列测试（需目标端）
sudo ./bench           # 吞吐对照基准（需目标端）
sudo ./abort_shutdown_test # Abort + 优雅关机测试（需目标端）
sudo ./reconnect_test     # 目标端 kill 后自动重连测试（需目标端）
```

完整流程（`nof_init`）：
1. admin CM 建链（qid=0）→ Fabric Connect → Property Set(CC.EN)
2. Identify Controller + Namespace（拿 MN/SN/FR/MDTS/NN、nsze、块大小）
3. IO CM 建链（qid=1）→ Fabric Connect
4. 按 Identify 得到的块大小 Write + Read，校验数据一致性

### 集成压力测试（`stress_init`）

| 场景 | 覆盖 |
|------|------|
| pipeline | 灌满 QD（32）并发写、并发读，按 cmd id 分派 CQE |
| large | 单命令 256 KiB 写 + 读（64 LBA） |
| sweep | 256 块（1 MiB）写后读回，整盘 CRC32 比对 |
| loops | 上述场景重复 20 轮（累计 1832 条命令）探测抖动/泄漏 |

### 重连生命周期测试（`lifecycle_test`）

反复 `connect → CC.EN → I/O → teardown → reconnect` 共 10 轮，验证：

- **连接拆除**已实现：`cm_disconnect()` 发 CM DREQ 并等 DREP，`ib_mark_qp_error()` 把 QP 置 ERR，
  再销毁 QP/CQ（见 `src/cm.cpp` / `src/initiator.hpp::teardown_conn`）。
- 目标端控制器被**彻底回收**（每轮 `cntlid=1` 被复用），因此**同一目标端可连续多轮运行，
  无需再 `reset_nvmet.sh`**。

### Keep Alive + AER（`keepalive_test`）

| 场景 | 结果 |
|------|------|
| positive | KATO=2000ms，空闲 6s（3×KATO）期间每 500ms 发 Keep Alive → 12/12 成功，控制器存活 |
| AER | 投递 Async Event Request → 目标端 parked（300ms 内无完成），证明 AER 被接受 |
| negative | KATO=1000ms，空闲 2.5s 不发 Keep Alive → 控制器被目标端 fatal error 拆除 |

关键语义（内核实现）：

- Connect 命令的 `kato` 字段（SQE 字节 48–51）单位是**毫秒**（内核 `cmd.connect.kato = ctrl->kato * 1000`），
  目标端 `ctrl->kato = DIV_ROUND_UP(kato, 1000)`（秒）。`kato=0` 表示禁用 keep-alive。
- Keep Alive（opcode `0x18`）与 AER（opcode `0x0c`）都是**无数据 admin 命令**，同样需要
  `PSDT=SGL(0x40)` + **null Keyed SGL**（`sqe[39]=0x40, len=0`）。
- AER 是「parked」命令：目标端挂起直到异步事件发生才完成；超上限返回 `ASYNC_LIMIT`。
  当前只验证了投递/parked，事件触发→完成的闭环（需目标端 configfs 切换 namespace）未自动化。

### Set/Get Features + 多 I/O 队列（`multi_queue_test`）

- `build_set_features` / `build_get_features`（opcode `0x09`/`0x0a`，FID=`0x07`，均 null Keyed SGL）。
- `set_num_queues` / `get_num_queues`：Set 用 cdw11 报 `NCQR<<16 | NSQR`（1-based），
  Get 从 CQE DW0 读结果。nvmet 固定回 `max_qid=128`（不按请求实际收敛）。
- `multi_queue_test`：Set/Get Features → 建 4 条 IO 队列（qid=1..4）→ 每队列写读 + 全队列并发写读回校验，
  数据一致。目标端 `NVMET_NR_QUEUES=128`，qid 需 `<= max_qid`。

### 吞吐对照（`bench`）：多队列在 soft-RoCE 上无并行收益

`bench` 固定总 in-flight=32（1q×32 vs 4q×8），三组实测：

| 场景 | 1 队列 | 4 队列 | speedup |
|------|--------|--------|---------|
| 4 KiB 读 | ~950 IOPS | ~980 IOPS | ≈1.0x |
| 64 KiB 读 | ~7.5 MB/s | ~5.7 MB/s | 0.5–1.1x（噪声大） |

**结论**：soft-RoCE（`rdma_rxe`）是纯软件实现，per-device 报文处理（软件 CRC/状态机）在单 tasklet 上
串行执行，是瓶颈所在；**加队列不带来吞吐提升**（甚至略降）。多 I/O 队列的正确性（多 QP 并发 in-flight、
数据一致）已由 `multi_queue_test` 验证，但性能收益要等真实 RDMA 硬件（多硬件队列上下文）才能体现。

> 调试 `bench` 时发现并修复一个真问题：并发读会触发目标端 `SEND ... remote operation error`（我方
> RQ 空导致 NAK）。根因是 recv 投递方式——改为「先投全部 recv，再投全部 send」（且每 SQE 独立 MR），
> 与 `stress_init::io_batch` 一致后解决。顺带在 `fabric_connect`/`admin_submit_sync` 末尾加了 `drain_cq`
> 清残留 SEND 完成，并把 CQ 容量从 `qd*2` 提到 `qd*2+16`。

### Abort + 优雅关机 + 自动重连（`abort_shutdown_test` / `reconnect_test`）

| 能力 | 实现 | 验证 |
|------|------|------|
| Abort | `build_abort`（opcode `0x08`，cdw10=`SQID<<16\|CID`，null SGL）+ `abort_cmd` | nvmet 为 no-op，回 `result bit0=1` |
| 优雅关机 | `shutdown_controller`：`CC.SHN=01`(bit14) → 轮询 `CSTS.SHST` → `CC.EN=0` | CSTS 从 `0x1`(RDY) 变 `0x8`(SHST=complete) |
| Property Get | `build_prop_get`（fctype `0x04`）+ `prop_get`（读 CQE DW0） | 读 CSTS/CC 验证 |
| 自动重连 | `connect_admin_with_retry`：短 CM 超时(3s) + 循环重试 + 退避 | `reconnect_test` 三阶段：连→杀目标→重连→I/O 验证 |
| CQ 错误处理 | `wc_status_name` / `wc_is_fatal`（verbs）+ `admin_submit_sync` 错误日志 | 单元测试覆盖 |

关键点：

- `establish_conn` 增加 `rep_timeout_ms` 参数（默认 50s），自动重连用 3s 短超时，避免死目标等待过久。
- 目标端「杀」需先清 configfs 再 `modprobe -r nvmet_rdma nvmet`（`nvmet_down.sh`），否则 configfs 端口
  引用导致卸载失败；重启用 `reset_nvmet.sh`。
- 重连循环带**时间戳 + 尝试计数 + 失败阶段**日志。实测（目标端 `nvmet_down` 后重启）：
  ```
  reconnect attempt 1: FAIL at 'establish' at t=3.151s   # 目标宕机，3s 短超时失败
  reconnect attempt 2: FAIL at 'establish' at t=8.915s
  reconnect attempt 3: OK (cntlid=1) at t=9.983s         # 目标恢复，重连成功
  ```

## 能力差距与待办（vs SPDK 原生 initiator）

| 能力 | 现状 |
|------|------|
| CM 建链/拆除、Fabric Connect、CC.EN、单队列读写 | ✅ 已实现 |
| Property Set | ⚠️ 仅 CC.EN（无 Property Get / 其它寄存器） |
| Identify 流程 | ✅ 已实现（`nof_init` 跑 Identify Controller + Namespace，块大小由 `lbaf[0].ds` 动态获取） |
| **Keep Alive（KATO）** | ✅ 已实现（`build_keep_alive` + KATO 协商 + 定时发送） |
| **AER（Async Event Request）** | ⚠️ 已实现投递（`build_async_event`，目标端 parked 验证过），事件触发→完成的闭环未自动化 |
| Set/Get Features（Number of Queues） | ✅ 已实现（`build_set/get_features` + `set_num_queues`/`get_num_queues`） |
| 多 I/O 队列 | ✅ 已实现（`multi_queue_test` 建 4 条，并发读写） |
| Abort | ✅ 已实现（`build_abort` + `abort_cmd`；nvmet 为 no-op，always success） |
| 优雅关机（CC.SHN / Property Get） | ✅ 已实现（`shutdown_controller`：CC.SHN → 轮询 CSTS.SHST → CC.EN=0） |
| 错误恢复 / 目标端 kill 自动重连 | ✅ 已实现（`connect_admin_with_retry` + 短 CM 超时，`reconnect_test` 验证） |
| CQ 错误处理 | ✅ 已实现（`wc_status_name` / `wc_is_fatal` + 错误日志） |
| Discovery / ANA 多路径 | ❌ |

### 下一步建议（按优先级）

1. **AER 事件→完成闭环自动化**：目标端 configfs 切换 namespace 触发 `NS_CHANGED` 事件，验证 AER
   真的完成（当前只验证了 parked）。
2. **Discovery / ANA 多路径**：发现服务（Discovery NQN）+ 多端口/命名空间访问。
3. **（可选）真正的 keep-alive 后台线程**：目前 keep-alive 是测试里主动循环发的，真实 initiator
   会起后台线程按 `KATO/2` 周期发，并在 CQ 错误时联动自动重连。

### 工程化收尾（非功能，但建议做）

- **去重**：`io_batch`/`io_one` 目前在 `stress_init`/`multi_queue`/`bench`/`lifecycle` 里各复制一份，
  应收拢到 `initiator.hpp`。
- **参数化**：`SRC_IP`/`DST_IP`/`SUBNQN` 等在 `initiator.hpp` 里是 `static` 常量，可改成命令行参数。
- **里程碑程序**：`m0_selfpair.cpp`/`m1_cm.cpp` 是历史里程碑，主流程已被 `nof_init` 取代，可归档。
- **`ref/cm_wire_format.md`**：补 DREQ/DREP 字段布局（本次拆除时逆向的）。

## 已修复的 bug

1. **硬编码源 IP**：`cm.cpp`/`nof_init.cpp` 中写死的 `192.168.255.20` → 改为 `CmChannel::open(ifname, src_ip, dst_ip, port)` 参数。
2. **debug 残留**：`cm.cpp` 无条件写 `/tmp/nof_pkt.bin` → 移除。
3. **`LOCAL_CA_GUID` 恒为 0**：`establish_conn` 未填 CA GUID → 改为传 `dev_attr.sys_image_guid`。
4. **类型收窄警告**：`rsp.size()`（size_t）→ `ibv_sge.length`（uint32_t）隐式收窄 → 显式 `(uint32_t)`。
5. **小端 helper 重复**：`nof_init.cpp`/`m1_cm.cpp` 各自复制 `put_le*` → 统一收拢到 `cm.hpp`。

## 踩坑记录（协议层关键点，详见 `ref/cm_wire_format.md`）

- **CM 走 UDP 4791**，报文 = `BTH(12)+DETH(8)+MAD(256)+ICRC(4)`，双向 dst 都是 4791。
- **ICRC** 覆盖 [掩码后的 IP+UDP+BTH] + DETH + MAD，种子 `0xDEBB20E3`，结果取反后**小端**存储；
  且必须用 **AF_PACKET 发完整以太帧**（否则内核重写 IP `id` 导致 ICRC 对不上）。
- **PSN 换位**：内核 rdma_cm 把 QP 的 `sq_psn`/`rq_psn` 反着设（`cma.c:1124`、`cm.c:2093`），
  发起端也必须反着设才能匹配。
- **NVMe-oF 不允许 Create/Delete I/O CQ/SQ**，IO 队列完全靠 RDMA CM + Fabric Connect 建立。
- Connect 后需先 **Property Set CC.EN**（`0x00460001`）使能控制器，否则 Identify 报 disabled。
- **无数据 fabrics 命令也要带 null SGL**：`sqe[39]` 必须是 `0x40`（Keyed Data Block、len=0），
  不能留 0（0 会被内核 `nvmet_rdma_map_sgl` 当作非法 SGL 子类型 → `INVALID_FIELD`）。

## 内核 nvmet 目标端验证（SPDK vs 内核 nvmet 差异）

发起端跑通 SPDK 后，又对着**内核 nvmet**（`nvmet_rdma` + configfs）做了交叉验证。结论：
**线协议兼容，但发起端并非"零改动"** —— 内核 nvmet 更严格，暴露了 4 个被 SPDK 宽松逻辑掩盖的真 bug。

### 已修复的 4 个 bug

| # | bug | 根因（为什么 SPDK 没暴露） | 修复 |
|---|-----|---------------------------|------|
| 1 | CM 私有数据字节序 | 我用了 `put_be16`(大端)，内核读 `le16_to_cpu`(小端)；SPDK 用 `min()` 协商，把大端读成的大数被硬件上限截断，碰巧没炸 | `put_be16 → put_le16` |
| 2 | admin 队列深度超限 | `hsqsize=64` → `recv_queue_size=65 > NVME_AQ_DEPTH=32`，被 `INVALID_HSQSIZE` 拒；SPDK 无此上限 | QD 降到 32，`hsqsize=qd-1` |
| 3 | Connect 数据内嵌 | 内核 nvmet admin RQ 只有 64B（`num_sge = admin ? 1 : ...`），SPDK 是 64+4096；1024B connect data 必须走 **Keyed SGL**（目标端 RDMA READ） | Connect 改用 `build_connect_data` + `build_connect_sqe`(Keyed SGL) |
| 4 | Property Set 缺 null SGL | 无数据 fabrics 命令的 SGL 描述符字节（`sqe[39]`）留成了 0（Data Block + ADDRESS 子类型），被内核 `nvmet_rdma_map_sgl` 判为非法 SGL 子类型 → `INVALID_FIELD`；SPDK 不检查该字节 | `build_prop_set` 补 `sqe[39]=0x40`（null Keyed SGL，len=0，等价于内核发起端 `nvme_rdma_set_sg_null`） |

### Property Set INVALID_FIELD 定位过程（已解决）

- 现象：命令逐字节正确（`opc=0x7f fctype=0x00 attrib=0 ofst=0x14 value=0x00460001`，
  `offsetof(attrib)=40, offset=44, value=48` 也实测无误），但内核仍返回 `INVALID_FIELD`。
- 用 `ftrace`（`nvmet_req_init` / `nvmet_req_complete`）确认命令解析无误，且 CQE 解码为
  `raw_status = trace_status - 0x2000 = 0x4002 = INVALID_FIELD|DNR`（`nvmet_set_error`
  写 `cqe->status = (raw<<1) | (1<<14)`，追踪点再 `>>1`，故相差 `0x2000`）。
- 用 `objdump -d nvmet.ko` 反汇编实际运行的内核模块，确认 `nvmet_execute_prop_set`
  逻辑与主线一致（只在 `attrib&1` / `offset!=0x14` 报错），且**该函数根本没被调用到**。
- 真正卡点在 `nvmet_rdma_handle_command` → `nvmet_rdma_map_sgl`：它在 `nvmet_req_init`
  之后、`req->execute` 之前解析 SGL，发现 `sqe[39]=0`（非法子类型）→ `INVALID_FIELD`。

### 关键差异对照

| 维度 | SPDK `nvmf_tgt` | 内核 `nvmet` |
|------|----------------|-------------|
| admin 队列深度上限 | 无（`min()` 协商） | `NVME_AQ_DEPTH=32` 硬限制 |
| admin RQ 内嵌数据 | 64 + `in_capsule_data_size`(4096) | 仅 64B（无内嵌，Connect 数据须走 RDMA） |
| 私有数据字节序校验 | 宽松（`uint16_t` 直接读 + `min()`） | 严格（`le16_to_cpu` 精确比较） |
| 配置方式 | RPC | configfs |
| 运行位置 | 用户态（轮询） | 内核态（中断） |

### 内核 nvmet 配置（.10 上，`reset_nvmet.sh`）

```bash
sudo bash reset_nvmet.sh   # 卸载/重载 RDMA + nvmet，重配 configfs + loop 设备
```

核心 configfs 步骤：创建 subsystem + namespace(loop 设备) + port(rdma 192.168.255.10:4420)。
