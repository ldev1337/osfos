#include <string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/media/jpeg.h>
#include <kernel/memory/heap.h>
#include <kernel/net/smoltcp_bridge.h>
#include <kernel/programs/youtube.h>
#include <kernel/scheduler.h>

#include <cstddef>
#include <cstdint>

namespace Kernel::Apps::YouTube {
namespace {
constexpr size_t SEARCH_CAPACITY = 65536;
constexpr size_t QUERY_CAPACITY = 96;
constexpr size_t THUMBNAIL_RESPONSE_CAPACITY = 32768;
constexpr uint32_t THUMBNAIL_WIDTH = 120;
constexpr uint32_t THUMBNAIL_HEIGHT = 68;
constexpr size_t THUMBNAIL_PIXEL_COUNT =
    static_cast<size_t>(THUMBNAIL_WIDTH) * THUMBNAIL_HEIGHT;
constexpr uint32_t BG = 0xFF101418;
constexpr uint32_t PANEL = 0xFF1A2229;
constexpr uint32_t ROW = 0xFF202A32;
constexpr uint32_t SELECTED = 0xFF293D43;
constexpr uint32_t TEXT = 0xFFE7ECEF;
constexpr uint32_t MUTED = 0xFF9AAAB1;
constexpr uint32_t ACCENT = 0xFFFF3030;

enum class ThumbnailState : uint8_t {
  Empty = 0,
  Loading = 1,
  Loaded = 2,
  Failed = 3,
};

struct ThumbnailCacheEntry {
  ThumbnailState state;
  int error_code;
  uint32_t width;
  uint32_t height;
  char id[sizeof(Video::id)];
  uint32_t pixels[THUMBNAIL_PIXEL_COUNT];
};

void clear_thumbnail_cache(ThumbnailCacheEntry *cache, size_t capacity) {
  if (!cache)
    return;
  for (size_t i = 0; i < capacity; ++i) {
    cache[i].state = ThumbnailState::Empty;
    cache[i].error_code = 0;
    cache[i].width = 0;
    cache[i].height = 0;
    cache[i].id[0] = '\0';
  }
}

void copy_text(char *out, size_t capacity, const char *text, size_t length) {
  if (!capacity)
    return;
  if (length >= capacity)
    length = capacity - 1;
  memcpy(out, text, length);
  out[length] = '\0';
}

bool append(char *out, size_t capacity, size_t &length, char value) {
  if (length + 1 >= capacity)
    return false;
  out[length++] = value;
  out[length] = '\0';
  return true;
}

bool append_literal(char *out, size_t capacity, size_t &length,
                    const char *text) {
  for (; *text; ++text)
    if (!append(out, capacity, length, *text))
      return false;
  return true;
}

bool append_json_escaped(char *out, size_t capacity, size_t &length,
                         const char *text) {
  for (; *text; ++text) {
    const unsigned char ch = static_cast<unsigned char>(*text);
    if (ch == '"' || ch == '\\') {
      if (!append(out, capacity, length, '\\') ||
          !append(out, capacity, length, static_cast<char>(ch)))
        return false;
    } else if (ch < 0x20) {
      constexpr char hex[] = "0123456789abcdef";
      if (!append(out, capacity, length, '\\') ||
          !append(out, capacity, length, 'u') ||
          !append(out, capacity, length, '0') ||
          !append(out, capacity, length, '0') ||
          !append(out, capacity, length, hex[ch >> 4]) ||
          !append(out, capacity, length, hex[ch & 15]))
        return false;
    } else if (!append(out, capacity, length, static_cast<char>(ch))) {
      return false;
    }
  }
  return true;
}

bool append_utf8(char *out, size_t capacity, size_t &length,
                 uint32_t codepoint) {
  if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
    return false;
  if (codepoint <= 0x7Fu)
    return append(out, capacity, length, static_cast<char>(codepoint));
  if (codepoint <= 0x7FFu)
    return append(out, capacity, length,
                  static_cast<char>(0xC0u | (codepoint >> 6))) &&
           append(out, capacity, length,
                  static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  if (codepoint <= 0xFFFFu)
    return append(out, capacity, length,
                  static_cast<char>(0xE0u | (codepoint >> 12))) &&
           append(out, capacity, length,
                  static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu))) &&
           append(out, capacity, length,
                  static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  return append(out, capacity, length,
                static_cast<char>(0xF0u | (codepoint >> 18))) &&
         append(out, capacity, length,
                static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu))) &&
         append(out, capacity, length,
                static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu))) &&
         append(out, capacity, length,
                static_cast<char>(0x80u | (codepoint & 0x3Fu)));
}

