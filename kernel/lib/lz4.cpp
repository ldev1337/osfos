#include <kernel/lib/lz4.h>
#include <limits.h>

namespace lib::lz4 {

static constexpr size_t MIN_MATCH = 4;
static constexpr size_t LAST_LITERALS = 5;
static constexpr size_t MATCH_FIND_LIMIT = 12;
static constexpr uint32_t EMPTY = UINT32_MAX;

static inline uint32_t read32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// 15-bit bucket index; each bucket holds 2 entries (HASH_ENTRIES / 2 buckets).
static inline uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> 17; }

static inline bool put(uint8_t *dst, size_t cap, size_t &pos, uint8_t v) {
  if (pos >= cap)
    return false;
  dst[pos++] = v;
  return true;
}

static bool put_extension(uint8_t *dst, size_t cap, size_t &pos, size_t len) {
  while (len >= 255) {
    if (!put(dst, cap, pos, 255))
      return false;
    len -= 255;
  }
  return put(dst, cap, pos, (uint8_t)len);
}

static bool emit_sequence(uint8_t *dst, size_t cap, size_t &op,
                          const uint8_t *literals, size_t lit_len,
                          uint16_t offset, size_t match_len) {
  if (match_len < MIN_MATCH)
    return false;
  if (op >= cap)
    return false;

  size_t token_pos = op++;
  size_t match_code = match_len - MIN_MATCH;

  dst[token_pos] = (uint8_t)((lit_len < 15 ? lit_len : 15) << 4) |
                   (uint8_t)(match_code < 15 ? match_code : 15);

  if (lit_len >= 15 && !put_extension(dst, cap, op, lit_len - 15))
    return false;
  if (lit_len > cap - op)
    return false;

  for (size_t i = 0; i < lit_len; i++)
    dst[op++] = literals[i];

  if (cap - op < 2)
    return false;
  dst[op++] = (uint8_t)offset;
  dst[op++] = (uint8_t)(offset >> 8);

  if (match_code >= 15 && !put_extension(dst, cap, op, match_code - 15))
    return false;
  return true;
}

static bool emit_last_literals(uint8_t *dst, size_t cap, size_t &op,
                               const uint8_t *src, size_t len) {
  if (op >= cap)
    return false;

  dst[op++] = (uint8_t)((len < 15 ? len : 15) << 4);

  if (len >= 15 && !put_extension(dst, cap, op, len - 15))
    return false;
  if (len > cap - op)
    return false;

  for (size_t i = 0; i < len; i++)
    dst[op++] = src[i];
  return true;
}

size_t compress_bound(size_t n) {
  if (n > (size_t)INT_MAX)
    return 0;
  size_t extra = n / 255 + 16;
  if (n > (size_t)INT_MAX - extra)
    return 0;
  return n + extra;
}

namespace {

struct Match {
  size_t start;
  size_t ref;
  size_t len; // 0 = no match
};

inline void insert_pos(uint32_t *table, const uint8_t *src, size_t p) {
  uint32_t *b = table + ((size_t)hash4(read32(src + p)) << 1);
  b[1] = b[0];
  b[0] = (uint32_t)p;
}

// Looks up position p (must be <= n - MATCH_FIND_LIMIT), inserts it, and
// returns the best match covering p, extended backward down to `anchor`.
inline Match find_match(const uint8_t *src, size_t n, uint32_t *table, size_t p,
                        size_t anchor) {
  const size_t limit = n - LAST_LITERALS;
  uint32_t *b = table + ((size_t)hash4(read32(src + p)) << 1);
  uint32_t cand[2] = {b[0], b[1]};
  b[1] = b[0];
  b[0] = (uint32_t)p;

  const uint32_t cur = read32(src + p);
  Match best = {p, 0, 0};
  size_t best_len = 0;

  for (int k = 0; k < 2; k++) {
    uint32_t c = cand[k];
    if (c == EMPTY || c >= p || p - (size_t)c > 65535 || read32(src + c) != cur)
      continue;

    size_t len = MIN_MATCH;
    while (p + len < limit && src[p + len] == src[c + len])
      ++len;

    if (len > best_len) { // ties keep the closer candidate (cand[0])
      best_len = len;
      best.ref = c;
    }
  }

  if (!best_len)
    return best;

  size_t start = p, ref = best.ref;
  while (start > anchor && ref > 0 && src[start - 1] == src[ref - 1]) {
    --start;
    --ref;
  }

  best.start = start;
  best.ref = ref;
  best.len = best_len + (p - start);
  return best;
}

} // namespace

