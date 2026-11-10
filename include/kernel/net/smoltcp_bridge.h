#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Net {

struct Info {
  uint8_t configured;
  uint8_t prefix_len;
  uint8_t mac[6];
  uint32_t ipv4;
  uint32_t gateway;
  uint32_t dns;
};

struct PingStats {
  uint32_t sent;
  uint32_t received;
  uint32_t received_mask;
  uint32_t rtt_ms[4];
};

int initialize(const uint8_t mac[6], uint64_t seed);
int poll();
int configure(uint32_t timeout_ms);
int get_info(Info *info);
int ping(const char *host, PingStats *stats);
int tcp_open(const char *host, uint16_t port, uint32_t timeout_ms);
int tcp_read(uint8_t *output, size_t capacity, uint32_t timeout_ms);
int tcp_write(const uint8_t *data, size_t length);
int tcp_close();
int tcp_echo(const char *host, uint16_t port, const char *message, char *output,
             size_t output_capacity, uint32_t timeout_ms);
int fetch(const char *url, char *output, size_t output_capacity);

} // namespace Kernel::Net