bool parse_json_string(const char *begin, const char *end, const char **after,
                       char *out, size_t capacity) {
  if (begin >= end || *begin != '"')
    return false;
  size_t written = 0;
  const char *cursor = begin + 1;
  while (cursor < end) {
    const unsigned char ch = static_cast<unsigned char>(*cursor++);
    if (ch == '"') {
      if (out && capacity)
        out[written] = '\0';
      if (after)
        *after = cursor;
      return true;
    }
    if (ch == '\\') {
      if (cursor >= end)
        return false;
      const char escaped = *cursor++;
      uint32_t codepoint = 0;
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        codepoint = static_cast<unsigned char>(escaped);
        break;
      case 'b':
        codepoint = '\b';
        break;
      case 'f':
        codepoint = '\f';
        break;
      case 'n':
        codepoint = '\n';
        break;
      case 'r':
        codepoint = '\r';
        break;
      case 't':
        codepoint = '\t';
        break;
      case 'u': {
        if (cursor + 4 > end)
          return false;
        for (int i = 0; i < 4; ++i) {
          const char digit = cursor[i];
          uint32_t value;
          if (digit >= '0' && digit <= '9')
            value = static_cast<uint32_t>(digit - '0');
          else if (digit >= 'a' && digit <= 'f')
            value = static_cast<uint32_t>(digit - 'a' + 10);
          else if (digit >= 'A' && digit <= 'F')
            value = static_cast<uint32_t>(digit - 'A' + 10);
          else
            return false;
          codepoint = (codepoint << 4) | value;
        }
        cursor += 4;
        break;
      }
      default:
        return false;
      }
      if (out && capacity && written + 1 < capacity)
        append_utf8(out, capacity, written, codepoint);
      continue;
    }
    if (ch < 0x20)
      return false;
    if (out && capacity && written + 1 < capacity)
      append(out, capacity, written, static_cast<char>(ch));
  }
  return false;
}

const char *find_key(const char *begin, const char *end, const char *key) {
  const size_t key_length = strlen(key);
  for (const char *cursor = begin; cursor + key_length + 2 <= end; ++cursor) {
    if (cursor[0] != '"' || memcmp(cursor + 1, key, key_length) != 0 ||
        cursor[1 + key_length] != '"')
      continue;
    const char *value = cursor + key_length + 2;
    while (value < end && (*value == ' ' || *value == '\t' || *value == '\r' ||
                           *value == '\n'))
      ++value;
    if (value < end && *value == ':') {
      ++value;
      while (value < end && (*value == ' ' || *value == '\t' ||
                             *value == '\r' || *value == '\n'))
        ++value;
      return value;
    }
  }
  return nullptr;
}

bool extract_object(const char *begin, const char *end, const char **object,
                    const char **after) {
  if (begin >= end || *begin != '{')
    return false;
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (const char *cursor = begin; cursor < end; ++cursor) {
    const char ch = *cursor;
    if (in_string) {
      if (escape)
        escape = false;
      else if (ch == '\\')
        escape = true;
      else if (ch == '"')
        in_string = false;
      continue;
    }
    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '{') {
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0) {
        *object = begin;
        *after = cursor + 1;
        return true;
      }
    }
  }
  return false;
}

