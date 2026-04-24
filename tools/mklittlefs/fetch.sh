#!/usr/bin/env bash
# Fetches the earlephilhower/mklittlefs binary for the current host and
# stages it as tools/mklittlefs/mklittlefs-<os>-<arch>. Pinned to a
# specific release so re-running produces a byte-identical binary.
#
# LittleFS on-disk version matters: this release must match the LittleFS
# version bundled with ESP-IDF's joltwallet__littlefs component, or
# images won't mount on the device. Bump MKLFS_VERSION / MKLFS_COMMIT
# together when joltwallet__littlefs upgrades LittleFS.
set -euo pipefail

MKLFS_VERSION="4.1.0"
MKLFS_COMMIT="42acb97"   # embedded in release asset filenames
BASE_URL="https://github.com/earlephilhower/mklittlefs/releases/download/${MKLFS_VERSION}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

uname_s="$(uname -s)"
uname_m="$(uname -m)"

case "${uname_s}-${uname_m}" in
  Darwin-arm64)   asset="aarch64-apple-darwin-mklittlefs-${MKLFS_COMMIT}.tar.gz";   out="mklittlefs-darwin-arm64" ;;
  Darwin-x86_64)  asset="x86_64-apple-darwin-mklittlefs-${MKLFS_COMMIT}.tar.gz";    out="mklittlefs-darwin-x86_64" ;;
  Linux-x86_64)   asset="x86_64-linux-gnu-mklittlefs-${MKLFS_COMMIT}.tar.gz";       out="mklittlefs-linux-x86_64" ;;
  Linux-aarch64)  asset="aarch64-linux-gnu-mklittlefs-${MKLFS_COMMIT}.tar.gz";      out="mklittlefs-linux-aarch64" ;;
  Linux-armv7l)   asset="arm-linux-gnueabihf-mklittlefs-${MKLFS_COMMIT}.tar.gz";    out="mklittlefs-linux-armv7l" ;;
  *)
    echo "fetch.sh: unsupported host ${uname_s}-${uname_m}; see ${BASE_URL}" >&2
    exit 1
    ;;
esac

dest="${HERE}/${out}"
if [[ -x "${dest}" ]] && "${dest}" --version 2>/dev/null | grep -q "mklittlefs ver. ${MKLFS_VERSION}"; then
  echo "fetch.sh: ${dest} already at ${MKLFS_VERSION}"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "fetch.sh: downloading ${asset}"
curl -fsSL -o "${tmp}/pkg.tar.gz" "${BASE_URL}/${asset}"
tar -C "${tmp}" -xzf "${tmp}/pkg.tar.gz"

# Release tarballs contain mklittlefs/mklittlefs
if [[ ! -f "${tmp}/mklittlefs/mklittlefs" ]]; then
  echo "fetch.sh: unexpected tarball layout" >&2
  exit 1
fi

install -m 0755 "${tmp}/mklittlefs/mklittlefs" "${dest}"
echo "fetch.sh: installed ${dest}"
"${dest}" --version | head -1
