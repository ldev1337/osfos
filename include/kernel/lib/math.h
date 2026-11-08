#ifndef KERNEL_LIB_MATH_H
#define KERNEL_LIB_MATH_H

#include <cstddef>
#include <cstdint>

namespace Kernel::Lib::Math {

// Constant definition for Floating Point Math
constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float HALF_PI = 1.57079632679489661923f;
constexpr float DEG_TO_RAD = PI / 180.0f;
constexpr float RAD_TO_DEG = 180.0f / PI;

// Constant definition for Legacy Fixed-Point Q16.16 Math
constexpr int32_t Q16_ONE = 65536;

// --- Legacy Fixed-Point Structs (used in early graphics drivers) ---
struct Vector3D {
  int32_t x, y, z;
};

struct Matrix4x4 {
  int32_t m[4][4];
};

// --- Floating-Point Math Functions ---
float fabs(float x) noexcept;
float sqrt(float x) noexcept;
float sin(float rad) noexcept;
float cos(float rad) noexcept;
float tan(float rad) noexcept;
float atan2(float y, float x) noexcept;

// --- Fixed-Point Trigonometry & Arithmetic Utilities ---
int32_t normalize_degrees(int32_t deg);
int16_t fixed_sin(int32_t deg);
int16_t fixed_cos(int32_t deg);

uint64_t isqrt_u64(uint64_t x);
uint32_t isqrt_u32(uint32_t x);

int32_t lerp_q16(int32_t a, int32_t b, uint32_t t);
int32_t fixed_mul(int32_t a, int32_t b);
int32_t fixed_div(int32_t a, int32_t b);

Matrix4x4 mat4_identity();
Vector3D mat4_mul_vec3(const Matrix4x4 &mat, const Vector3D &v);
Matrix4x4 mat4_multiply(const Matrix4x4 &a, const Matrix4x4 &b);

} // namespace Kernel::Lib::Math

#endif // KERNEL_LIB_MATH_H