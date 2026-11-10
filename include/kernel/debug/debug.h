#pragma once
#include <cstdint>
#include <cstddef>

namespace Kernel::Debug {
constexpr size_t LOG_MESSAGE_SIZE = 112;
constexpr size_t LOG_CAPACITY = 128;
struct LogEntry {
  uint64_t timestamp;
  char message[LOG_MESSAGE_SIZE];
};

void init();
void log(const char *message);
void warn(const char *message);
void error(const char *message);
void log_hex(const char *label, uint64_t value);
void log_dec(const char *label, uint64_t value);
size_t log_count();
bool read_log(size_t oldest_index, LogEntry *entry);
} // namespace Kernel::Debug
