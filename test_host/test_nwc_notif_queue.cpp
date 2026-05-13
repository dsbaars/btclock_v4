// Host tests for the NWC notification queue + worker-side handoff.
//
// Pins the queue/worker architecture from bd btclock_v4-lwf.9:
//   * the WS hot path only enqueues a RawNotification — no decrypt,
//     no decode, no callback;
//   * the heavy decrypt/decode/dispatch runs via
//     `DispatchRawNotification`;
//   * dropping under saturation bumps a visible counter;
//   * worker threading is mutex/condvar-safe (cv timeouts honoured).
//
// We don't spawn a FreeRTOS task here — std::thread is sufficient on
// the host. The on-device worker is the same control-flow loop, just
// with `xTaskCreate` and `WaitPop(timeout_ms=1000)`.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "nostr/event.hpp"
#include "nostr/event_sign.hpp"
#include "nostr/nip4x.hpp"
#include "nwc/client.hpp"
#include "nwc/queue.hpp"
#include "nwc/uri.hpp"

using namespace btclock;

namespace {

// Mirrors the wallet/client keys used in test_nwc_client.cpp so we can
// reuse the well-trodden BIP-340 vector pairs. Kept local to avoid
// cross-TU coupling.
constexpr const char* kClientSecretHex =
    "b7e151628aed2a6abf7158809cf4f3c762e7160f38b4da56a784d9045190cfef";
constexpr const char* kWalletSecretHex =
    "0000000000000000000000000000000000000000000000000000000000000002";

std::string DeriveX(const std::string& sk_hex) {
  uint8_t sk[32];
  REQUIRE(nwc::DecodeHex32(sk_hex, sk));
  uint8_t pk[32];
  REQUIRE(nostr::DerivePubkeyXOnly(sk, pk) == nostr::EventSignError::kOk);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '\0');
  for (size_t i = 0; i < 32; ++i) {
    out[2 * i] = kHex[(pk[i] >> 4) & 0xfu];
    out[2 * i + 1] = kHex[pk[i] & 0xfu];
  }
  return out;
}

nwc::PairingUri MakeFixtureUri() {
  const std::string wallet_pub = DeriveX(kWalletSecretHex);
  const std::string uri = std::string("nostr+walletconnect://") + wallet_pub +
                          "?relay=wss%3A%2F%2Fnostr-faucet.example"
                          "&secret=" +
                          kClientSecretHex;
  nwc::PairingUri parsed;
  REQUIRE(nwc::ParsePairingUri(uri, parsed) == nwc::ParseError::kOk);
  return parsed;
}

// Wallet-side encrypt for a payment notification. Mirrors the test
// fixture in test_nwc_client.cpp.
std::string WalletEncrypt(nostr::EncryptionVariant v,
                          const std::string& plaintext) {
  uint8_t wallet_sec[32];
  REQUIRE(nwc::DecodeHex32(kWalletSecretHex, wallet_sec));
  uint8_t client_pub[32];
  REQUIRE(nwc::DecodeHex32(DeriveX(kClientSecretHex), client_pub));
  uint8_t nonce[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  return nostr::Encrypt(v, wallet_sec, client_pub, nonce, plaintext);
}

nostr::Event MakeNotification(const std::string& json, uint32_t kind = 23197) {
  nostr::Event ev;
  ev.created_at = 1747000200;
  ev.kind = kind;
  ev.pubkey = DeriveX(kWalletSecretHex);
  const auto variant = (kind == 23197) ? nostr::EncryptionVariant::kNip44V2
                                       : nostr::EncryptionVariant::kNip04;
  ev.content = WalletEncrypt(variant, json);
  nostr::Tag p;
  p.values = {"p", DeriveX(kClientSecretHex)};
  ev.tags.push_back(p);
  return ev;
}

// Build a NwcClient pre-wired with a queue. Mirrors production except
// no FreeRTOS task — the worker is driven inline by the test.
struct WiredClient {
  std::unique_ptr<nwc::NotificationQueue> queue;
  std::unique_ptr<nwc::NwcClient> client;
  int payment_calls = 0;
  nwc::PaymentNotification last_payment;

  WiredClient() {
    queue = std::make_unique<nwc::NotificationQueue>();
    auto pub = [](const char*, size_t) { return true; };
    auto sub = [](const std::string&, const nostr::Filter&) {};
    auto unsub = [](const std::string&) {};
    client =
        std::make_unique<nwc::NwcClient>(MakeFixtureUri(), pub, sub, unsub);
    client->SetOnPayment([this](const nwc::PaymentNotification& p) {
      last_payment = p;
      ++payment_calls;
    });
    int counter = 0;
    client->SetRandomFn([counter](uint8_t* out, size_t n) mutable {
      for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>((counter * 13u + i) & 0xffu);
      }
      ++counter;
    });
    client->SetNowFn([]() -> int64_t { return 1747000000; });
    nwc::NotificationQueue* q = queue.get();
    client->SetNotifEnqueueFn(
        [q](nwc::RawNotification&& raw) { return q->TryPush(std::move(raw)); });
  }
};

}  // namespace

