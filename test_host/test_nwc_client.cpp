// Host tests for the NWC client state machine.
//
// Strategy: construct an NwcClient with fake subscribe / unsubscribe
// / publish functors that capture every call. Then drive the inbound
// path via `NwcClient::HandleEvent(ev)` with crafted kind 13194 INFO
// / 23195 response / 23197 + 23196 notification events. The crypto
// roundtrip is real — we encrypt the test payload with the wallet's
// `secret`, mirroring what a real wallet service would put on the
// wire. The signer is real too (every published frame is fully
// canonical-id'd and schnorr-signed) so `VerifyEvent` on the
// captured frame would succeed end-to-end.

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "nostr/event.hpp"
#include "nostr/event_sign.hpp"
#include "nostr/event_verify.hpp"
#include "nostr/nip4x.hpp"
#include "nostr/parser.hpp"
#include "nwc/client.hpp"
#include "nwc/uri.hpp"

using namespace btclock;

namespace {

// Test fixture: pairing URI built from BIP-340 vector-1's seckey as
// the *client* secret and a known wallet keypair (we generate the
// wallet keypair by deriving from sk=2 here, just for determinism).
constexpr const char* kClientSecretHex =
    "b7e151628aed2a6abf7158809cf4f3c762e7160f38b4da56a784d9045190cfef";

// Wallet keypair: sk = 02..02 (BIP-340 vector-style). Derived pubkey
// (computed below) is the x-only point we'll feed into the URI.
constexpr const char* kWalletSecretHex =
    "0000000000000000000000000000000000000000000000000000000000000002";

// Compute the wallet x-only pubkey hex from kWalletSecretHex once,
// cached on first call. This lets us build a URI literally derivable
// by anyone who runs the test with `secp256k1` linked.
std::string WalletPubkeyHex() {
  static std::string cached;
  if (!cached.empty()) return cached;
  uint8_t sk[32];
  REQUIRE(nwc::DecodeHex32(kWalletSecretHex, sk));
  uint8_t pk[32];
  REQUIRE(nostr::DerivePubkeyXOnly(sk, pk) == nostr::EventSignError::kOk);
  static constexpr char kHex[] = "0123456789abcdef";
  cached.resize(64);
  for (size_t i = 0; i < 32; ++i) {
    cached[2 * i] = kHex[(pk[i] >> 4) & 0xfu];
    cached[2 * i + 1] = kHex[pk[i] & 0xfu];
  }
  return cached;
}

std::string ClientPubkeyHex() {
  static std::string cached;
  if (!cached.empty()) return cached;
  uint8_t sk[32];
  REQUIRE(nwc::DecodeHex32(kClientSecretHex, sk));
  uint8_t pk[32];
  REQUIRE(nostr::DerivePubkeyXOnly(sk, pk) == nostr::EventSignError::kOk);
  static constexpr char kHex[] = "0123456789abcdef";
  cached.resize(64);
  for (size_t i = 0; i < 32; ++i) {
    cached[2 * i] = kHex[(pk[i] >> 4) & 0xfu];
    cached[2 * i + 1] = kHex[pk[i] & 0xfu];
  }
  return cached;
}

// Build a parsed URI fixture (relay URL is informational — no socket
// is opened in these tests).
nwc::PairingUri MakeFixtureUri() {
  const std::string uri = std::string("nostr+walletconnect://") +
                          WalletPubkeyHex() +
                          "?relay=wss%3A%2F%2Fnostr-faucet.example"
                          "&secret=" +
                          kClientSecretHex;
  nwc::PairingUri parsed;
  REQUIRE(nwc::ParsePairingUri(uri, parsed) == nwc::ParseError::kOk);
  return parsed;
}

// Construct a kind-13194 INFO event published by the wallet.
nostr::Event MakeInfoEvent(const std::string& methods,
                           const std::vector<std::string>& encryption,
                           const std::vector<std::string>& notifications) {
  nostr::Event ev;
  ev.created_at = 1747000000;
  ev.kind = 13194;
  ev.content = methods;
  ev.pubkey = WalletPubkeyHex();
  {
    nostr::Tag t;
    t.values.push_back("encryption");
    for (const auto& e : encryption) t.values.push_back(e);
    ev.tags.push_back(t);
  }
  if (!notifications.empty()) {
    nostr::Tag t;
    t.values.push_back("notifications");
    for (const auto& n : notifications) t.values.push_back(n);
    ev.tags.push_back(t);
  }
  // No need to sign — NwcClient::HandleEvent doesn't re-verify.
  return ev;
}

// Encrypt a plaintext payload as the wallet would — wallet secret +
// client pubkey + chosen variant. Returns the base64 content suitable
// for an Event's `content` field.
std::string WalletEncrypt(nostr::EncryptionVariant v,
                          const std::string& plaintext) {
  uint8_t wallet_sec[32];
  REQUIRE(nwc::DecodeHex32(kWalletSecretHex, wallet_sec));
  uint8_t client_pub[32];
  REQUIRE(nwc::DecodeHex32(ClientPubkeyHex(), client_pub));
  uint8_t nonce[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};  // deterministic
  return nostr::Encrypt(v, wallet_sec, client_pub, nonce, plaintext);
}

// Construct a kind-23195 response from the wallet to the client.
nostr::Event MakeResponse(const std::string& json_plaintext,
                          const std::string& request_id,
                          nostr::EncryptionVariant variant) {
  nostr::Event ev;
  ev.created_at = 1747000100;
  ev.kind = 23195;
  ev.pubkey = WalletPubkeyHex();
  ev.content = WalletEncrypt(variant, json_plaintext);
  {
    nostr::Tag p;
    p.values = {"p", ClientPubkeyHex()};
    ev.tags.push_back(p);
  }
  if (!request_id.empty()) {
    nostr::Tag e;
    e.values = {"e", request_id};
    ev.tags.push_back(e);
  }
  return ev;
}

// Construct a kind-23197 (modern) notification event.
nostr::Event MakeNotification(const std::string& json_plaintext,
                              uint32_t kind = 23197) {
  nostr::Event ev;
  ev.created_at = 1747000200;
  ev.kind = kind;
  ev.pubkey = WalletPubkeyHex();
  const auto variant = (kind == 23197) ? nostr::EncryptionVariant::kNip44V2
                                        : nostr::EncryptionVariant::kNip04;
  ev.content = WalletEncrypt(variant, json_plaintext);
  {
    nostr::Tag p;
    p.values = {"p", ClientPubkeyHex()};
    ev.tags.push_back(p);
  }
  return ev;
}

// Bundle the fake hooks so each test gets a clean instance.
struct ClientFixture {
  std::vector<std::pair<std::string, nostr::Filter>> subscribes;
  std::vector<std::string> unsubscribes;
  std::vector<std::string> publishes;
  uint64_t last_balance = 0;
  int balance_calls = 0;
  int payment_calls = 0;
  int ready_calls = 0;
  nwc::PaymentNotification last_payment;
  nwc::InfoEvent last_info;
  std::unique_ptr<nwc::NwcClient> client;

