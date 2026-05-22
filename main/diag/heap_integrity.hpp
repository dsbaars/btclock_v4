// Periodic heap integrity check.
//
// Walks every allocator block on the internal heap at a configurable
// interval and aborts via panic if `heap_caps_check_integrity_all`
// returns false. Designed to convert "secondary-victim panic at the
// next allocation" into "primary detection panic with a coredump
// captured at the moment corruption is observed". The httpd-task
// abort dumps from 2026-05-22 (bd btclock_v4-28n) hit `_M_create`
// with stack-range pointers in size-typed registers — the classic
// "the std::string was corrupt before it was used" pattern. This
// catches that whenever it happens in a deterministic place rather
// than wherever the next allocator call lands.
//
// Cheap: full walk over ~300 allocator blocks completes in ~1-2 ms;
// at 30 s interval that's ~0.005 % CPU. Default-enabled — the cost
// is negligible against the diagnostic value if the bug recurs.
//
// Pairs naturally with CONFIG_HEAP_POISONING_LIGHT or
// CONFIG_HEAP_POISONING_COMPREHENSIVE (chip-wide IDF Kconfig) for
// finer-grained corruption detection. The default IDF setting on
// this project is `CONFIG_HEAP_POISONING_DISABLED=y` which limits
// what this check can detect to allocator-arena metadata damage —
// adjacent-block stomp bugs still slip through until they cascade.

#pragma once

namespace btclock {

// Start the periodic check task. Idempotent: subsequent calls are
// no-ops. Safe to call after FreeRTOS scheduler is up.
//
// On `heap_caps_check_integrity_all` failure the task logs ERROR with
// a marker the panic decoder can grep on ("HEAP_INTEGRITY_FAILED")
// then calls `abort()` so a coredump captures the live state at the
// moment corruption was detected.
void InitHeapIntegrityMonitor();

}  // namespace btclock
