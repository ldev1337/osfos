#include <kernel/lib/math.h>

namespace Kernel::Lib::Math {

namespace {

// 91-entry lookup table for degree-based fixed_sin (0 to 90 deg, scaled by
// 1024)
static constexpr int16_t SIN_TABLE[91] = {
    0,    18,   36,   54,   71,   89,   107,  125,  143,  160,  178,  195,
    213,  230,  248,  265,  282,  299,  316,  333,  350,  367,  384,  400,
    416,  433,  449,  465,  481,  496,  512,  527,  543,  558,  573,  587,
    602,  616,  630,  644,  658,  672,  685,  698,  711,  724,  737,  749,
    761,  773,  784,  796,  807,  818,  828,  839,  849,  859,  868,  878,
    887,  896,  904,  912,  920,  928,  935,  943,  949,  956,  962,  968,
    974,  979,  984,  989,  994,  998,  1002, 1005, 1008, 1011, 1014, 1016,
    1018, 1020, 1022, 1023, 1023, 1024, 1024};

// Range reduction helper: maps radian input into [-PI, PI]
inline float wrap_radians(float rad) noexcept {
  while (rad > PI)
    rad -= TWO_PI;
  while (rad < -PI)
    rad += TWO_PI;
  return rad;
}

} // namespace

// --- Floating-Point Implementation ---

float fabs(float x) noexcept { return x < 0.0f ? -x : x; }

float sqrt(float x) noexcept {
  if (x <= 0.0f)
    return 0.0f;

#if defined(__x86_64__) || defined(__SSE__)
  float result;
  __asm__("sqrtss %1, %0" : "=x"(result) : "x"(x));
  return result;
#else
  // Fast software Newton-Raphson approximation fallback
  union {
    float f;
    uint32_t i;
  } u = {x};

  u.i = (0x5f3759df - (u.i >> 1)); // Bitwise initial guess
  float y = u.f;
  const float xhalf = 0.5f * x;

  // 3 iterations for high precision
  y = y * (1.5f - (xhalf * y * y));
  y = y * (1.5f - (xhalf * y * y));
  y = y * (1.5f - (xhalf * y * y));

  return x * y;
#endif
}

float sin(float rad) noexcept {
  rad = wrap_radians(rad);

  // Remez polynomial approximation for sin(x) on [-PI, PI]
  const float x2 = rad * rad;
  return rad * (1.0f + x2 * (-0.1666665710f +
                             x2 * (0.0083330756f + x2 * (-0.0001981064f +
                                                         x2 * 0.0000026019f))));
}

float cos(float rad) noexcept { return sin(HALF_PI - rad); }

float tan(float rad) noexcept {
  const float c = cos(rad);
  if (fabs(c) < 1e-7f)
    return (rad >= 0.0f) ? 1e7f : -1e7f;
  return sin(rad) / c;
}

float atan2(float y, float x) noexcept {
  if (x == 0.0f) {
    if (y > 0.0f)
      return HALF_PI;
    if (y < 0.0f)
      return -HALF_PI;
    return 0.0f;
  }

  const float abs_y = fabs(y);
  const float abs_x = fabs(x);
  const float a = (abs_x > abs_y) ? (abs_y / abs_x) : (abs_x / abs_y);
  const float s = a * a;

  // Polynomial approximation for atan(a) on [0, 1]
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;

  if (abs_y > abs_x)
    r = HALF_PI - r;
  if (x < 0.0f)
    r = PI - r;
  if (y < 0.0f)
    r = -r;

  return r;
}

// --- Fixed-Point Implementation ---

int32_t normalize_degrees(int32_t deg) {
  deg %= 360;
  return deg < 0 ? deg + 360 : deg;
}

int16_t fixed_sin(int32_t deg) {
  deg = normalize_degrees(deg);
  if (deg <= 90)
    return SIN_TABLE[deg];
  if (deg <= 180)
    return SIN_TABLE[180 - deg];
  if (deg <= 270)
    return -SIN_TABLE[deg - 180];
  return -SIN_TABLE[360 - deg];
}

int16_t fixed_cos(int32_t deg) { return fixed_sin(deg + 90); }

uint64_t isqrt_u64(uint64_t x) {
  if (x == 0)
    return 0;
  uint64_t res = 0;
  uint64_t bit = 1ULL << 62;
  while (bit > x)
    bit >>= 2;
  while (bit != 0) {
    if (x >= res + bit) {
      x -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

uint32_t isqrt_u32(uint32_t x) { return static_cast<uint32_t>(isqrt_u64(x)); }

int32_t lerp_q16(int32_t a, int32_t b, uint32_t t) {
  return a + static_cast<int32_t>((static_cast<int64_t>(b - a) * t) >> 16);
}

int32_t fixed_mul(int32_t a, int32_t b) {
  return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 16);
}

int32_t fixed_div(int32_t a, int32_t b) {
  if (b == 0)
    return 0;
  return static_cast<int32_t>((static_cast<int64_t>(a) << 16) / b);
}

Matrix4x4 mat4_identity() {
  Matrix4x4 mat = {};
  for (int i = 0; i < 4; ++i)
    mat.m[i][i] = Q16_ONE;
  return mat;
}

Vector3D mat4_mul_vec3(const Matrix4x4 &mat, const Vector3D &v) {
  int64_t x = static_cast<int64_t>(mat.m[0][0]) * v.x +
              static_cast<int64_t>(mat.m[0][1]) * v.y +
              static_cast<int64_t>(mat.m[0][2]) * v.z +
              static_cast<int64_t>(mat.m[0][3]) * Q16_ONE;
  int64_t y = static_cast<int64_t>(mat.m[1][0]) * v.x +
              static_cast<int64_t>(mat.m[1][1]) * v.y +
              static_cast<int64_t>(mat.m[1][2]) * v.z +
              static_cast<int64_t>(mat.m[1][3]) * Q16_ONE;
  int64_t z = static_cast<int64_t>(mat.m[2][0]) * v.x +
              static_cast<int64_t>(mat.m[2][1]) * v.y +
              static_cast<int64_t>(mat.m[2][2]) * v.z +
              static_cast<int64_t>(mat.m[2][3]) * Q16_ONE;
  int64_t w = static_cast<int64_t>(mat.m[3][0]) * v.x +
              static_cast<int64_t>(mat.m[3][1]) * v.y +
              static_cast<int64_t>(mat.m[3][2]) * v.z +
              static_cast<int64_t>(mat.m[3][3]) * Q16_ONE;

  if (w == 0)
    w = Q16_ONE;

  return {static_cast<int32_t>((x << 16) / w),
          static_cast<int32_t>((y << 16) / w),
          static_cast<int32_t>((z << 16) / w)};
}

Matrix4x4 mat4_multiply(const Matrix4x4 &a, const Matrix4x4 &b) {
  Matrix4x4 res = {};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      int64_t sum = 0;
      for (int k = 0; k < 4; ++k) {
        sum += (static_cast<int64_t>(a.m[i][k]) * b.m[k][j]) >> 16;
      }
      res.m[i][j] = static_cast<int32_t>(sum);
    }
  }
  return res;
}

} // namespace Kernel::Lib::Math