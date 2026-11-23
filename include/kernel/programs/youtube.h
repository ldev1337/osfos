#pragma once

#include <cstddef>

namespace Kernel::Apps::YouTube {

constexpr size_t MAX_RESULTS = 8;

struct Video {
  char id[16];
  char title[160];
  char channel[96];
  char published[32];
  char thumbnail_url[128];
};

// Query YouTube search via the public innertube HTTPS API. Returns the number
// of videos or a negative Kernel::Net::fetch error / -20 for an invalid body.
int search(const char *query, Video *videos, size_t capacity);

void run();

} // namespace Kernel::Apps::YouTube
