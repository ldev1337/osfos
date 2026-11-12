#include <kernel/drivers/net/device.h>
#include <kernel/drivers/pit.h>
#include <kernel/net/smoltcp_bridge.h>
#include <kernel/scheduler.h>

extern "C" {
int32_t osfos_net_init(const uint8_t *mac, uint64_t seed);
int32_t osfos_net_poll();
int32_t osfos_net_configure(uint32_t timeout_ms);
int32_t osfos_net_info(Kernel::Net::Info *info);
int32_t osfos_net_ping(const char *host, Kernel::Net::PingStats *stats);
int32_t osfos_net_tcp_open(const char *host, uint16_t port,
                           uint32_t timeout_ms);
int32_t osfos_net_tcp_read(uint8_t *output, size_t capacity,
                           uint32_t timeout_ms);
int32_t osfos_net_tcp_write(const uint8_t *data, size_t length);
int32_t osfos_net_tcp_close();
int32_t osfos_net_tcp_echo(const char *host, uint16_t port,
                           const uint8_t *message, size_t message_length,
                           uint8_t *output, size_t output_capacity,
                           uint32_t timeout_ms);

int32_t os_net_device_receive(uint8_t *buffer, size_t capacity) {
  return Kernel::Drivers::Net::receive(buffer, capacity);
}

bool os_net_device_send(const uint8_t *buffer, size_t length) {
  return Kernel::Drivers::Net::send(buffer, length);
}

uint64_t os_net_clock_ms() {
  return static_cast<uint64_t>(Kernel::Drivers::PIT::get_ticks()) * 4;
}

void os_net_yield() { Kernel::Scheduler::thread_sleep_ms(1); }
}

namespace Kernel::Net {

int initialize(const uint8_t mac[6], uint64_t seed) {
  return osfos_net_init(mac, seed);
}

int poll() { return osfos_net_poll(); }

int configure(uint32_t timeout_ms) { return osfos_net_configure(timeout_ms); }

int get_info(Info *info) { return osfos_net_info(info); }

int ping(const char *host, PingStats *stats) {
  return osfos_net_ping(host, stats);
}

int tcp_open(const char *host, uint16_t port, uint32_t timeout_ms) {
  return osfos_net_tcp_open(host, port, timeout_ms);
}

int tcp_read(uint8_t *output, size_t capacity, uint32_t timeout_ms) {
  return osfos_net_tcp_read(output, capacity, timeout_ms);
}

int tcp_write(const uint8_t *data, size_t length) {
  return osfos_net_tcp_write(data, length);
}

int tcp_close() { return osfos_net_tcp_close(); }

int tcp_echo(const char *host, uint16_t port, const char *message, char *output,
             size_t output_capacity, uint32_t timeout_ms) {
  size_t message_length = 0;
  while (message[message_length] != '\0') {
    ++message_length;
  }
  return osfos_net_tcp_echo(
      host, port, reinterpret_cast<const uint8_t *>(message), message_length,
      reinterpret_cast<uint8_t *>(output), output_capacity, timeout_ms);
}

} // namespace Kernel::Net
