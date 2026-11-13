#include <kernel/drivers/net/device.h>

#include <kernel/drivers/e1000.h>
#include <kernel/drivers/net/rtl8139.h>
#include <kernel/drivers/net/rtl8168.h>

namespace Kernel::Drivers::Net {
namespace {
const Device *s_active;
const Device s_e1000 = {E1000::start, E1000::send, E1000::receive,
                        E1000::get_mac_address, "e1000"};
const Device s_rtl8168 = {RTL8168::start, RTL8168::send, RTL8168::receive,
                          RTL8168::get_mac_address, "rtl816x"};
const Device s_rtl8139 = {RTL8139::start, RTL8139::send, RTL8139::receive,
                          RTL8139::get_mac_address, "rtl8139"};
} // namespace

bool register_device(const Device *device) {
  if (!device || !device->send || !device->receive || !device->get_mac)
    return false;
  s_active = device;
  return true;
}

const Device *active_device() { return s_active; }

bool detect_and_init() {
  s_active = nullptr;
  if (RTL8168::detect_and_init())
    return register_device(&s_rtl8168);
  if (RTL8139::detect_and_init())
    return register_device(&s_rtl8139);
  if (E1000::detect_and_init())
    return register_device(&s_e1000);
  return false;
}

bool start() { return s_active && (!s_active->start || s_active->start()); }
bool send(const void *data, size_t length) {
  return s_active && s_active->send(data, length);
}
int receive(void *buffer, size_t capacity) {
  return s_active ? s_active->receive(buffer, capacity) : 0;
}
void get_mac_address(uint8_t mac[6]) {
  if (!mac)
    return;
  if (s_active)
    s_active->get_mac(mac);
  else
    for (size_t i = 0; i < 6; ++i)
      mac[i] = 0;
}
} // namespace Kernel::Drivers::Net
