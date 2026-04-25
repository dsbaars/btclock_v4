// Host tests for the v2 WS subscribe-frame helper.
//
// Pin the gating contract introduced when the prior firmware
// subscribed to BOTH `blockfee` and `blockfee2`, then double-dispatched
// fee ticks: exactly one of the two topics must be in flight, picked
// by `blockFeeDec`. The dispatch helper mirrors HandleBinaryFrame's
// gate so a future refactor that drops it would surface here too.

#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "btclock_subscribe.hpp"

using btclock::subscribe::BuildSubscribeLogLine;
using btclock::subscribe::BuildSubscribeTopics;
using btclock::subscribe::ShouldDispatchTopic;

namespace {

bool Contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST_CASE("BuildSubscribeTopics: blockFeeDec=true emits blockfee2, not blockfee") {
  const auto topics =
      BuildSubscribeTopics({"USD", "EUR"}, /*block_fee_dec=*/true);
  CHECK(Contains(topics, "blockfee2"));
  CHECK_FALSE(Contains(topics, "blockfee"));
  // Sanity: the other topics still ship.
  CHECK(Contains(topics, "blockheight"));
  CHECK(Contains(topics, "price:USD"));
  CHECK(Contains(topics, "price:EUR"));
  // Order: blockheight first, fee second, then currencies in input order.
  CHECK(topics[0] == "blockheight");
  CHECK(topics[1] == "blockfee2");
}

TEST_CASE("BuildSubscribeTopics: blockFeeDec=false emits blockfee, not blockfee2") {
  const auto topics =
      BuildSubscribeTopics({"USD"}, /*block_fee_dec=*/false);
  CHECK(Contains(topics, "blockfee"));
  CHECK_FALSE(Contains(topics, "blockfee2"));
  CHECK(topics[1] == "blockfee");
}

TEST_CASE("BuildSubscribeTopics: empty currencies still ships block + fee") {
  const auto topics = BuildSubscribeTopics({}, /*block_fee_dec=*/true);
  CHECK(topics.size() == 2u);
  CHECK(topics[0] == "blockheight");
  CHECK(topics[1] == "blockfee2");
}

TEST_CASE("BuildSubscribeLogLine: serial line names exactly one fee stream") {
  const auto line_dec =
      BuildSubscribeLogLine({"USD", "EUR"}, /*block_fee_dec=*/true);
  CHECK(line_dec == "subscribe: blockheight + blockfee2 + price/[USD,EUR]");
  // Critical: the integer name must NOT appear when the decimal stream
  // is selected (and vice versa). The previous firmware logged
  // "blockfee + blockfee2", which was the user-visible symptom of the
  // double-subscribe bug.
  CHECK(line_dec.find("blockfee +") == std::string::npos);

  const auto line_int =
      BuildSubscribeLogLine({"USD", "EUR"}, /*block_fee_dec=*/false);
  CHECK(line_int == "subscribe: blockheight + blockfee + price/[USD,EUR]");
  CHECK(line_int.find("blockfee2") == std::string::npos);
}

TEST_CASE("ShouldDispatchTopic: blockFeeDec=true accepts blockfee2, drops blockfee") {
  CHECK(ShouldDispatchTopic("blockfee2", /*block_fee_dec=*/true));
  CHECK_FALSE(ShouldDispatchTopic("blockfee", /*block_fee_dec=*/true));
}

TEST_CASE("ShouldDispatchTopic: blockFeeDec=false accepts blockfee, drops blockfee2") {
  CHECK(ShouldDispatchTopic("blockfee", /*block_fee_dec=*/false));
  CHECK_FALSE(ShouldDispatchTopic("blockfee2", /*block_fee_dec=*/false));
}

TEST_CASE("ShouldDispatchTopic: non-fee topics pass through both modes") {
  CHECK(ShouldDispatchTopic("blockheight", /*block_fee_dec=*/true));
  CHECK(ShouldDispatchTopic("blockheight", /*block_fee_dec=*/false));
  CHECK(ShouldDispatchTopic("price:USD", /*block_fee_dec=*/true));
  CHECK(ShouldDispatchTopic("price:USD", /*block_fee_dec=*/false));
}
