#!/usr/bin/env bash
# Cross-compile gb28181_client for reCamera (riscv64 musl). Run on seeed.
set -euo pipefail
export PATH=/home/seeed/zsz/TOOL/riscv64-linux-musl-x86_64/bin:$PATH
PREFIX=/home/seeed/gb28181/install
SR="/home/seeed/桌面/sg2002_recamera_emmc/buildroot-2021.05/output/cvitek_CV181X_musl_riscv64/host/riscv64-buildroot-linux-musl/sysroot"
cd "$(dirname "$0")"
riscv64-unknown-linux-musl-gcc gb28181_client.c -o gb28181_client \
  -I"$PREFIX/include" -I"$SR/usr/include" \
  -L"$PREFIX/lib" -L"$SR/usr/lib" \
  -leXosip2 -losip2 -losipparser2 -lcares -lssl -lcrypto -lpthread \
  -Wl,-rpath-link,"$PREFIX/lib" -Wl,-rpath-link,"$SR/usr/lib" -Wl,-rpath-link,"$SR/lib"
echo "built: $(ls -la gb28181_client)"
