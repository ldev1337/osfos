#ifndef KERNEL_GRAPHICS_K3D_H
#define KERNEL_GRAPHICS_K3D_H

#include <arch/x86_64/string.h>
#include <cstddef>
#include <cstdint>
#include <kernel/graphics/vfb.h>

namespace Kernel::Graphics::K3D {

// --- Fixed-point and Vector Mathematics ---
constexpr int32_t FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;

inline constexpr int32_t float_to_fp(float f) {
  return static_cast<int32_t>(f * static_cast<float>(FP_ONE));
}

inline constexpr float fp_to_float(int32_t fp) {
  return static_cast<float>(fp) / static_cast<float>(FP_ONE);
}

struct alignas(16) Vec3 {
  float x{0.0f}, y{0.0f}, z{0.0f};

  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

  constexpr Vec3 operator+(const Vec3 &o) const {
    return {x + o.x, y + o.y, z + o.z};
  }
  constexpr Vec3 operator-(const Vec3 &o) const {
    return {x - o.x, y - o.y, z - o.z};
  }
  constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

  constexpr float dot(const Vec3 &o) const {
    return x * o.x + y * o.y + z * o.z;
  }
  constexpr Vec3 cross(const Vec3 &o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
};

struct alignas(16) Vec4 {
  float x{0.0f}, y{0.0f}, z{0.0f}, w{1.0f};

  constexpr Vec4() = default;
  constexpr Vec4(float x_, float y_, float z_, float w_ = 1.0f)
      : x(x_), y(y_), z(z_), w(w_) {}
  constexpr Vec4(const Vec3 &v, float w_ = 1.0f)
      : x(v.x), y(v.y), z(v.z), w(w_) {}
};

struct alignas(64) Mat4 {
  float m[4][4]{};

  static Mat4 identity();
  static Mat4 translation(float tx, float ty, float tz);
  static Mat4 scale(float sx, float sy, float sz);
  static Mat4 rotation_x(float angle_rad);
  static Mat4 rotation_y(float angle_rad);
  static Mat4 rotation_z(float angle_rad);
  static Mat4 perspective(float fov_rad, float aspect, float znear, float zfar);
  static Mat4 look_at(const Vec3 &eye, const Vec3 &target, const Vec3 &up);

  Mat4 operator*(const Mat4 &o) const;
  Vec4 operator*(const Vec4 &v) const;
};

// --- Geometry Primitives ---
struct Vertex {
  Vec3 position;
  Vec3 normal;
  uint32_t color{0xFFFFFFFF};
};

struct Triangle {
  Vertex v[3];
};

struct BoundingSphere {
  Vec3 center;
  float radius{0.0f};
};

struct FrustumPlane {
  Vec3 normal;
  float distance{0.0f};

  constexpr float distance_to_point(const Vec3 &p) const {
    return normal.dot(p) + distance;
  }
};

struct Frustum {
  FrustumPlane planes[6]; // Left, Right, Bottom, Top, Near, Far

  bool intersects_sphere(const BoundingSphere &sphere) const {
    for (int i = 0; i < 6; ++i) {
      if (planes[i].distance_to_point(sphere.center) < -sphere.radius)
        return false;
    }
    return true;
  }
};

// --- Depth Buffer Management ---
class DepthBuffer {
public:
  DepthBuffer() = default;
  ~DepthBuffer();

  bool init(uint32_t width, uint32_t height);
  void destroy();
  void clear(float depth_value = 1.0f);

  inline float *data() { return buffer_; }
  inline uint32_t width() const { return width_; }
  inline uint32_t height() const { return height_; }

  inline bool test_and_set(uint32_t x, uint32_t y, float z) {
    const uint32_t idx = y * width_ + x;
    if (z < buffer_[idx]) {
      buffer_[idx] = z;
      return true;
    }
    return false;
  }

private:
  float *buffer_{nullptr};
  uint32_t width_{0};
  uint32_t height_{0};
};

// --- Main 3D Software Renderer Context ---
class Renderer3D {
public:
  Renderer3D() = default;
  ~Renderer3D() = default;

  bool init(uint32_t width, uint32_t height);
  void set_viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void clear_depth(float depth_val = 1.0f);

  void set_matrices(const Mat4 &model, const Mat4 &view, const Mat4 &proj);
  void set_light_direction(const Vec3 &dir);

  // Render pipeline entrypoints
  void render_mesh(VirtualFramebuffer &vfb, const Vertex *vertices,
                   size_t vertex_count, const uint32_t *indices,
                   size_t index_count, const BoundingSphere &bounds);

private:
  void rasterize_triangle(VirtualFramebuffer &vfb, const Vertex &v0,
                          const Vertex &v1, const Vertex &v2);

  Frustum extract_frustum(const Mat4 &clip_matrix);

  uint32_t vp_x_{0};
  uint32_t vp_y_{0};
  uint32_t vp_w_{0};
  uint32_t vp_h_{0};

  Mat4 mat_model_{Mat4::identity()};
  Mat4 mat_view_{Mat4::identity()};
  Mat4 mat_proj_{Mat4::identity()};
  Mat4 mat_mvp_{Mat4::identity()};

  Vec3 light_dir_{0.577f, 0.577f, -0.577f}; // Normalized directional light

  DepthBuffer depth_buffer_;
};

} // namespace Kernel::Graphics::K3D

#endif // KERNEL_GRAPHICS_K3D_H