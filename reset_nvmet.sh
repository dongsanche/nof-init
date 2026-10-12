#!/usr/bin/env bash
set -euo pipefail
NQN="nqn.2016-06.io.spdk:cnode1"

# 1. 先清掉残留 configfs（若有），再卸载模块
if [ -d /sys/kernel/config/nvmet ]; then
  sudo rm -f /sys/kernel/config/nvmet/ports/*/subsystems/"$NQN" 2>/dev/null || true
  for p in /sys/kernel/config/nvmet/ports/*; do sudo rmdir "$p" 2>/dev/null || true; done
  for s in /sys/kernel/config/nvmet/subsystems/*; do
    sudo bash -c "echo 0 > '$s'/namespaces/*/enable" 2>/dev/null || true
    for n in "$s"/namespaces/*; do sudo rmdir "$n" 2>/dev/null || true; done
    sudo rmdir "$s" 2>/dev/null || true
  done
fi

# 2. 卸载
sudo modprobe -r nvmet_rdma nvmet 2>/dev/null || true
sudo modprobe -r rpcrdma sunrpc ib_iser libiscsi scsi_transport_iscsi rdma_ucm rdma_cm iw_cm ib_cm 2>/dev/null || true
sudo modprobe -r rdma_rxe ib_uverbs 2>/dev/null || true
sleep 1

# 3. 重载
sudo modprobe rdma_rxe
sudo modprobe rdma_cm
sudo modprobe nvmet
sudo modprobe nvmet_rdma
sudo rdma link add rxe0 type rxe netdev ens33 2>/dev/null || true

# 4. loop 设备
if ! sudo losetup -a | grep -q nvme.img; then
    sudo losetup -f /tmp/nvme.img --sector-size 4096
fi
LOOP=$(sudo losetup -a | grep nvme.img | cut -d: -f1)

# 5. 配置 nvmet
sudo mkdir -p /sys/kernel/config/nvmet/subsystems/$NQN
echo 1 | sudo tee /sys/kernel/config/nvmet/subsystems/$NQN/attr_allow_any_host >/dev/null
sudo mkdir -p /sys/kernel/config/nvmet/subsystems/$NQN/namespaces/1
echo "$LOOP" | sudo tee /sys/kernel/config/nvmet/subsystems/$NQN/namespaces/1/device_path >/dev/null
echo 1 | sudo tee /sys/kernel/config/nvmet/subsystems/$NQN/namespaces/1/enable >/dev/null

sudo mkdir -p /sys/kernel/config/nvmet/ports/1
echo rdma | sudo tee /sys/kernel/config/nvmet/ports/1/addr_trtype >/dev/null
echo ipv4 | sudo tee /sys/kernel/config/nvmet/ports/1/addr_adrfam >/dev/null
echo 192.168.255.10 | sudo tee /sys/kernel/config/nvmet/ports/1/addr_traddr >/dev/null
echo 4420 | sudo tee /sys/kernel/config/nvmet/ports/1/addr_trsvcid >/dev/null
sudo ln -sf /sys/kernel/config/nvmet/subsystems/$NQN /sys/kernel/config/nvmet/ports/1/subsystems/$NQN
echo "nvmet ready"