  ClientFixture() {
    auto pairing = MakeFixtureUri();
    auto publish = [this](const char* data, size_t n) {
      publishes.emplace_back(data, n);
      return true;
    };
    auto subscribe = [this](const std::string& id,
                            const nostr::Filter& f) {
      subscribes.emplace_back(id, f);
    };
    auto unsubscribe = [this](const std::string& id) {
      unsubscribes.push_back(id);
    };
    client = std::make_unique<nwc::NwcClient>(std::move(pairing), publish,
                                              subscribe, unsubscribe);
    client->SetOnBalance(
        [this](uint64_t m) { last_balance = m; ++balance_calls; });
    client->SetOnPayment([this](const nwc::PaymentNotification& p) {
      last_payment = p;
      ++payment_calls;
    });
    client->SetOnReady([this](const nwc::InfoEvent& i) {
      last_info = i;
      ++ready_calls;
    });
    // Deterministic-but-non-zero RNG so encrypts don't repeat.
    int counter = 0;
    client->SetRandomFn(
        [counter](uint8_t* out, size_t n) mutable {
          for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<uint8_t>((counter * 13u + i) & 0xffu);
          }
          ++counter;
        });
    client->SetNowFn([]() -> int64_t { return 1747000000; });
  }
};

}  // namespace

