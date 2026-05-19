# coredump_decode

Wrapper around `espcoredump.py` that bypasses the `app_elf_sha256`
mismatch check.

## When you need this

`espcoredump.py` refuses to decode a coredump if the ELF you pass in
doesn't match the SHA the firmware baked into the coredump at crash
time. That's the right default — it prevents accidentally decoding
against a totally unrelated build. But it blocks two legitimate
scenarios:

1. **Non-reproducible build, original artifacts gone.** Without
   `CONFIG_APP_REPRODUCIBLE_BUILD=y` (enabled in this project since
   `2f5bd1e`, 2026-05-20), every fullclean rebuild of the same
   commit produces a different `.bin` SHA. If you dropped the
   `build-<variant>/btclock_v4.elf` from the crashing run, you're
   stuck.
2. **Cross-commit decode as a last resort.** When the original
   commit was never archived but a nearby commit's ELF is close
   enough to read the panic-handler context.

## Trust boundary

A SHA mismatch means the binary you're decoding against differs from
the binary that crashed. The further they drift, the more
unreliable the deeper frames become:

- **Frames `#0`-`#3`**: the panic-handler context (PC, registers,
  `esp_restart_noos`-chain). These are reliable — the addresses come
  straight from the crashed CPU state, not from symbol lookup.
- **Frames `#4`+**: function names and source locations come from
  resolving the ELF's symbol table against the coredump's stack
  pointers. If the symbol layout shifted between builds (it always
  shifts a little — different timestamps, different optimization
  decisions on functions that recompiled), the names you see can be
  off by hundreds of bytes. They'll look plausible but they're
  illusions.

Reproducible builds eliminate the drift in the common case (same
commit → same SHA → same symbol table). Reach for this wrapper only
when reproducible-build rebuild isn't an option.

See also: [`btclock_v4-ajf`](https://git.btclock.dev/btclock/btclock_v4/issues)
for the stale-coredump story that prompted this tool.

## Field-side discipline that beats this tool

Before reaching for the bypass, do this on any device with a coredump
you might want to decode later:

```bash
# Capture the firmware identity FIRST — gitRev is compiled into the
# binary via PROJECT_VER, so it's gone the moment you re-flash.
curl -s http://btclock-<MAC>.local/api/settings | jq -r .gitRev
# → e.g. "4.0.0-rc.12-14-g5b76bf8" — write this down.

curl -s http://btclock-<MAC>.local/api/coredump -o /tmp/dump.bin
```

Then you can pair the dump with `git checkout <commit>` + a fresh
build, get the matching ELF, and decode without any bypass. That's
strictly better — the backtrace stays trustworthy past frame `#3`.

The bypass below is for the cases where capturing gitRev didn't
happen and the original commit is already lost.

## Usage

```bash
# Source the IDF env so esp_coredump + xtensa toolchain are on PATH.
source ~/esp/v6.0/esp-idf/export.sh

# Decode. Most coredumps from /api/coredump are wrapped, so use -t raw;
# only the explicit `--save-core-to-file` path produces -t elf.
tools/coredump_decode/coredump_decode.py info_corefile \
  -t raw -c /tmp/dump.bin build-rev-b/btclock_v4.elf
```

Any arguments after the script name are forwarded to `espcoredump.py`
verbatim — the wrapper only patches the SHA-mismatch raise out, then
hands off.

A successful bypass surfaces a line like

```
WARNING (btclock SHA-bypass): coredump SHA256(a6e7c405) != app SHA256(92c4e540).
Trust only panic-handler frames; deeper backtrace may be symbol-resolution illusion.
```

on stderr. The decode then proceeds normally.

## Limitations

- The wrapper patches `esp_coredump.corefile.loader` in-process via
  monkey-patch, so it doesn't touch the on-disk install. If a future
  IDF update changes the method or message text the wrapper looks
  for, you'll see `warning: SHA-mismatch raise not found ...` and the
  original check stays active. Update `_RAISE_PATTERN` in
  `coredump_decode.py` to the new message format if that happens.
- Bytecode caches (`__pycache__`) for the loader module aren't
  affected by the in-process patch (the original `.pyc` stays
  untouched, which is what we want).
