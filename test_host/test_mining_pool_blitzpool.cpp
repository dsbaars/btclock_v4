// Host-only parser test for Blitzpool's PPLNS-balance endpoint.
//
// Phase 1 (worker list at /api/client/<addr>) reuses the public-pool
// parser and is exercised by test_mining_pool_public_pool. This file
// covers the phase-2 secondary parser only: extracting `balanceSats`
// from /api/pplns/<addr> into ParsedStats.daily_sats.

#include <cstdint>
#include <string>

#include "doctest.h"
#include "mining_pool_blitzpool/blitzpool_parser.hpp"
#include "mining_pool_common/parsed_stats.hpp"
#include "mining_pool_public_pool/public_pool_parser.hpp"

namespace {
using btclock::mining_pools::ParsedStats;
using btclock::mining_pools::blitzpool::parse_pplns_balance;

TEST_CASE("blitzpool: pplns balance with pending sats sets daily_sats") {
  // Shape from a live PPLNS address — extra fields ignored.
  constexpr const char* body = R"({
    "balanceSats": 12345,
    "totalPaidSats": 50000,
    "currentWindowShares": 1234567,
    "currentWindowPercent": 0.42,
    "balanceLabel": "pending"
  })";
  ParsedStats out;
  CHECK(parse_pplns_balance(body, out));
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 12345);
}

TEST_CASE("blitzpool: zero balance leaves daily_sats unset") {
  // Solo addresses always return balanceSats=0 — the earnings screen
  // should fall back to "n/a" rather than displaying 0 SATS, so we
  // leave has_daily_sats=false in that case.
  constexpr const char* body = R"({
    "balanceSats": 0,
    "totalPaidSats": 0,
    "currentWindowShares": 0,
    "currentWindowPercent": 0,
    "balanceLabel": "zero"
  })";
  ParsedStats out;
  CHECK(parse_pplns_balance(body, out));
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("blitzpool: fractional sats round to nearest integer") {
  // Hedge against a future move to fractional-sat accounting — the
  // earnings field is int64, so we round rather than truncate.
  constexpr const char* body = R"({"balanceSats": 12345.7})";
  ParsedStats out;
  CHECK(parse_pplns_balance(body, out));
  CHECK(out.has_daily_sats);
  CHECK(out.daily_sats == 12346);
}

TEST_CASE("blitzpool: missing balanceSats field is not a parse error") {
  // Older / partial responses lacking balanceSats shouldn't fail the
  // poll — the primary parse already populated hashrate + workers,
  // and we don't want to discard those because of a missing field.
  constexpr const char* body = R"({"balanceLabel": "n/a"})";
  ParsedStats out;
  CHECK(parse_pplns_balance(body, out));
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("blitzpool: malformed JSON returns false") {
  ParsedStats out;
  CHECK_FALSE(parse_pplns_balance("{not json", out));
  CHECK_FALSE(out.has_daily_sats);
}

TEST_CASE("blitzpool global: totalHashRate + totalMiners") {
  // Live shape of /api/pool — values picked so the round-trip lands
  // on a known integer (no fractional truncation drama).
  constexpr const char* body = R"({
    "totalHashRate": 7901351912194065,
    "blockHeight": 949664,
    "totalMiners": 684,
    "blocksFound": [],
    "fee": 0,
    "_cachedAt": "2026-05-16T13:30:17.003Z"
  })";
  ParsedStats out;
  CHECK(btclock::mining_pools::public_pool::parse_pool_global(body, out));
  CHECK(out.hashrate == "7901351912194065");
  CHECK(out.has_workers);
  CHECK(out.workers == 684);
}

TEST_CASE("blitzpool global: zero totalHashRate leaves hashrate empty") {
  // Bootstrap / outage case — the base PollOnce keeps the previous
  // snapshot when hashrate is empty, so we must NOT emit "0".
  constexpr const char* body = R"({"totalHashRate": 0, "totalMiners": 0})";
  ParsedStats out;
  CHECK(btclock::mining_pools::public_pool::parse_pool_global(body, out));
  CHECK(out.hashrate.empty());
}

}  // namespace
