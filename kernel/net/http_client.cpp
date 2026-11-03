#include <bearssl.h>
#include <kernel/cpu.h>
#include <kernel/net/smoltcp_bridge.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" const br_x509_trust_anchor osfos_https_trust_anchors[2];

namespace Kernel::Net {
namespace {
constexpr size_t HOST_CAPACITY = 128;
constexpr size_t PATH_CAPACITY = 256;
constexpr size_t REQUEST_CAPACITY = 512;
constexpr size_t READ_CHUNK_SIZE = 512;
constexpr uint32_t NETWORK_TIMEOUT_MS = 15000;

struct Url {
  char host[HOST_CAPACITY];
  char path[PATH_CAPACITY];
  uint16_t port;
  bool https;
};

br_ssl_client_context tls_client;
br_x509_minimal_context tls_x509;
br_sslio_context tls_io;
unsigned char tls_buffer[BR_SSL_BUFSIZE_BIDI];
int tls_transport_failure;

bool starts_with(const char *text, const char *prefix) {
  while (*prefix) {
    if (*text++ != *prefix++)
      return false;
  }
  return true;
}

bool parse_port(const char *text, uint16_t &port) {
  uint32_t value = 0;
  if (!*text)
    return false;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9')
      return false;
    value = value * 10 + static_cast<uint32_t>(*text - '0');
    if (value > 65535)
      return false;
  }
  if (!value)
    return false;
  port = static_cast<uint16_t>(value);
  return true;
}

bool parse_url(const char *text, Url &url) {
  if (!text)
    return false;
  const char *authority;
  if (starts_with(text, "http://")) {
    url.https = false;
    url.port = 80;
    authority = text + 7;
  } else if (starts_with(text, "https://")) {
    url.https = true;
    url.port = 443;
    authority = text + 8;
  } else {
    return false;
  }

  const char *path = authority;
  while (*path && *path != '/' && *path != '?' && *path != '#')
    ++path;
  if (path == authority || *authority == '[')
    return false;

  const char *host_end = path;
  const char *port_start = nullptr;
  for (const char *cursor = authority; cursor < path; ++cursor) {
    if (*cursor == '@' || static_cast<unsigned char>(*cursor) <= 32)
      return false;
    if (*cursor == ':') {
      if (port_start)
        return false;
      host_end = cursor;
      port_start = cursor + 1;
    } else if (!((*cursor >= 'a' && *cursor <= 'z') ||
                 (*cursor >= 'A' && *cursor <= 'Z') ||
                 (*cursor >= '0' && *cursor <= '9') || *cursor == '.' ||
                 *cursor == '-')) {
      return false;
    }
  }
  size_t host_length = static_cast<size_t>(host_end - authority);
  if (!host_length || host_length >= sizeof(url.host))
    return false;
  memcpy(url.host, authority, host_length);
  url.host[host_length] = '\0';
  if (port_start && !parse_port(port_start, url.port))
    return false;

  size_t path_length = 0;
  if (*path == '?' || *path == '#')
    url.path[path_length++] = '/';
  while (*path && *path != '#') {
    unsigned char ch = static_cast<unsigned char>(*path++);
    if (ch < 33 || ch > 126 || path_length + 1 >= sizeof(url.path))
      return false;
    url.path[path_length++] = static_cast<char>(ch);
  }
  if (!path_length) {
    url.path[path_length++] = '/';
  }
  url.path[path_length] = '\0';
  return true;
}

bool append_text(char *buffer, size_t capacity, size_t &length,
                 const char *text) {
  size_t text_length = strlen(text);
  if (text_length >= capacity - length)
    return false;
  memcpy(buffer + length, text, text_length);
  length += text_length;
  return true;
}

bool append_url_authority(char *buffer, size_t capacity, size_t &length,
                          const Url &url) {
  if (!append_text(buffer, capacity, length, url.host))
    return false;
  if ((!url.https && url.port != 80) || (url.https && url.port != 443)) {
    char digits[5];
    size_t digit_count = 0;
    uint16_t port = url.port;
    do {
      digits[digit_count++] = static_cast<char>('0' + port % 10);
      port /= 10;
    } while (port);
    if (!append_text(buffer, capacity, length, ":"))
      return false;
    while (digit_count) {
      char digit[2] = {digits[--digit_count], '\0'};
      if (!append_text(buffer, capacity, length, digit))
        return false;
    }
  }
  return true;
}

bool make_request(const Url &url, char *request, size_t capacity,
                  size_t &length) {
  length = 0;
  return append_text(request, capacity, length, "GET ") &&
         append_text(request, capacity, length, url.path) &&
         append_text(request, capacity, length, " HTTP/1.0\r\nHost: ") &&
         append_url_authority(request, capacity, length, url) &&
         append_text(request, capacity, length,
                     "\r\nConnection: close\r\nAccept: */*\r\n"
                     "User-Agent: osfos/0.1\r\n\r\n");
}

bool random_word(uint32_t &value) {
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    unsigned char success;
    __asm__ volatile("rdrand %0; setc %1"
                     : "=r"(value), "=qm"(success)
                     :
                     : "cc");
    if (success)
      return true;
  }
  return false;
}

