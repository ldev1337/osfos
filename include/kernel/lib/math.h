#pragma once
#include <cstdint>

namespace Kernel::Lib::Math {

constexpr int32_t SIN_SCALE = 1024;
constexpr int32_t Q16_ONE = 65536;

struct Vector2D {
  int32_t x, y;
};

struct Vector3D {
  int32_t x, y, z;
};

struct Matrix3x3 {
  int32_t m[3][3];
};

struct Matrix4x4 {
  int32_t m[4][4];
};

struct Rect {
  int32_t x, y, width, height;
  constexpr bool contains(int32_t px, int32_t py) const {
    return px >= x && px < x + width && py >= y && py < y + height;
  }
};

int32_t normalize_degrees(int32_t deg);
int16_t fixed_sin(int32_t deg);
int16_t fixed_cos(int32_t deg);

constexpr uint32_t abs_i32(int32_t x) {
  return x < 0 ? static_cast<uint32_t>(-x) : static_cast<uint32_t>(x);
}
constexpr uint32_t abs_u32(int32_t x) { return abs_i32(x); }

constexpr int32_t min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
constexpr int32_t max_i32(int32_t a, int32_t b) { return a > b ? a : b; }
constexpr int32_t clamp_i32(int32_t x, int32_t lo, int32_t hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

uint64_t isqrt_u64(uint64_t x);
uint32_t isqrt_u32(uint32_t x);
int32_t lerp_q16(int32_t a, int32_t b, uint32_t t);
int32_t fixed_mul(int32_t a, int32_t b);
int32_t fixed_div(int32_t a, int32_t b);

Vector3D mat4_mul_vec3(const Matrix4x4 &mat, const Vector3D &v);
Matrix4x4 mat4_identity();
Matrix4x4 mat4_multiply(const Matrix4x4 &a, const Matrix4x4 &b);

} // namespace Kernel::Lib::Math