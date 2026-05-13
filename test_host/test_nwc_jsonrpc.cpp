// Host tests for the NWC JSON-RPC encode/decode helpers.

#include <string>
#include <vector>

#include "doctest.h"
#include "nwc/jsonrpc.hpp"

using namespace btclock::nwc;

TEST_CASE("BuildGetBalanceRequest emits the bit-fixed NIP-47 payload") {
  CHECK(BuildGetBalanceRequest() == R"({"method":"get_balance","params":{}})");
}

TEST_CASE("DecodeBalanceResponse: spec-shaped success payload") {
  const std::string json =
      R"({"result_type":"get_balance","result":{"balance":10000}})";
  BalanceResponse out;
  WalletError err;
  CHECK(DecodeBalanceResponse(json, out, err) == RpcError::kOk);
  CHECK(out.balance_msat == 10000u);
}

TEST_CASE("DecodeBalanceResponse: large balance survives 53-bit clean-up") {
  // ~21 BTC in msat. Well under 2^53; precision survives.
  const std::string json =
      R"({"result_type":"get_balance","result":{"balance":2100000000000000}})";
  BalanceResponse out;
  WalletError err;
  CHECK(DecodeBalanceResponse(json, out, err) == RpcError::kOk);
  CHECK(out.balance_msat == 2100000000000000ULL);
}

TEST_CASE("DecodeBalanceResponse: stringified balance accepted") {
  const std::string json =
      R"({"result_type":"get_balance","result":{"balance":"123456789012345"}})";
  BalanceResponse out;
  WalletError err;
  CHECK(DecodeBalanceResponse(json, out, err) == RpcError::kOk);
  CHECK(out.balance_msat == 123456789012345ULL);
}

TEST_CASE("DecodeBalanceResponse: wallet error surfaced") {
  const std::string json =
      R"({"result_type":"get_balance","error":{"code":"UNAUTHORIZED","message":"no perms"},"result":null})";
  BalanceResponse out;
  WalletError err;
  CHECK(DecodeBalanceResponse(json, out, err) == RpcError::kWalletError);
  CHECK(err.code == "UNAUTHORIZED");
  CHECK(err.message == "no perms");
}

TEST_CASE("DecodeBalanceResponse: wrong result_type rejected") {
  const std::string json =
      R"({"result_type":"pay_invoice","result":{"preimage":"deadbeef"}})";
  BalanceResponse out;
  WalletError err;
  CHECK(DecodeBalanceResponse(json, out, err) == RpcError::kMethodMismatch);
}

TEST_CASE("DecodeBalanceResponse: garbage rejected") {
  BalanceResponse out;
  WalletError err;
  CHECK(DecodeBalanceResponse("not json", out, err) == RpcError::kNotJson);
  CHECK(DecodeBalanceResponse("{}", out, err) == RpcError::kMissingResultType);
  CHECK(DecodeBalanceResponse(R"({"result_type":"get_balance"})", out, err) ==
        RpcError::kMissingResult);
}

TEST_CASE("DecodeInfoEvent: INFO event content + tags") {
  // Spec-shaped INFO event:
  //   content = space-separated methods
  //   tags    = [["encryption","nip44_v2 nip04"],
  //   ["notifications","payment_received payment_sent"]]
  std::vector<std::vector<std::string>> tags = {
      {"encryption", "nip44_v2 nip04"},
      {"notifications", "payment_received payment_sent"},
  };
  InfoEvent info;
  REQUIRE(DecodeInfoEvent("pay_invoice get_balance notifications", tags, info));
  REQUIRE(info.methods.size() == 3);
  CHECK(info.methods[0] == "pay_invoice");
  CHECK(info.methods[1] == "get_balance");
  CHECK(info.methods[2] == "notifications");
  REQUIRE(info.encryption.size() == 2);
  CHECK(info.encryption[0] == "nip44_v2");
  CHECK(info.encryption[1] == "nip04");
  REQUIRE(info.notifications.size() == 2);
  CHECK(info.notifications[0] == "payment_received");
  CHECK(info.notifications[1] == "payment_sent");
}

TEST_CASE("DecodeInfoEvent: encryption split across multiple tag values") {
  // Some wallets emit `["encryption","nip44_v2","nip04"]` (each on
  // its own value index) instead of one space-separated value.
  std::vector<std::vector<std::string>> tags = {
      {"encryption", "nip44_v2", "nip04"},
  };
  InfoEvent info;
  REQUIRE(DecodeInfoEvent("get_balance", tags, info));
  REQUIRE(info.encryption.size() == 2);
  CHECK(info.encryption[0] == "nip44_v2");
  CHECK(info.encryption[1] == "nip04");
}

TEST_CASE("DecodeInfoEvent: empty content rejected") {
  std::vector<std::vector<std::string>> tags = {};
  InfoEvent info;
  CHECK(!DecodeInfoEvent("", tags, info));
}