int compress(const void *source, size_t n, void *output, size_t cap,
             uint32_t *table, size_t entries) {
  if (!source || !output || !table)
    return InvalidArgument;
  if (entries < HASH_ENTRIES)
    return InvalidArgument;
  if (!compress_bound(n))
    return UnsupportedSize;

  uint8_t *dst = (uint8_t *)output;
  const uint8_t *src = (const uint8_t *)source;

  for (size_t i = 0; i < HASH_ENTRIES; i++)
    table[i] = EMPTY;

  if (!n) {
    if (!cap)
      return OutputTooSmall;
    dst[0] = 0;
    return 1;
  }

  size_t ip = 0;
  size_t anchor = 0;
  size_t op = 0;

  if (n >= MATCH_FIND_LIMIT + 1) {
    const size_t match_limit = n - MATCH_FIND_LIMIT;
    size_t next_insert = 0; // first position not yet hashed

    while (ip <= match_limit) {
      Match m = find_match(src, n, table, ip, anchor);
      next_insert = ip + 1;

      if (!m.len) {
        ++ip;
        continue;
      }

      // Lazy matching: prefer a later match if it gains more than the
      // literal bytes it costs.
      while (ip + 1 <= match_limit) {
        Match nx = find_match(src, n, table, ip + 1, anchor);
        next_insert = ip + 2;
        if (!nx.len)
          break;

        size_t e1 = m.start + m.len;
        size_t e2 = nx.start + nx.len;
        size_t penalty = nx.start > m.start ? nx.start - m.start : 0;

        if (e2 > e1 + penalty) {
          m = nx;
          ++ip;
        } else {
          break;
        }
      }

      size_t offset = m.start - m.ref;
      if (!offset || offset > 65535)
        return InvalidBlock;

      if (!emit_sequence(dst, cap, op, src + anchor, m.start - anchor,
                         (uint16_t)offset, m.len)) {
        return OutputTooSmall;
      }

      ip = m.start + m.len;
      anchor = ip;

      for (size_t p = next_insert; p < ip && p <= match_limit; p++)
        insert_pos(table, src, p);
    }
  }

  if (!emit_last_literals(dst, cap, op, src + anchor, n - anchor))
    return OutputTooSmall;
  if (op > (size_t)INT_MAX)
    return UnsupportedSize;

  return (int)op;
}

static bool read_length(const uint8_t *src, size_t n, size_t &ip, size_t base,
                        bool extended, size_t &result) {
  result = base;
  if (!extended)
    return true;

  for (;;) {
    if (ip >= n)
      return false;

    uint8_t v = src[ip++];

    if (result > SIZE_MAX - v)
      return false;
    result += v;

    if (v != 255)
      return true;
  }
}

int decompress(const void *source, size_t n, void *output, size_t cap) {
  if (!source || !output || !n)
    return InvalidArgument;
  if (n > (size_t)INT_MAX)
    return UnsupportedSize;

  const uint8_t *src = (const uint8_t *)source;
  uint8_t *dst = (uint8_t *)output;

  size_t ip = 0;
  size_t op = 0;
  size_t last_match = SIZE_MAX;
  size_t last_literals = 0;

  for (;;) {
    if (ip >= n)
      return InvalidBlock;

    uint8_t token = src[ip++];
    size_t lit_len;

    uint8_t lit_code = token >> 4;

    if (!read_length(src, n, ip, lit_code, lit_code == 15, lit_len))
      return InvalidBlock;
    if (lit_len > n - ip)
      return InvalidBlock;
    if (lit_len > cap - op)
      return OutputTooSmall;
    if (lit_len > (size_t)INT_MAX - op)
      return UnsupportedSize;

    for (size_t i = 0; i < lit_len; i++)
      dst[op++] = src[ip++];

    if (ip == n) {
      last_literals = lit_len;
      break;
    }

    if (n - ip < 2)
      return InvalidBlock;

    uint16_t offset =
        (uint16_t)((uint16_t)src[ip] | ((uint16_t)src[ip + 1] << 8));
    ip += 2;

    if (!offset || offset > op)
      return InvalidBlock;

    size_t match_len;
    uint8_t match_code = token & 15;

    if (!read_length(src, n, ip, (size_t)match_code + MIN_MATCH,
                     match_code == 15, match_len))
      return InvalidBlock;
    if (match_len > cap - op)
      return OutputTooSmall;
    if (match_len > (size_t)INT_MAX - op)
      return UnsupportedSize;

    last_match = op;

    for (size_t i = 0; i < match_len; i++) {
      dst[op + i] = dst[op + i - offset];
    }

    op += match_len;
  }

  if (op >= LAST_LITERALS && last_literals < LAST_LITERALS)
    return InvalidBlock;

  if (last_match != SIZE_MAX &&
      (op < MATCH_FIND_LIMIT || last_match > op - MATCH_FIND_LIMIT))
    return InvalidBlock;

  return (int)op;
}

} // namespace lib::lz4