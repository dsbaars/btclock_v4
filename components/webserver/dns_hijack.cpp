#include "provisioning_server.hpp"

#include <arpa/inet.h>
#include <lwip/sockets.h>
#include <sys/socket.h>

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {
namespace {
constexpr const char* kTag = "dns";
}  // namespace

DnsHijack::DnsHijack(uint32_t target_ip) : target_ip_(target_ip) {}

DnsHijack::~DnsHijack() {
  stop_ = true;
  if (sock_ >= 0) {
    ::shutdown(sock_, 0);
    ::close(sock_);
    sock_ = -1;
  }
  if (task_) {
    vTaskDelay(pdMS_TO_TICKS(50));   // let the task exit cleanly
  }
}

esp_err_t DnsHijack::Start() {
  sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock_ < 0) return ESP_FAIL;

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(kTag, "bind(53) failed: %s", strerror(errno));
    ::close(sock_);
    sock_ = -1;
    return ESP_FAIL;
  }

  if (xTaskCreate(TaskTrampoline, "dns_hijack", 4096, this,
                  tskIDLE_PRIORITY + 1, &task_) != pdPASS) {
    ::close(sock_);
    sock_ = -1;
    return ESP_FAIL;
  }
  ESP_LOGI(kTag, "DNS hijack up on UDP 53 -> " IPSTR,
           static_cast<unsigned>(target_ip_ & 0xFF),
           static_cast<unsigned>((target_ip_ >> 8) & 0xFF),
           static_cast<unsigned>((target_ip_ >> 16) & 0xFF),
           static_cast<unsigned>((target_ip_ >> 24) & 0xFF));
  return ESP_OK;
}

void DnsHijack::TaskTrampoline(void* arg) {
  static_cast<DnsHijack*>(arg)->Run();
  vTaskDelete(nullptr);
}

void DnsHijack::Run() {
  uint8_t buf[512];
  sockaddr_in peer = {};
  socklen_t plen = sizeof(peer);
  while (!stop_) {
    const int n = ::recvfrom(sock_, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&peer), &plen);
    if (n < 12) continue;   // DNS header is 12 bytes

    // Turn the request into a response with one A record answer:
    //   set QR=1 (response), RA=1, RCODE=0
    //   ANCOUNT=1, copy question, append pointer-compressed answer.
    buf[2] = 0x81;   // QR=1 RD copied
    buf[3] = 0x80;   // RA=1
    buf[4] = 0x00; buf[5] = 0x01;  // QDCOUNT = 1
    buf[6] = 0x00; buf[7] = 0x01;  // ANCOUNT = 1
    buf[8] = 0x00; buf[9] = 0x00;  // NSCOUNT
    buf[10] = 0x00; buf[11] = 0x00;// ARCOUNT

    // Walk to the end of the question section (after QNAME + QTYPE + QCLASS).
    int p = 12;
    while (p < n && buf[p] != 0) {
      if (buf[p] & 0xC0) break;    // compressed label — shouldn't happen in a query
      p += 1 + buf[p];
    }
    if (p >= n) continue;
    p += 1 + 4;                    // skip null label + QTYPE(2) + QCLASS(2)
    if (p + 16 > static_cast<int>(sizeof(buf))) continue;

    // Answer: name pointer to offset 12, TYPE A, CLASS IN, TTL 60, RDLENGTH 4, RDATA
    buf[p++] = 0xC0; buf[p++] = 0x0C;        // pointer to QNAME
    buf[p++] = 0x00; buf[p++] = 0x01;        // TYPE A
    buf[p++] = 0x00; buf[p++] = 0x01;        // CLASS IN
    buf[p++] = 0x00; buf[p++] = 0x00;        // TTL hi
    buf[p++] = 0x00; buf[p++] = 0x3C;        // TTL lo = 60 s
    buf[p++] = 0x00; buf[p++] = 0x04;        // RDLENGTH = 4
    buf[p++] = static_cast<uint8_t>(target_ip_ & 0xFF);
    buf[p++] = static_cast<uint8_t>((target_ip_ >> 8) & 0xFF);
    buf[p++] = static_cast<uint8_t>((target_ip_ >> 16) & 0xFF);
    buf[p++] = static_cast<uint8_t>((target_ip_ >> 24) & 0xFF);

    ::sendto(sock_, buf, p, 0, reinterpret_cast<sockaddr*>(&peer), plen);
  }
}

}  // namespace btclock