TEST_CASE("NwcClient::Start subscribes with the right filter") {
  ClientFixture f;
  f.client->Start();
  REQUIRE(f.subscribes.size() == 1);
  const auto& [sub_id, filter] = f.subscribes[0];
  CHECK(sub_id.rfind("nwc-", 0) == 0);
  // Filter kinds cover INFO + responses + both notification variants.
  REQUIRE(filter.kinds.size() == 4);
  CHECK(filter.kinds[0] == 13194u);
  CHECK(filter.kinds[1] == 23195u);
  CHECK(filter.kinds[2] == 23197u);
  CHECK(filter.kinds[3] == 23196u);
  // `#p` is our client pubkey so the wallet's responses to us
  // come back; `authors` pins INFO to come from the wallet pubkey.
  REQUIRE(filter.p_tags.size() == 1);
  CHECK(filter.p_tags[0] == ClientPubkeyHex());
  REQUIRE(filter.authors.size() == 1);
  CHECK(filter.authors[0] == WalletPubkeyHex());
  CHECK(f.client->state() == nwc::State::kBootstrapping);
}

TEST_CASE("NwcClient: INFO event locks in nip44_v2 encryption + fires ready") {
  ClientFixture f;
  f.client->Start();
  auto info = MakeInfoEvent(
      "pay_invoice get_balance notifications", {"nip44_v2", "nip04"},
      {"payment_received", "payment_sent"});
  f.client->HandleEvent(info);
  CHECK(f.client->state() == nwc::State::kReady);
  CHECK(f.client->encryption() == nostr::EncryptionVariant::kNip44V2);
  CHECK(f.ready_calls == 1);
  CHECK(f.last_info.methods.size() == 3);
  CHECK(f.last_info.notifications.size() == 2);
}

TEST_CASE("NwcClient: INFO with only nip04 advertised -> downgrade") {
  ClientFixture f;
  f.client->Start();
  auto info = MakeInfoEvent("get_balance", {"nip04"}, {});
  f.client->HandleEvent(info);
  CHECK(f.client->encryption() == nostr::EncryptionVariant::kNip04);
}

// Extract the JSON event object from a client → relay publish frame:
//   ["EVENT",{"id":"…",…}]
// The Nostr parser is built for relay → client frames (`["EVENT",
// sub-id, {…}]`), so for the publish-side shape we strip the wrapper
// by hand and feed the bare object through `ParseEventObject`.
static bool ExtractPublishEvent(const std::string& frame, nostr::Event& out) {
  const std::string prefix = R"(["EVENT",)";
  if (frame.size() < prefix.size() + 2) return false;
  if (frame.compare(0, prefix.size(), prefix) != 0) return false;
  if (frame.back() != ']') return false;
  const std::string body = frame.substr(prefix.size(),
                                        frame.size() - prefix.size() - 1);
  return nostr::ParseEventObject(body, out);
}

TEST_CASE("NwcClient: RequestGetBalance publishes signed kind-23194 event") {
  ClientFixture f;
  f.client->Start();
  REQUIRE(f.client->RequestGetBalance());
  REQUIRE(f.publishes.size() == 1);
  // Parse the published EVENT publish-side frame and verify the
  // schnorr signature checks out end-to-end.
  nostr::Event published;
  REQUIRE(ExtractPublishEvent(f.publishes[0], published));
  CHECK(published.kind == 23194u);
  CHECK(published.pubkey == ClientPubkeyHex());
  // The schnorr signature must verify against the recomputed id.
  CHECK(nostr::VerifyEvent(published) == nostr::EventVerifyResult::kOk);
  // Expected tags: `p` pointing at the wallet + `encryption` carrying
  // the negotiated variant.
  bool saw_p = false, saw_enc = false;
  for (const auto& tag : published.tags) {
    if (tag.values.size() >= 2 && tag.values[0] == "p") {
      CHECK(tag.values[1] == WalletPubkeyHex());
      saw_p = true;
    }
    if (tag.values.size() >= 2 && tag.values[0] == "encryption") {
      CHECK(tag.values[1] == "nip44_v2");
      saw_enc = true;
    }
  }
  CHECK(saw_p);
  CHECK(saw_enc);
  // In-flight id matches the published event id.
  CHECK(f.client->last_request_id() == published.id);
}