bool extract_run_text(const char *object, const char *object_end,
                      const char *key, char *out, size_t capacity) {
  const char *field = find_key(object, object_end, key);
  if (!field)
    return false;
  const char *runs = find_key(field, object_end, "runs");
  if (!runs || runs >= object_end || *runs != '[')
    return false;
  const char *item = runs + 1;
  while (item < object_end &&
         (*item == ' ' || *item == '\t' || *item == '\r' || *item == '\n'))
    ++item;
  if (item >= object_end || *item != '{')
    return false;
  const char *run_object = nullptr;
  const char *run_after = nullptr;
  if (!extract_object(item, object_end, &run_object, &run_after))
    return false;
  const char *text = find_key(run_object, run_after, "text");
  return text && parse_json_string(text, run_after, nullptr, out, capacity);
}

bool already_have_id(const Video *videos, size_t count, const char *id) {
  for (size_t i = 0; i < count; ++i)
    if (memcmp(videos[i].id, id, 12) == 0)
      return true;
  return false;
}

bool fill_thumbnail_url(Video &video) {
  size_t url_length = 0;
  video.thumbnail_url[0] = '\0';
  return append_literal(video.thumbnail_url, sizeof(video.thumbnail_url),
                        url_length, "https://i.ytimg.com/vi/") &&
         append_literal(video.thumbnail_url, sizeof(video.thumbnail_url),
                        url_length, video.id) &&
         append_literal(video.thumbnail_url, sizeof(video.thumbnail_url),
                        url_length, "/mqdefault.jpg");
}

int parse_search_response(char *response, size_t response_length, Video *videos,
                          size_t capacity) {
  const char *header_end = nullptr;
  for (size_t i = 0; i + 3 < response_length; ++i) {
    if (memcmp(response + i, "\r\n\r\n", 4) == 0) {
      header_end = response + i + 4;
      break;
    }
  }
  if (!header_end || response_length < 12 || memcmp(response, "HTTP/", 5))
    return -20;
  if (response[9] != '2' || response[10] != '0' || response[11] != '0')
    return -20;

  const char *body = header_end;
  const char *end = response + response_length;
  static constexpr char renderer[] = "\"videoWithContextRenderer\"";
  size_t count = 0;
  for (const char *cursor = body;
       cursor + sizeof(renderer) - 1 <= end && count < capacity;) {
    const char *match = nullptr;
    for (const char *p = cursor; p + sizeof(renderer) - 1 <= end; ++p) {
      if (memcmp(p, renderer, sizeof(renderer) - 1) == 0) {
        match = p;
        break;
      }
    }
    if (!match)
      break;
    const char *object_start = match + sizeof(renderer) - 1;
    while (object_start < end &&
           (*object_start == ' ' || *object_start == '\t' ||
            *object_start == '\r' || *object_start == '\n' ||
            *object_start == ':'))
      ++object_start;
    const char *object = nullptr;
    const char *after = nullptr;
    if (!extract_object(object_start, end, &object, &after)) {
      cursor = match + 1;
      continue;
    }

    const char *id_value = find_key(object, after, "videoId");
    char id[sizeof(Video::id)]{};
    if (!id_value ||
        !parse_json_string(id_value, after, nullptr, id, sizeof(id)) ||
        strlen(id) != 11 || already_have_id(videos, count, id)) {
      cursor = after;
      continue;
    }

    Video &video = videos[count];
    memset(&video, 0, sizeof(video));
    memcpy(video.id, id, sizeof(video.id));
    if (!extract_run_text(object, after, "headline", video.title,
                          sizeof(video.title))) {
      cursor = after;
      continue;
    }
    extract_run_text(object, after, "shortBylineText", video.channel,
                     sizeof(video.channel));
    extract_run_text(object, after, "publishedTimeText", video.published,
                     sizeof(video.published));
    if (!fill_thumbnail_url(video)) {
      cursor = after;
      continue;
    }
    ++count;
    cursor = after;
  }
  return static_cast<int>(count);
}

struct ThumbnailJob {
  volatile uint32_t state;
  volatile uint32_t references;
  uint32_t generation;
  size_t index;
  int result;
  uint32_t width;
  uint32_t height;
  char id[sizeof(Video::id)];
  char url[sizeof(Video::thumbnail_url)];
  uint32_t pixels[THUMBNAIL_PIXEL_COUNT];
};

void release_thumbnail_job(ThumbnailJob *job) {
  if (__atomic_sub_fetch(&job->references, 1u, __ATOMIC_ACQ_REL) == 0)
    kfree(job);
}

