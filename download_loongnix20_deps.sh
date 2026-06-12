#!/usr/bin/env bash
# Copyright (c) 2026 jingzezhang818.
# All rights reserved.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${1:-${SCRIPT_DIR}/loongnix20_debs}"
APT_ROOT="${OUT_DIR}/apt-root"
DEB_DIR="${OUT_DIR}/debs"

PACKAGES=(
  cmake
  cmake-data
  make
  pkg-config
  qtbase5-dev
  qtbase5-dev-tools
  qttools5-dev-tools
  qtmultimedia5-dev
  libqt5multimedia5
  libqt5multimediawidgets5
  libqt5multimedia5-plugins
  libqt5multimediagsttools5
  libgstreamer1.0-0
  libgstreamer1.0-dev
  libgstreamer-plugins-base1.0-dev
  libglib2.0-dev
  gstreamer1.0-tools
  gstreamer1.0-plugins-base
  gstreamer1.0-plugins-good
  gstreamer1.0-plugins-bad
  gstreamer1.0-x
  v4l-utils
  libv4l-0
)

mkdir -p \
  "${APT_ROOT}/etc/apt" \
  "${APT_ROOT}/etc/apt/sources.list.d" \
  "${APT_ROOT}/etc/apt/preferences.d" \
  "${APT_ROOT}/var/lib/apt/lists/partial" \
  "${APT_ROOT}/var/cache/apt/archives/partial" \
  "${APT_ROOT}/var/lib/dpkg" \
  "${DEB_DIR}"

cat > "${APT_ROOT}/etc/apt/sources.list" <<'EOF'
deb [trusted=yes] http://pkg.loongnix.cn/loongnix/20 DaoXiangHu-stable main contrib non-free
EOF

: > "${APT_ROOT}/var/lib/dpkg/status"

APT_OPTS=(
  -o "Dir=${APT_ROOT}"
  -o "Dir::Etc::sourcelist=sources.list"
  -o "Dir::Etc::sourceparts=sources.list.d"
  -o "Dir::Etc::preferencesparts=preferences.d"
  -o "Dir::Etc::trusted=/dev/null"
  -o "Dir::Etc::trustedparts=/dev/null"
  -o "Dir::State::status=${APT_ROOT}/var/lib/dpkg/status"
  -o "Dir::Cache::archives=${DEB_DIR}"
  -o "APT::Architecture=loongarch64"
  -o "APT::Architectures::=loongarch64"
  -o "Acquire::AllowInsecureRepositories=true"
  -o "Acquire::AllowDowngradeToInsecureRepositories=true"
)

apt-get "${APT_OPTS[@]}" update

apt-get "${APT_OPTS[@]}" \
  --download-only \
  --no-install-recommends \
  --allow-unauthenticated \
  -y install "${PACKAGES[@]}"

echo
echo "Downloaded Loongnix 20 loongarch64 packages to:"
echo "${DEB_DIR}"