TEST_CASE("NwcClient: kind 23195 response decodes and updates balance cache") {
  ClientFixture f;
  f.client->Start();
  // Advertise INFO first so the cached encryption variant is nip44_v2.
  f.client->HandleEvent(MakeInfoEvent("get_balance", {"nip44_v2"}, {}));
  REQUIRE(f.client->RequestGetBalance());
  const std::string req_id = f.client->last_request_id();

  // Wallet "answers" with the standard get_balance response payload.
  const std::string resp_json =
      R"({"result_type":"get_balance","result":{"balance":4242000}})";
  auto resp = MakeResponse(resp_json, req_id,
                           nostr::EncryptionVariant::kNip44V2);
  f.client->HandleEvent(resp);
  CHECK(f.balance_calls == 1);
  CHECK(f.last_balance == 4242000u);
  CHECK(f.client->balance_msat_cache() == 4242000u);
  // After the response is consumed, no request is in flight.
  CHECK(f.client->last_request_id().empty());
}

TEST_CASE("NwcClient: payment_received notification fires payment callback") {
  ClientFixture f;
  f.client->Start();
  f.client->HandleEvent(MakeInfoEvent("get_balance notifications",
                                      {"nip44_v2"},
                                      {"payment_received", "payment_sent"}));
  f.client->RequestGetBalance();  // populate cache via response
  auto resp = MakeResponse(
      R"({"result_type":"get_balance","result":{"balance":1000000}})",
      f.client->last_request_id(), nostr::EncryptionVariant::kNip44V2);
  f.client->HandleEvent(resp);
  REQUIRE(f.client->balance_msat_cache() == 1000000u);

  const std::string notif_json = R"({
    "notification_type":"payment_received",
    "notification":{"amount":21000,"description":"hi","payment_hash":"abc"}
  })";
  auto notif = MakeNotification(notif_json, 23197);
  f.client->HandleEvent(notif);
  CHECK(f.payment_calls == 1);
  CHECK(f.last_payment.direction == nwc::PaymentDirection::kIncoming);
  CHECK(f.last_payment.amount_msat == 21000u);
  CHECK(f.last_payment.description == "hi");
  // Cached balance optimistically bumped.
  CHECK(f.client->balance_msat_cache() == 1021000u);
}

TEST_CASE("NwcClient: legacy kind 23196 notification decodes via NIP-04") {
  ClientFixture f;
  f.client->Start();
  // Advertise nip04-only — wallet will encrypt 23196 with NIP-04.
  f.client->HandleEvent(MakeInfoEvent("get_balance", {"nip04"}, {}));
  CHECK(f.client->encryption() == nostr::EncryptionVariant::kNip04);

  const std::string notif_json = R"({
    "notification_type":"payment_sent",
    "notification":{"amount":5000,"fees_paid":2,"payment_hash":"x"}
  })";
  auto notif = MakeNotification(notif_json, 23196);
  f.client->HandleEvent(notif);
  CHECK(f.payment_calls == 1);
  CHECK(f.last_payment.direction == nwc::PaymentDirection::kOutgoing);
  CHECK(f.last_payment.amount_msat == 5000u);
  CHECK(f.last_payment.fees_paid_msat == 2u);
}

TEST_CASE("NwcClient: Stop fires unsubscribe + clears in-flight") {
  ClientFixture f;
  f.client->Start();
  f.client->RequestGetBalance();
  REQUIRE(!f.client->last_request_id().empty());
  f.client->Stop();
  REQUIRE(f.unsubscribes.size() == 1);
  CHECK(f.unsubscribes[0] == f.subscribes[0].first);
  CHECK(f.client->state() == nwc::State::kIdle);
  CHECK(f.client->last_request_id().empty());
}
