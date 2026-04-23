# Project Instructions for AI Agents

This is **btclock_v4** — an ESP-IDF v5.5 C++ firmware for BTClock. The
project was extracted from `btclock_v3_fci/idf_cpp_proto/` on YYYY-MM-DD;
see the old repo at https://git.btclock.dev/btclock/btclock_v3 for
Arduino-era history and the original port's day-1 commits.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->

## Build & Test

```bash
# Source IDF env (install at ~/esp/v5.5.4/)
. ~/esp/v5.5.4/esp-idf/export.sh

# Variants select via POC_BOARD; keep per-build SDKCONFIG so variants
# don't poison each other
idf.py -B build-rev-a -D POC_BOARD=REV_A -D SDKCONFIG=build-rev-a/sdkconfig build
idf.py -B build-rev-b -D POC_BOARD=REV_B -D SDKCONFIG=build-rev-b/sdkconfig build
idf.py -B build-v8    -D POC_BOARD=V8    -D SDKCONFIG=build-v8/sdkconfig    build

# Host tests (no IDF dep, plain cmake)
cmake -S test_host -B build-host && cmake --build build-host && ./build-host/btclock_host_tests
```

## Architecture Overview

ESP32-S3, three hardware variants (Rev A / Rev B / V8), shared IDF code
with per-variant sdkconfig fragments. See docs/FEATURE_MATRIX.md for the
full parity matrix against the Arduino btclock_v3 firmware (links there
are pinned at old-repo SHA eac3a28).

## Conventions & Patterns

- WHY-only comments; avoid WHAT comments (identifiers already tell what).
- Renderer screen code under main/screens/; pure-logic helpers under
  main/screens/ or components/<area>/ so they're host-testable.
- Every HTTP endpoint goes in components/webserver/control_server.cpp with
  a trampoline-to-method pattern; max_uri_handlers budget lives there too.