int decode_thumbnail_response(char *response, int response_length,
                              uint32_t *pixels, uint32_t *width,
                              uint32_t *height) {
  if (response_length <= 0)
    return -1;
  const size_t length = static_cast<size_t>(response_length);
  size_t body_offset = 0;
  for (size_t i = 0; i + 3 < length; ++i) {
    if (memcmp(response + i, "\r\n\r\n", 4) == 0) {
      body_offset = i + 4;
      break;
    }
  }
  if (!body_offset || length < 12 || memcmp(response, "HTTP/", 5) ||
      response[9] != '2' || response[10] != '0' || response[11] != '0')
    return -2;
  return Kernel::Media::JPEG::decode(
      reinterpret_cast<const uint8_t *>(response + body_offset),
      length - body_offset, pixels, THUMBNAIL_PIXEL_COUNT, THUMBNAIL_WIDTH,
      THUMBNAIL_HEIGHT, width, height);
}

void thumbnail_worker(void *argument) {
  auto *job = static_cast<ThumbnailJob *>(argument);
  auto *response = static_cast<char *>(kmalloc(THUMBNAIL_RESPONSE_CAPACITY));
  if (!response) {
    job->result = -3;
  } else {
    const int response_length =
        Kernel::Net::fetch(job->url, response, THUMBNAIL_RESPONSE_CAPACITY);
    if (response_length < 0)
      job->result = response_length;
    else
      job->result = decode_thumbnail_response(
          response, response_length, job->pixels, &job->width, &job->height);
    kfree(response);
  }
  __atomic_store_n(&job->state, 1u, __ATOMIC_RELEASE);
  release_thumbnail_job(job);
  Kernel::Scheduler::thread_exit();
}

ThumbnailJob *request_thumbnail(const Video &video, size_t index,
                                uint32_t generation) {
  auto *job = static_cast<ThumbnailJob *>(kmalloc(sizeof(ThumbnailJob)));
  if (!job)
    return nullptr;
  memset(job, 0, sizeof(*job));
  job->generation = generation;
  job->index = index;
  memcpy(job->id, video.id, sizeof(job->id));
  memcpy(job->url, video.thumbnail_url, sizeof(job->url));
  __atomic_store_n(&job->references, 2u, __ATOMIC_RELAXED);
  if (Kernel::Scheduler::thread_create(
          thumbnail_worker, job, Kernel::Scheduler::ThreadPriority::NORMAL) <
      0) {
    kfree(job);
    return nullptr;
  }
  return job;
}

void draw_text(Graphics::VirtualFramebuffer &vfb, int x, int y,
               const char *text, uint32_t color, float size = 14.0f) {
  AppCommon::text(vfb, x, y, text, color, size);
}