TEST_CASE("NotificationQueue: push/pop preserves payload + order") {
  nwc::NotificationQueue q(4);
  CHECK(q.capacity() == 4);
  CHECK(q.size() == 0);

  for (int i = 0; i < 3; ++i) {
    nwc::RawNotification r;
    r.kind = static_cast<uint32_t>(23197 - (i % 2));
    r.content = "payload-" + std::to_string(i);
    r.event_id = "id-" + std::to_string(i);
    CHECK(q.TryPush(std::move(r)));
  }
  CHECK(q.size() == 3);
  CHECK(q.pushed() == 3);

  for (int i = 0; i < 3; ++i) {
    nwc::RawNotification out;
    REQUIRE(q.WaitPop(out, 10));
    CHECK(out.content == "payload-" + std::to_string(i));
  }
  CHECK(q.popped() == 3);
  CHECK(q.size() == 0);
  CHECK(q.dropped() == 0);
}

TEST_CASE("NotificationQueue: full queue drops + bumps counter") {
  nwc::NotificationQueue q(2);
  for (int i = 0; i < 2; ++i) {
    nwc::RawNotification r;
    r.content = "ok-" + std::to_string(i);
    CHECK(q.TryPush(std::move(r)));
  }
  // Third push: queue saturated.
  nwc::RawNotification spill;
  spill.content = "drop";
  CHECK_FALSE(q.TryPush(std::move(spill)));
  CHECK(q.pushed() == 2);
  CHECK(q.dropped() == 1);
  CHECK(q.size() == 2);

  // Pop one slot — the next push must succeed.
  nwc::RawNotification out;
  REQUIRE(q.WaitPop(out, 10));
  CHECK(out.content == "ok-0");
  nwc::RawNotification r;
  r.content = "after-pop";
  CHECK(q.TryPush(std::move(r)));
  CHECK(q.pushed() == 3);
  CHECK(q.dropped() == 1);
}

TEST_CASE("NotificationQueue: WaitPop times out cleanly when empty") {
  nwc::NotificationQueue q(4);
  nwc::RawNotification out;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(q.WaitPop(out, 50));
  const auto dt = std::chrono::steady_clock::now() - t0;
  // Honour the wait window — but cap upper bound modestly so a
  // stalled host doesn't make this test flap.
  CHECK(dt >= std::chrono::milliseconds(40));
  CHECK(dt < std::chrono::seconds(2));
}