bool fill_tls_entropy(unsigned char entropy[32]) {
  uint32_t eax, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                   : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                   : "a"(1), "c"(0));
  if ((ecx & (1u << 30)) == 0)
    return false;
  for (size_t offset = 0; offset < 32; offset += sizeof(uint32_t)) {
    uint32_t value;
    if (!random_word(value))
      return false;
    memcpy(entropy + offset, &value, sizeof(value));
  }
  return true;
}

uint8_t read_cmos(uint8_t reg) {
  outb(0x70, reg);
  return inb(0x71);
}

uint8_t from_bcd(uint8_t value) {
  return static_cast<uint8_t>((value & 0x0F) + ((value >> 4) * 10));
}

struct RtcFields {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t day;
  uint8_t month;
  uint8_t year;
  uint8_t century;
  uint8_t status_b;
};

bool read_rtc_fields(RtcFields &fields) {
  for (unsigned wait = 0; wait < 100000; ++wait) {
    if ((read_cmos(0x0A) & 0x80) == 0)
      break;
    if (wait == 99999)
      return false;
  }
  if ((read_cmos(0x0D) & 0x80) == 0)
    return false;
  fields.second = read_cmos(0x00);
  fields.minute = read_cmos(0x02);
  fields.hour = read_cmos(0x04);
  fields.day = read_cmos(0x07);
  fields.month = read_cmos(0x08);
  fields.year = read_cmos(0x09);
  fields.century = read_cmos(0x32);
  fields.status_b = read_cmos(0x0B);
  return fields.second == read_cmos(0x00);
}

bool rtc_validation_time(uint32_t &days, uint32_t &seconds) {
  RtcFields fields{};
  bool stable = false;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    RtcFields first{}, second{};
    if (read_rtc_fields(first) && read_rtc_fields(second) &&
        first.second == second.second && first.minute == second.minute &&
        first.hour == second.hour && first.day == second.day &&
        first.month == second.month && first.year == second.year &&
        first.century == second.century) {
      fields = second;
      stable = true;
      break;
    }
  }
  if (!stable)
    return false;

  const bool binary = (fields.status_b & 0x04) != 0;
  const bool hour24 = (fields.status_b & 0x02) != 0;
  const bool pm = (fields.hour & 0x80) != 0;
  fields.hour &= 0x7F;
  if (!binary) {
    fields.second = from_bcd(fields.second);
    fields.minute = from_bcd(fields.minute);
    fields.hour = from_bcd(fields.hour);
    fields.day = from_bcd(fields.day);
    fields.month = from_bcd(fields.month);
    fields.year = from_bcd(fields.year);
    fields.century = from_bcd(fields.century);
  }
  if (!hour24) {
    fields.hour %= 12;
    if (pm)
      fields.hour += 12;
  }

  uint32_t year =
      fields.century >= 19 && fields.century <= 99
          ? static_cast<uint32_t>(fields.century) * 100 + fields.year
          : 2000u + fields.year;
  if (year < 2020 || year > 2099 || fields.month < 1 || fields.month > 12 ||
      fields.day < 1 || fields.day > 31 || fields.hour > 23 ||
      fields.minute > 59 || fields.second > 60)
    return false;

  uint64_t total_days = 0;
  for (uint32_t current = 0; current < year; ++current) {
    const bool leap =
        current % 4 == 0 && (current % 100 != 0 || current % 400 == 0);
    total_days += leap ? 366 : 365;
  }
  static constexpr uint8_t month_lengths[12] = {31, 28, 31, 30, 31, 30,
                                                31, 31, 30, 31, 30, 31};
  for (uint8_t month = 1; month < fields.month; ++month) {
    total_days += month_lengths[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
      ++total_days;
  }
  uint8_t max_day = month_lengths[fields.month - 1];
  if (fields.month == 2 && year % 4 == 0 &&
      (year % 100 != 0 || year % 400 == 0))
    ++max_day;
  if (fields.day > max_day)
    return false;
  total_days += fields.day - 1;
  if (total_days > UINT32_MAX)
    return false;
  days = static_cast<uint32_t>(total_days);
  seconds = static_cast<uint32_t>(fields.hour) * 3600 +
            static_cast<uint32_t>(fields.minute) * 60 + fields.second;
  return true;
}