void draw_ui(Graphics::VirtualFramebuffer &vfb, const char *query,
             const Video *videos, size_t count, size_t selected,
             const char *status, bool busy,
             const ThumbnailCacheEntry *thumbnail_cache) {
  const int32_t width = static_cast<int32_t>(vfb.width());
  const int32_t height = static_cast<int32_t>(vfb.height());
  vfb.clear(BG);
  vfb.fill_rect(0, 0, width, 54, PANEL);
  vfb.fill_rect(14, 12, 5, 28, ACCENT);
  draw_text(vfb, 30, 10, "YouTube", TEXT, 22.0f);
  draw_text(vfb, 144, 17, "OSFOS live search", MUTED, 13.0f);
  vfb.fill_rounded_rect(14, 66, width - 28, 38, 7, PANEL);
  draw_text(vfb, 25, 77, query, TEXT, 14.0f);
  if (!*query)
    draw_text(vfb, 25, 77, "Type a search and press Enter", MUTED, 14.0f);

  const int32_t row_height = 88;
  const int32_t card_height = 80;
  const int32_t first_y = 114;
  const int32_t thumb_box_x = 22;
  const int32_t thumb_box_w = static_cast<int32_t>(THUMBNAIL_WIDTH);
  const int32_t thumb_box_h = static_cast<int32_t>(THUMBNAIL_HEIGHT);
  const int32_t text_x = thumb_box_x + thumb_box_w + 14;

  const size_t visible =
      height > first_y + 34
          ? static_cast<size_t>((height - first_y - 34) / row_height)
          : 0;
  const size_t first =
      visible && selected >= visible ? selected - visible + 1 : 0;

  for (size_t row = 0; row < visible && first + row < count; ++row) {
    const size_t i = first + row;
    const int32_t y = first_y + static_cast<int32_t>(row) * row_height;
    const int32_t thumb_box_y = y + (card_height - thumb_box_h) / 2;

    vfb.fill_rounded_rect(14, y, width - 28, card_height, 6,
                          i == selected ? SELECTED : ROW);

    const ThumbnailCacheEntry *entry =
        thumbnail_cache ? &thumbnail_cache[i] : nullptr;
    if (entry && entry->state == ThumbnailState::Loaded &&
        memcmp(entry->id, videos[i].id, sizeof(entry->id)) == 0 &&
        entry->width > 0 && entry->height > 0) {
      const int32_t thumb_x =
          thumb_box_x + (thumb_box_w - static_cast<int32_t>(entry->width)) / 2;
      const int32_t thumb_y =
          thumb_box_y + (thumb_box_h - static_cast<int32_t>(entry->height)) / 2;
      vfb.fill_rounded_rect(thumb_box_x, thumb_box_y, thumb_box_w, thumb_box_h,
                            5, 0xFF101418);
      vfb.blit(entry->pixels, thumb_x, thumb_y, entry->width, entry->height,
               entry->width * sizeof(uint32_t));
    } else {
      vfb.fill_rounded_rect(thumb_box_x, thumb_box_y, thumb_box_w, thumb_box_h,
                            5, 0xFF303A41);
      const int32_t cx = thumb_box_x + thumb_box_w / 2;
      const int32_t cy = thumb_box_y + thumb_box_h / 2;
      vfb.fill_triangle(cx - 8, cy - 12, cx - 8, cy + 12, cx + 11, cy, ACCENT);
    }

    draw_text(vfb, text_x, y + 10, videos[i].title, TEXT, 14.0f);
    draw_text(vfb, text_x, y + 36, videos[i].channel, MUTED, 12.0f);
    draw_text(vfb, text_x, y + 56, videos[i].published, MUTED, 10.0f);
  }

  vfb.fill_rect(0, height - 27, width, 27, PANEL);
  draw_text(vfb, 14, height - 22, busy ? "Searching YouTube..." : status,
            busy ? ACCENT : MUTED, 11.0f);
  draw_text(vfb, width - 190, height - 22, "Up/Down select  Esc close", MUTED,
            11.0f);
  vfb.flush();
}

size_t pick_next_thumbnail_index(const Video *videos, size_t count,
                                 size_t selected,
                                 const ThumbnailCacheEntry *cache) {
  if (!videos || !cache || count == 0)
    return count;

  if (selected < count && cache[selected].state == ThumbnailState::Empty)
    return selected;

  for (size_t i = 0; i < count; ++i) {
    if (cache[i].state == ThumbnailState::Empty)
      return i;
  }
  return count;
}

size_t count_loaded_thumbnails(const ThumbnailCacheEntry *cache, size_t count) {
  if (!cache)
    return 0;
  size_t loaded = 0;
  for (size_t i = 0; i < count; ++i) {
    if (cache[i].state == ThumbnailState::Loaded)
      ++loaded;
  }
  return loaded;
}
} // namespace

int search(const char *query, Video *videos, size_t capacity) {
  if (!query || !*query || !videos || !capacity || capacity > MAX_RESULTS)
    return -20;

  char body[512];
  size_t body_length = 0;
  if (!append_literal(body, sizeof(body), body_length,
                      "{\"context\":{\"client\":{\"clientName\":\"MWEB\","
                      "\"clientVersion\":\"2.20241126.01.00\",\"hl\":\"en\","
                      "\"gl\":\"US\"}},"
                      "\"query\":\"") ||
      !append_json_escaped(body, sizeof(body), body_length, query) ||
      !append_literal(body, sizeof(body), body_length,
                      "\",\"params\":\"EgIQAQ%3D%3D\"}"))
    return -20;

  char *response = static_cast<char *>(kmalloc(SEARCH_CAPACITY));
  if (!response)
    return -1;
  const int length = Kernel::Net::fetch_post(
      "https://www.youtube.com/youtubei/v1/search?prettyPrint=false",
      reinterpret_cast<const uint8_t *>(body), body_length, response,
      SEARCH_CAPACITY);
  int result =
      length < 0 ? length
                 : parse_search_response(response, static_cast<size_t>(length),
                                         videos, capacity);
  kfree(response);
  return result;
}

