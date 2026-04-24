# mklittlefs (vendored)

Replacement for the stale PlatformIO-bundled `mklittlefs` (commit
`05d49dc`, Sep 2023). That binary ships LittleFS v1.x metadata and
clashes with the LittleFS 2.11 runtime pulled in by ESP-IDF v5.5.4's
`joltwallet__littlefs` component.

## What's vendored

- Release: [`earlephilhower/mklittlefs` 4.1.0](https://github.com/earlephilhower/mklittlefs/releases/tag/4.1.0) (commit `42acb97`).
- LittleFS: `v2.11.1` — same major/minor as
  `managed_components/joltwallet__littlefs` (1.21.1), which bundles
  LittleFS 2.11 (`LFS_DISK_VERSION 0x00020001`). On-disk images are
  mutually mountable.
- Filename limit: `LFS_NAME_MAX = 255` on both packer and runtime, so
  the superblock `name_max` the packer records will always fit.

## Usage

```bash
# Auto-select the right binary for the current host (macOS arm64/x86_64,
# Linux x86_64/aarch64/armv7l):
tools/mklittlefs/mklittlefs --create data/build_gz --size 0xCD000 \
  --block 4096 --page 256 build-rev-b/storage.bin
```

The wrapper execs `mklittlefs-<os>-<arch>` from this directory. If it
isn't there (e.g. fresh clone, new host), run:

```bash
tools/mklittlefs/fetch.sh
```

`fetch.sh` pins both the release version and the asset-filename commit
SHA, so re-running produces a byte-identical binary.

## Bumping the pinned version

LittleFS on-disk format is tied to the version. Bump **only** when
`joltwallet__littlefs` bumps its vendored LittleFS:

1. Check `managed_components/joltwallet__littlefs/src/littlefs/lfs.h`
   for the new `LFS_DISK_VERSION` / `LFS_VERSION`.
2. Pick the matching `earlephilhower/mklittlefs` release from
   <https://github.com/earlephilhower/mklittlefs/releases>.
3. Edit `MKLFS_VERSION` and `MKLFS_COMMIT` in `fetch.sh`, rerun it for
   every supported host, and commit the resulting binaries.