int tls_read(void *, unsigned char *data, size_t length) {
  int result = tcp_read(data, length, NETWORK_TIMEOUT_MS);
  if (result > 0)
    return result;
  tls_transport_failure = result == 0 ? 2 : 1;
  return -1;
}

int tls_write(void *, const unsigned char *data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    size_t chunk = length - offset;
    if (chunk > 1024)
      chunk = 1024;
    int result = tcp_write(data + offset, chunk);
    if (result <= 0) {
      tls_transport_failure = 3;
      return -1;
    }
    offset += static_cast<size_t>(result);
  }
  return static_cast<int>(offset);
}

bool response_has_content_length(const char *response, size_t length,
                                 size_t &response_length) {
  size_t header_end = 0;
  while (header_end + 3 < length &&
         (response[header_end] != '\r' || response[header_end + 1] != '\n' ||
           response[header_end + 2] != '\r' || response[header_end + 3] != '\n'))
    ++header_end;
  if (header_end + 3 >= length)
    return false;
  header_end += 4;

  size_t line = 0;
  while (line + 1 < header_end) {
    size_t line_end = line;
    while (line_end + 1 < header_end &&
           (response[line_end] != '\r' || response[line_end + 1] != '\n'))
      ++line_end;
    if (line_end + 1 >= header_end || line_end == line)
      break;

    constexpr char header_name[] = "content-length:";
    if (line_end - line >= sizeof(header_name) - 1) {
      bool matches = true;
      for (size_t index = 0; index < sizeof(header_name) - 1; ++index) {
        char ch = response[line + index];
        if (ch >= 'A' && ch <= 'Z')
          ch = static_cast<char>(ch - 'A' + 'a');
        if (ch != header_name[index]) {
          matches = false;
          break;
        }
      }
      if (matches) {
        size_t cursor = line + sizeof(header_name) - 1;
        while (cursor < line_end &&
               (response[cursor] == ' ' || response[cursor] == '\t'))
          ++cursor;
        size_t body_length = 0;
        if (cursor == line_end)
          return false;
        for (; cursor < line_end; ++cursor) {
          if (response[cursor] < '0' || response[cursor] > '9')
            return false;
          const auto digit = static_cast<size_t>(response[cursor] - '0');
          if (body_length > (static_cast<size_t>(-1) - digit) / 10)
            return false;
          body_length = body_length * 10 + digit;
        }
        if (body_length > static_cast<size_t>(-1) - header_end)
          return false;
        response_length = header_end + body_length;
        return true;
      }
    }
    line = line_end + 2;
  }
  return false;
}

bool response_is_complete(const char *response, size_t length) {
  size_t expected_length = 0;
  return response_has_content_length(response, length, expected_length) &&
         length >= expected_length;
}

