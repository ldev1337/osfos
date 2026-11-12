#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Drivers::Net::RTL8139 {
bool detect_and_init();
bool start();
bool send(const void *data, size_t length);
int receive(void *buffer, size_t capacity);
void get_mac_address(uint8_t mac[6]);
} // namespace Kernel::Drivers::Net::RTL8139