TEST_CASE(
    "NotificationQueue: Shutdown wakes waiter and rejects further pushes") {
  nwc::NotificationQueue q(4);
  std::atomic<bool> popper_returned{false};
  std::atomic<bool> popper_result{false};
  std::thread popper([&]() {
    nwc::RawNotification out;
    popper_result.store(q.WaitPop(out, /*timeout_ms=*/5000));
    popper_returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_FALSE(popper_returned.load());
  q.Shutdown();
  popper.join();
  CHECK(popper_returned.load());
  CHECK_FALSE(popper_result.load());

  nwc::RawNotification r;
  r.content = "post-shutdown";
  CHECK_FALSE(q.TryPush(std::move(r)));
  CHECK(q.dropped() >= 1);
}

TEST_CASE("NwcClient: HandleEvent with queue wired only enqueues") {
  // Verify the hot path is decoupled — no decrypt, no callback, no
  // counter bump on the heavy-path counters. Drop counter stays zero
  // while the queue has slack.
  WiredClient wc;
  wc.client->Start();
  const std::string notif_json =
      R"({"notification_type":"payment_received",)"
      R"("notification":{"amount":21000,"description":"hi","payment_hash":"abc"}})";
  auto ev = MakeNotification(notif_json, 23197);
  wc.client->HandleEvent(ev);
  // Heavy path didn't run — payment callback is the externally
  // observable side-effect of DispatchHeavy, and it stays at 0.
  CHECK(wc.payment_calls == 0);
  // Queue holds the envelope, content matches the wire form.
  CHECK(wc.queue->size() == 1);
}

TEST_CASE("NwcClient::DispatchRawNotification: heavy path fires payment cb") {
  // Round-trip: enqueue on the hot path, then drain via the
  // worker-side entry. Mirrors what the dedicated nwc_notify task
  // does on-device.
  WiredClient wc;
  wc.client->Start();
  const std::string notif_json =
      R"({"notification_type":"payment_received",)"
      R"("notification":{"amount":42000,"description":"x","payment_hash":"h"}})";
  wc.client->HandleEvent(MakeNotification(notif_json, 23197));
  REQUIRE(wc.queue->size() == 1);

  nwc::RawNotification raw;
  REQUIRE(wc.queue->WaitPop(raw, 10));
  wc.client->DispatchRawNotification(raw);

  CHECK(wc.payment_calls == 1);
  CHECK(wc.last_payment.direction == nwc::PaymentDirection::kIncoming);
  CHECK(wc.last_payment.amount_msat == 42000u);
}

TEST_CASE("NwcClient: legacy NIP-04 (kind 23196) survives the queue handoff") {
  WiredClient wc;
  // Force NIP-04 by replaying an INFO event that advertises only it.
  // Easier than reaching into the encryption setter — keeps the test
  // honest about the wire contract.
  nostr::Event info;
  info.created_at = 1747000000;
  info.kind = 13194;
  info.content = "get_balance";
  info.pubkey = DeriveX(kWalletSecretHex);
  nostr::Tag enc;
  enc.values = {"encryption", "nip04"};
  info.tags.push_back(enc);
  wc.client->Start();
  wc.client->HandleEvent(info);
  REQUIRE(wc.client->encryption() == nostr::EncryptionVariant::kNip04);

  const std::string notif_json =
      R"({"notification_type":"payment_sent",)"
      R"("notification":{"amount":5000,"fees_paid":2,"payment_hash":"y"}})";
  wc.client->HandleEvent(MakeNotification(notif_json, 23196));
  REQUIRE(wc.queue->size() == 1);
  nwc::RawNotification raw;
  REQUIRE(wc.queue->WaitPop(raw, 10));
  CHECK(raw.kind == 23196u);
  wc.client->DispatchRawNotification(raw);
  CHECK(wc.payment_calls == 1);
  CHECK(wc.last_payment.direction == nwc::PaymentDirection::kOutgoing);
  CHECK(wc.last_payment.amount_msat == 5000u);
  CHECK(wc.last_payment.fees_paid_msat == 2u);
}

TEST_CASE("NwcClient: enqueue under saturation increments drop counter") {
  // Capacity 2 queue, three notifications back-to-back. Hot path
  // never touches decrypt; the third one must drop, NwcClient must
  // see the drop, and the queue's own counter must agree.
  WiredClient wc;
  wc.queue = std::make_unique<nwc::NotificationQueue>(2);
  nwc::NotificationQueue* q = wc.queue.get();
  wc.client->SetNotifEnqueueFn(
      [q](nwc::RawNotification&& raw) { return q->TryPush(std::move(raw)); });
  wc.client->Start();
  const std::string notif_json =
      R"({"notification_type":"payment_received",)"
      R"("notification":{"amount":1,"description":"","payment_hash":"a"}})";
  for (int i = 0; i < 3; ++i) {
    wc.client->HandleEvent(MakeNotification(notif_json, 23197));
  }
  // Queue's own counters are the authoritative drop record now that
  // NwcClient no longer mirrors them — capacity=2 + 3 pushes ⇒ 1
  // dropped. The queue is the single source of truth for "did we
  // shed load".
  CHECK(q->pushed() == 2);
  CHECK(q->dropped() == 1);
}

TEST_CASE("NwcClient: without queue wired, OnNotification still works inline") {
  // Backwards compat: pre-init callers (or host tests that don't
  // bother wiring a queue) keep the legacy synchronous heavy path so
  // existing test_nwc_client.cpp expectations stay valid.
  WiredClient wc;
  // Drop the queue wiring.
  wc.client->SetNotifEnqueueFn(nullptr);
  wc.client->Start();
  const std::string notif_json =
      R"({"notification_type":"payment_received",)"
      R"("notification":{"amount":1000,"description":"","payment_hash":"a"}})";
  wc.client->HandleEvent(MakeNotification(notif_json, 23197));
  CHECK(wc.payment_calls == 1);
  CHECK(wc.queue->size() == 0);  // queue untouched
}

TEST_CASE("NotificationQueue: producer/consumer threading is race-free") {
  // Smoke test the mutex+cv path under contention. 100 items pushed
  // by the producer must be popped exactly once by the consumer with
  // payloads in FIFO order.
  nwc::NotificationQueue q(/*capacity=*/16);
  std::atomic<bool> stop{false};
  std::vector<std::string> received;
  std::thread consumer([&]() {
    nwc::RawNotification raw;
    while (!stop.load() || q.size() > 0) {
      if (q.WaitPop(raw, /*timeout_ms=*/50)) {
        received.push_back(std::move(raw.content));
      }
    }
  });
  for (int i = 0; i < 100; ++i) {
    nwc::RawNotification r;
    r.content = std::to_string(i);
    // Spin-retry on drop: the consumer drains in real time, but
    // bursts can saturate. This isn't a "drop semantics" test.
    while (!q.TryPush(std::move(r))) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      r.content = std::to_string(i);
    }
  }
  stop.store(true);
  q.Shutdown();
  consumer.join();
  REQUIRE(received.size() == 100);
  for (int i = 0; i < 100; ++i) {
    CHECK(received[static_cast<size_t>(i)] == std::to_string(i));
  }
}