int read_response(bool https, char *output, size_t capacity) {
  size_t total = 0;
  unsigned char chunk[READ_CHUNK_SIZE];
  while (total + 1 < capacity) {
    if (response_is_complete(output, total))
      break;
    size_t request_length = capacity - total - 1;
    request_length = std::min(request_length, sizeof(chunk));
    int count;
    if (https) {
      count = br_sslio_read(&tls_io, chunk, request_length);
    } else {
      count = tcp_read(chunk, request_length, NETWORK_TIMEOUT_MS);
    }
    if (count == 0)
      break;
    if (count < 0) {
      if (https && br_ssl_engine_last_error(&tls_client.eng) == 0)
        break;
      if (response_is_complete(output, total))
        break;
      if (!https && total != 0)
        break;
      if (https)
        return -2000 - tls_transport_failure * 100 -
               br_ssl_engine_last_error(&tls_client.eng);
      return -5;
    }
    memcpy(output + total, chunk, static_cast<size_t>(count));
    total += static_cast<size_t>(count);
    if (response_is_complete(output, total))
      break;
  }
  output[total] = '\0';
  return static_cast<int>(total);
}

} // namespace

int fetch(const char *url_text, char *output, size_t output_capacity) {
  if (!output || output_capacity < 2)
    return -1;

  if (url_text == nullptr)
    return -1;
  while (*url_text == ' ' || *url_text == '\t')
    ++url_text;
  size_t url_length = strlen(url_text);
  while ((url_length != 0U) && (url_text[url_length - 1] == ' ' ||
                        url_text[url_length - 1] == '\t' ||
                        url_text[url_length - 1] == '\r' ||
                        url_text[url_length - 1] == '\n'))
    --url_length;
  char normalized_url[512];
  if ((url_length == 0U) || url_length >= sizeof(normalized_url))
    return -8;
  memcpy(normalized_url, url_text, url_length);
  normalized_url[url_length] = '\0';

  Url url{};
  if (!parse_url(normalized_url, url))
    return -8;
  int result = tcp_open(url.host, url.port, NETWORK_TIMEOUT_MS);
  if (result != 0)
    return -2;

  char request[REQUEST_CAPACITY];
  size_t request_length;
  if (!make_request(url, request, sizeof(request), request_length)) {
    tcp_close();
    return -3;
  }

  if (url.https) {
    tls_transport_failure = 0;
    unsigned char entropy[32];
    uint32_t days;
    uint32_t seconds;
    if (!fill_tls_entropy(entropy)) {
      tcp_close();
      return -4;
    }
    if (!rtc_validation_time(days, seconds)) {
      tcp_close();
      return -5;
    }
    br_ssl_client_init_full(&tls_client, &tls_x509, osfos_https_trust_anchors,
                            2);
    br_ssl_engine_set_versions(&tls_client.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_buffer(&tls_client.eng, tls_buffer, sizeof(tls_buffer),
                             1);
    br_x509_minimal_set_time(&tls_x509, days, seconds);
    br_ssl_engine_inject_entropy(&tls_client.eng, entropy, sizeof(entropy));
    memset(entropy, 0, sizeof(entropy));
    if (!br_ssl_client_reset(&tls_client, url.host, 0)) {
      tcp_close();
      return -6;
    }

    br_sslio_init(&tls_io, &tls_client.eng, tls_read, nullptr, tls_write,
                  nullptr);
    if (br_sslio_write_all(&tls_io, request, request_length) < 0 ||
        br_sslio_flush(&tls_io) < 0) {
      int tls_error = br_ssl_engine_last_error(&tls_client.eng);
      tcp_close();
      return (tls_error != 0) ? -1000 - tls_error : -7;
    }
    result = read_response(true, output, output_capacity);
  } else {
    if (tcp_write(reinterpret_cast<const uint8_t *>(request), request_length) <
        0) {
      tcp_close();
      return -7;
    }
    result = read_response(false, output, output_capacity);
  }
  tcp_close();
  return result;
}

} // namespace Kernel::Net
