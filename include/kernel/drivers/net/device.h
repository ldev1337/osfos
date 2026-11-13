#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers::Net {

struct Device {
  bool (*start)();
  bool (*send)(const void *, size_t);
  int (*receive)(void *, size_t);
  void (*get_mac)(uint8_t[6]);
  const char *name;
};

bool register_device(const Device *device);
const Device *active_device();
bool detect_and_init();
bool start();
bool send(const void *data, size_t length);
int receive(void *buffer, size_t capacity);
void get_mac_address(uint8_t mac[6]);

} // namespace Kernel::Drivers::Net