void run() {
  Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(820, 620, 400, 240))
    return;

  auto *videos = static_cast<Video *>(kmalloc(sizeof(Video) * MAX_RESULTS));
  auto *thumbnail_cache = static_cast<ThumbnailCacheEntry *>(
      kmalloc(sizeof(ThumbnailCacheEntry) * MAX_RESULTS));
  if (!videos || !thumbnail_cache) {
    if (videos)
      kfree(videos);
    if (thumbnail_cache)
      kfree(thumbnail_cache);
    return;
  }
  memset(videos, 0, sizeof(Video) * MAX_RESULTS);
  memset(thumbnail_cache, 0, sizeof(ThumbnailCacheEntry) * MAX_RESULTS);

  char query[QUERY_CAPACITY]{};
  char status[112] = "Enter a search term to load live results.";
  size_t length = 0;
  size_t count = 0;
  size_t selected = 0;
  uint32_t search_generation = 0;
  ThumbnailJob *thumbnail_job = nullptr;
  bool busy = false;
  bool running = true;
  bool up_was_pressed = false;
  bool down_was_pressed = false;

  draw_ui(vfb, query, videos, count, selected, status, busy, thumbnail_cache);

  while (running && !Scheduler::should_stop()) {
    bool changed = false;
    while (Drivers::Keyboard::available()) {
      const char key = Drivers::Keyboard::getchar();
      if (key == 27) {
        running = false;
        break;
      }
      if (key == '\n' || key == '\r') {
        if (length) {
          ++search_generation;
          if (thumbnail_job) {
            release_thumbnail_job(thumbnail_job);
            thumbnail_job = nullptr;
          }
          clear_thumbnail_cache(thumbnail_cache, MAX_RESULTS);
          memset(videos, 0, sizeof(Video) * MAX_RESULTS);

          busy = true;
          count = 0;
          selected = 0;
          copy_text(status, sizeof(status), "Contacting YouTube over TLS...",
                    sizeof("Contacting YouTube over TLS...") - 1);
          draw_ui(vfb, query, videos, count, selected, status, busy,
                  thumbnail_cache);

          const int result = search(query, videos, MAX_RESULTS);
          busy = false;
          if (result >= 0) {
            count = static_cast<size_t>(result);
            if (count) {
              copy_text(status, sizeof(status),
                        "Live results. Caching thumbnails...",
                        sizeof("Live results. Caching thumbnails...") - 1);
            } else {
              copy_text(status, sizeof(status), "No results returned.",
                        sizeof("No results returned.") - 1);
            }
          } else if (result == -4) {
            copy_text(status, sizeof(status), "HTTPS requires CPU RDRAND.",
                      sizeof("HTTPS requires CPU RDRAND.") - 1);
          } else if (result == -5) {
            copy_text(status, sizeof(status),
                      "HTTPS refused: RTC time unavailable.",
                      sizeof("HTTPS refused: RTC time unavailable.") - 1);
          } else if (result == -20) {
            copy_text(status, sizeof(status),
                      "YouTube search response was not supported.",
                      sizeof("YouTube search response was not supported.") - 1);
          } else {
            copy_text(status, sizeof(status),
                      "Network/search failed. Run netup first.",
                      sizeof("Network/search failed. Run netup first.") - 1);
          }
          changed = true;
        }
      } else if (key == '\b' || key == 127) {
        if (length) {
          query[--length] = '\0';
          changed = true;
        }
      } else if (key >= 32 && key <= 126 && length + 1 < sizeof(query)) {
        query[length++] = key;
        query[length] = '\0';
        changed = true;
      }
    }

    const bool up_pressed = Drivers::Keyboard::is_extended_pressed(0x48);
    const bool down_pressed = Drivers::Keyboard::is_extended_pressed(0x50);
    if (count && up_pressed && !up_was_pressed) {
      if (selected > 0) {
        --selected;
        changed = true;
      }
    }
    if (count && down_pressed && !down_was_pressed) {
      if (selected + 1 < count) {
        ++selected;
        changed = true;
      }
    }
    up_was_pressed = up_pressed;
    down_was_pressed = down_pressed;

    if (thumbnail_job &&
        __atomic_load_n(&thumbnail_job->state, __ATOMIC_ACQUIRE) != 0) {
      if (thumbnail_job->generation == search_generation &&
          thumbnail_job->index < count &&
          memcmp(thumbnail_job->id, videos[thumbnail_job->index].id,
                 sizeof(thumbnail_job->id)) == 0) {
        ThumbnailCacheEntry &entry = thumbnail_cache[thumbnail_job->index];
        memcpy(entry.id, thumbnail_job->id, sizeof(entry.id));
        if (thumbnail_job->result == 0 && thumbnail_job->width > 0 &&
            thumbnail_job->height > 0) {
          memcpy(entry.pixels, thumbnail_job->pixels, sizeof(entry.pixels));
          entry.width = thumbnail_job->width;
          entry.height = thumbnail_job->height;
          entry.error_code = 0;
          entry.state = ThumbnailState::Loaded;
        } else {
          entry.width = 0;
          entry.height = 0;
          entry.error_code = thumbnail_job->result;
          entry.state = ThumbnailState::Failed;
        }

        const size_t loaded = count_loaded_thumbnails(thumbnail_cache, count);
        if (loaded == count) {
          copy_text(status, sizeof(status),
                    "All thumbnails cached. Up/Down selects.",
                    sizeof("All thumbnails cached. Up/Down selects.") - 1);
        } else if (thumbnail_job->index == selected &&
                   entry.state == ThumbnailState::Failed) {
          char message[96];
          size_t message_length = 0;
          append_literal(message, sizeof(message), message_length,
                         "Thumbnail failed (");
          int code = entry.error_code;
          if (code < 0) {
            append(message, sizeof(message), message_length, '-');
            code = -code;
          }
          char digits[16];
          size_t digit_count = 0;
          do {
            digits[digit_count++] = static_cast<char>('0' + (code % 10));
            code /= 10;
          } while (code && digit_count < sizeof(digits));
          while (digit_count)
            append(message, sizeof(message), message_length,
                   digits[--digit_count]);
          append_literal(message, sizeof(message), message_length, ").");
          copy_text(status, sizeof(status), message, message_length);
        }
        changed = true;
      }

      release_thumbnail_job(thumbnail_job);
      thumbnail_job = nullptr;
    }

    if (count && !thumbnail_job) {
      const size_t next_index =
          pick_next_thumbnail_index(videos, count, selected, thumbnail_cache);
      if (next_index < count) {
        thumbnail_cache[next_index].state = ThumbnailState::Loading;
        memcpy(thumbnail_cache[next_index].id, videos[next_index].id,
               sizeof(thumbnail_cache[next_index].id));
        thumbnail_job = request_thumbnail(videos[next_index], next_index,
                                          search_generation);
        if (!thumbnail_job) {
          thumbnail_cache[next_index].state = ThumbnailState::Failed;
          thumbnail_cache[next_index].error_code = -3;
          if (next_index == selected) {
            copy_text(status, sizeof(status),
                      "Thumbnail worker could not start.",
                      sizeof("Thumbnail worker could not start.") - 1);
          }
          changed = true;
        }
      }
    }

    if (changed)
      draw_ui(vfb, query, videos, count, selected, status, busy,
              thumbnail_cache);

    Scheduler::thread_sleep_ms(16);
  }

  if (thumbnail_job)
    release_thumbnail_job(thumbnail_job);
  kfree(thumbnail_cache);
  kfree(videos);
}

} // namespace Kernel::Apps::YouTube