TEST_CASE("DecodePaymentNotification: payment_received decodes amount + desc") {
  const std::string json = R"({
    "notification_type": "payment_received",
    "notification": {
      "type": "incoming",
      "state": "settled",
      "invoice": "lnbc...",
      "description": "test zap",
      "payment_hash": "deadbeef",
      "amount": 21000,
      "fees_paid": 0,
      "created_at": 1747000000,
      "settled_at": 1747000005
    }
  })";
  PaymentNotification n;
  CHECK(DecodePaymentNotification(json, n) == RpcError::kOk);
  CHECK(n.direction == PaymentDirection::kIncoming);
  CHECK(n.amount_msat == 21000u);
  CHECK(n.fees_paid_msat == 0u);
  CHECK(n.description == "test zap");
  CHECK(n.payment_hash == "deadbeef");
  CHECK(n.created_at == 1747000000u);
  CHECK(n.settled_at == 1747000005u);
}

TEST_CASE("DecodePaymentNotification: payment_sent flips direction") {
  const std::string json = R"({
    "notification_type": "payment_sent",
    "notification": {
      "type": "outgoing",
      "amount": 5000,
      "fees_paid": 1,
      "payment_hash": "abc"
    }
  })";
  PaymentNotification n;
  CHECK(DecodePaymentNotification(json, n) == RpcError::kOk);
  CHECK(n.direction == PaymentDirection::kOutgoing);
  CHECK(n.amount_msat == 5000u);
  CHECK(n.fees_paid_msat == 1u);
}

TEST_CASE(
    "DecodePaymentNotification: unknown notification_type stays kUnknown") {
  const std::string json = R"({
    "notification_type": "hold_invoice_accepted",
    "notification": { "payment_hash": "abc" }
  })";
  PaymentNotification n;
  CHECK(DecodePaymentNotification(json, n) == RpcError::kOk);
  CHECK(n.direction == PaymentDirection::kUnknown);
  CHECK(n.payment_hash == "abc");
}

TEST_CASE("DecodePaymentNotification: garbage rejected") {
  PaymentNotification n;
  CHECK(DecodePaymentNotification("not-json", n) == RpcError::kNotJson);
}

TEST_CASE("BuildListTransactionsRequest: from + until + limit") {
  // Spec-conformant JSON with both bounds + unpaid=false filter.
  CHECK(
      BuildListTransactionsRequest(1747000000, 1747000600, 20) ==
      R"({"method":"list_transactions","params":{"from":1747000000,"until":1747000600,"limit":20,"unpaid":false}})");
}

TEST_CASE("BuildListTransactionsRequest: until=0 omits the field") {
  // until=0 means "now" per the spec — let the wallet default it.
  CHECK(
      BuildListTransactionsRequest(1747000000, 0, 20) ==
      R"({"method":"list_transactions","params":{"from":1747000000,"limit":20,"unpaid":false}})");
}

TEST_CASE("DecodeListTransactionsResponse: mixed incoming + outgoing") {
  const std::string json = R"({
    "result_type":"list_transactions",
    "result":{
      "transactions":[
        {"type":"outgoing","amount":250000,"fees_paid":100,
         "description":"coffee","payment_hash":"a","created_at":1,
         "settled_at":1747000050},
        {"type":"incoming","amount":1500000,"fees_paid":0,
         "description":"topup","payment_hash":"b","created_at":2,
         "settled_at":1747000100}
      ]
    }
  })";
  std::vector<PaymentNotification> txs;
  WalletError err;
  REQUIRE(DecodeListTransactionsResponse(json, txs, err) == RpcError::kOk);
  REQUIRE(txs.size() == 2);
  CHECK(txs[0].direction == PaymentDirection::kOutgoing);
  CHECK(txs[0].amount_msat == 250000u);
  CHECK(txs[0].fees_paid_msat == 100u);
  CHECK(txs[0].description == "coffee");
  CHECK(txs[1].direction == PaymentDirection::kIncoming);
  CHECK(txs[1].amount_msat == 1500000u);
  CHECK(txs[1].settled_at == 1747000100u);
}

TEST_CASE("DecodeListTransactionsResponse: empty array → kOk with no entries") {
  // Wallet returning an empty array is the "no payments in window"
  // case — boot-poll path treats it as success + does nothing.
  const std::string json =
      R"({"result_type":"list_transactions","result":{"transactions":[]}})";
  std::vector<PaymentNotification> txs;
  WalletError err;
  CHECK(DecodeListTransactionsResponse(json, txs, err) == RpcError::kOk);
  CHECK(txs.empty());
}

TEST_CASE("DecodeListTransactionsResponse: wallet error propagated") {
  const std::string json = R"({
    "result_type":"list_transactions",
    "error":{"code":"INTERNAL","message":"db unavailable"}
  })";
  std::vector<PaymentNotification> txs;
  WalletError err;
  CHECK(DecodeListTransactionsResponse(json, txs, err) ==
        RpcError::kWalletError);
  CHECK(err.code == "INTERNAL");
  CHECK(err.message == "db unavailable");
  CHECK(txs.empty());
}

TEST_CASE("DecodeListTransactionsResponse: result_type mismatch") {
  const std::string json =
      R"({"result_type":"get_balance","result":{"balance":1000}})";
  std::vector<PaymentNotification> txs;
  WalletError err;
  CHECK(DecodeListTransactionsResponse(json, txs, err) ==
        RpcError::kMethodMismatch);
}
