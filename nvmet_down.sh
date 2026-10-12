#!/usr/bin/env bash
# Bring the kernel nvmet target DOWN (unload nvmet + nvmet_rdma + configfs), without reloading.
# Use reset_nvmet.sh to bring it back up.
set -u
NQN="nqn.2016-06.io.spdk:cnode1"

if [ -d /sys/kernel/config/nvmet ]; then
  sudo rm -f /sys/kernel/config/nvmet/ports/*/subsystems/"$NQN" 2>/dev/null || true
  for p in /sys/kernel/config/nvmet/ports/*; do sudo rmdir "$p" 2>/dev/null || true; done
  for s in /sys/kernel/config/nvmet/subsystems/*; do
    sudo bash -c "echo 0 > '$s'/namespaces/*/enable" 2>/dev/null || true
    for n in "$s"/namespaces/*; do sudo rmdir "$n" 2>/dev/null || true; done
    sudo rmdir "$s" 2>/dev/null || true
  done
fi

sudo modprobe -r nvmet_rdma nvmet 2>/dev/null || true
echo "nvmet down"