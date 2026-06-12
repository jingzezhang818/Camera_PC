#!/usr/bin/env bash
# Copyright (c) 2026 jingzezhang818.
# All rights reserved.

set -euo pipefail

# Default to MSI mode(equivalent to ./load_driver.sh 1)
MODE="${1:-1}"

SELF_PATH="$(readlink -f "$0")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="${SCRIPT_DIR}/../dma_ip_drivers/XDMA/linux-kernel/tests"

if [[ ! -d "$TEST_DIR" ]]; then
  echo "Error: XDMA tests directory not found: $TEST_DIR" >&2
  exit 1
fi

if [[ $EUID -ne 0 ]]; then
  echo "[INFO] Switching to root via sudo to load XDMA driver..."
  exec sudo "$SELF_PATH" "$MODE"
fi

cd "$TEST_DIR"
./load_driver.sh "$MODE"
