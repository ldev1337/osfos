#include <algorithm>
#include <arch/x86_64/string.h>
#include <kernel/graphics/k3d.h>
#include <kernel/lib/math.h>
#include <kernel/memory/heap.h>

namespace Kernel::Graphics::K3D {

// --- Matrix Transformations ---
Mat4 Mat4::identity() {
  Mat4 r;
  r.m[0][0] = 1.0f;
  r.m[1][1] = 1.0f;
  r.m[2][2] = 1.0f;
  r.m[3][3] = 1.0f;
  return r;
}

Mat4 Mat4::translation(float tx, float ty, float tz) {
  Mat4 r = identity();
  r.m[0][3] = tx;
  r.m[1][3] = ty;
  r.m[2][3] = tz;
  return r;
}

Mat4 Mat4::scale(float sx, float sy, float sz) {
  Mat4 r;
  r.m[0][0] = sx;
  r.m[1][1] = sy;
  r.m[2][2] = sz;
  r.m[3][3] = 1.0f;
  return r;
}

Mat4 Mat4::rotation_x(float angle_rad) {
  Mat4 r = identity();
  const float c = Kernel::Lib::Math::cos(angle_rad);
  const float s = Kernel::Lib::Math::sin(angle_rad);
  r.m[1][1] = c;
  r.m[1][2] = -s;
  r.m[2][1] = s;
  r.m[2][2] = c;
  return r;
}

Mat4 Mat4::rotation_y(float angle_rad) {
  Mat4 r = identity();
  const float c = Kernel::Lib::Math::cos(angle_rad);
  const float s = Kernel::Lib::Math::sin(angle_rad);
  r.m[0][0] = c;
  r.m[0][2] = s;
  r.m[2][0] = -s;
  r.m[2][2] = c;
  return r;
}

Mat4 Mat4::rotation_z(float angle_rad) {
  Mat4 r = identity();
  const float c = Kernel::Lib::Math::cos(angle_rad);
  const float s = Kernel::Lib::Math::sin(angle_rad);
  r.m[0][0] = c;
  r.m[0][1] = -s;
  r.m[1][0] = s;
  r.m[1][1] = c;
  return r;
}

Mat4 Mat4::perspective(float fov_rad, float aspect, float znear, float zfar) {
  Mat4 r;
  const float tan_half_fov = Kernel::Lib::Math::tan(fov_rad * 0.5f);
  r.m[0][0] = 1.0f / (aspect * tan_half_fov);
  r.m[1][1] = 1.0f / tan_half_fov;
  r.m[2][2] = -(zfar + znear) / (zfar - znear);
  r.m[2][3] = -(2.0f * zfar * znear) / (zfar - znear);
  r.m[3][2] = -1.0f;
  return r;
}

Mat4 Mat4::operator*(const Mat4 &o) const {
  Mat4 r;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] +
                  m[i][2] * o.m[2][j] + m[i][3] * o.m[3][j];
    }
  }
  return r;
}

Vec4 Mat4::operator*(const Vec4 &v) const {
  return Vec4(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3] * v.w,
              m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3] * v.w,
              m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] * v.w,
              m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3] * v.w);
}

// --- Depth Buffer Management ---
DepthBuffer::~DepthBuffer() { destroy(); }

bool DepthBuffer::init(uint32_t width, uint32_t height) {
  destroy();
  if (width == 0 || height == 0)
    return false;

  width_ = width;
  height_ = height;
  const size_t size = width_ * height_ * sizeof(float);
  buffer_ = static_cast<float *>(kmalloc(size));
  if (!buffer_)
    return false;

  clear();
  return true;
}

void DepthBuffer::destroy() {
  if (buffer_) {
    kfree(buffer_);
    buffer_ = nullptr;
  }
  width_ = height_ = 0;
}

void DepthBuffer::clear(float depth_value) {
  if (!buffer_)
    return;
  const uint32_t count = width_ * height_;
  for (uint32_t i = 0; i < count; ++i) {
    buffer_[i] = depth_value;
  }
}

// --- Software Renderer Engine ---
bool Renderer3D::init(uint32_t width, uint32_t height) {
  set_viewport(0, 0, width, height);
  return depth_buffer_.init(width, height);
}

void Renderer3D::set_viewport(uint32_t x, uint32_t y, uint32_t width,
                              uint32_t height) {
  vp_x_ = x;
  vp_y_ = y;
  vp_w_ = width;
  vp_h_ = height;
}

void Renderer3D::clear_depth(float depth_val) {
  depth_buffer_.clear(depth_val);
}

void Renderer3D::set_matrices(const Mat4 &model, const Mat4 &view,
                              const Mat4 &proj) {
  mat_model_ = model;
  mat_view_ = view;
  mat_proj_ = proj;
  mat_mvp_ = mat_proj_ * (mat_view_ * mat_model_);
}

void Renderer3D::set_light_direction(const Vec3 &dir) {
  const float len =
      Kernel::Lib::Math::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
  if (len > 0.0001f) {
    light_dir_ = dir * (1.0f / len);
  }
}

Frustum Renderer3D::extract_frustum(const Mat4 &m) {
  Frustum f;
  // Left plane
  f.planes[0].normal = {m.m[3][0] + m.m[0][0], m.m[3][1] + m.m[0][1],
                        m.m[3][2] + m.m[0][2]};
  f.planes[0].distance = m.m[3][3] + m.m[0][3];
  // Right plane
  f.planes[1].normal = {m.m[3][0] - m.m[0][0], m.m[3][1] - m.m[0][1],
                        m.m[3][2] - m.m[0][2]};
  f.planes[1].distance = m.m[3][3] - m.m[0][3];
  // Bottom plane
  f.planes[2].normal = {m.m[3][0] + m.m[1][0], m.m[3][1] + m.m[1][1],
                        m.m[3][2] + m.m[1][2]};
  f.planes[2].distance = m.m[3][3] + m.m[1][3];
  // Top plane
  f.planes[3].normal = {m.m[3][0] - m.m[1][0], m.m[3][1] - m.m[1][1],
                        m.m[3][2] - m.m[1][2]};
  f.planes[3].distance = m.m[3][3] - m.m[1][3];
  // Near plane
  f.planes[4].normal = {m.m[3][0] + m.m[2][0], m.m[3][1] + m.m[2][1],
                        m.m[3][2] + m.m[2][2]};
  f.planes[4].distance = m.m[3][3] + m.m[2][3];
  // Far plane
  f.planes[5].normal = {m.m[3][0] - m.m[2][0], m.m[3][1] - m.m[2][1],
                        m.m[3][2] - m.m[2][2]};
  f.planes[5].distance = m.m[3][3] - m.m[2][3];

  // Normalize frustum planes
  for (int i = 0; i < 6; ++i) {
    const float l =
        Kernel::Lib::Math::sqrt(f.planes[i].normal.x * f.planes[i].normal.x +
                                f.planes[i].normal.y * f.planes[i].normal.y +
                                f.planes[i].normal.z * f.planes[i].normal.z);
    if (l > 0.0001f) {
      f.planes[i].normal = f.planes[i].normal * (1.0f / l);
      f.planes[i].distance /= l;
    }
  }
  return f;
}

void Renderer3D::render_mesh(VirtualFramebuffer &vfb, const Vertex *vertices,
                             size_t vertex_count, const uint32_t *indices,
                             size_t index_count, const BoundingSphere &bounds) {
  if (!vertices || !indices || index_count < 3)
    return;

  // 1. Frustum Culling on Mesh Object Level
  const Frustum frustum = extract_frustum(mat_mvp_);
  if (bounds.radius > 0.0f && !frustum.intersects_sphere(bounds)) {
    return; // Entire mesh culled!
  }

  const float hw = static_cast<float>(vp_w_) * 0.5f;
  const float hh = static_cast<float>(vp_h_) * 0.5f;

  for (size_t i = 0; i < index_count; i += 3) {
    const Vertex &v0 = vertices[indices[i]];
    const Vertex &v1 = vertices[indices[i + 1]];
    const Vertex &v2 = vertices[indices[i + 2]];

    // 2. Transform Vertices to Clip Space
    Vec4 clip0 = mat_mvp_ * Vec4(v0.position, 1.0f);
    Vec4 clip1 = mat_mvp_ * Vec4(v1.position, 1.0f);
    Vec4 clip2 = mat_mvp_ * Vec4(v2.position, 1.0f);

    // 3. Early Near-Z Plane Rejection (W <= 0)
    if (clip0.w <= 0.001f && clip1.w <= 0.001f && clip2.w <= 0.001f) {
      continue;
    }

    // 4. Perspective Divide to Screen Coordinates
    const float inv_w0 = 1.0f / clip0.w;
    const float inv_w1 = 1.0f / clip1.w;
    const float inv_w2 = 1.0f / clip2.w;

    Vertex s0 = v0;
    s0.position = {(clip0.x * inv_w0 + 1.0f) * hw + vp_x_,
                   (1.0f - clip0.y * inv_w0) * hh + vp_y_, clip0.z * inv_w0};
    Vertex s1 = v1;
    s1.position = {(clip1.x * inv_w1 + 1.0f) * hw + vp_x_,
                   (1.0f - clip1.y * inv_w1) * hh + vp_y_, clip1.z * inv_w1};
    Vertex s2 = v2;
    s2.position = {(clip2.x * inv_w2 + 1.0f) * hw + vp_x_,
                   (1.0f - clip2.y * inv_w2) * hh + vp_y_, clip2.z * inv_w2};

    // 5. Backface Culling via 2D Cross Product
    const float edge_cross =
        (s1.position.x - s0.position.x) * (s2.position.y - s0.position.y) -
        (s1.position.y - s0.position.y) * (s2.position.x - s0.position.x);

    if (edge_cross >= 0.0f) {
      continue; // Back-facing or degenerate triangle culled
    }

    std::swap(s1, s2);

    // 6. Flat Directional Shading calculation
    Vec3 face_normal =
        (v1.position - v0.position).cross(v2.position - v0.position);
    const float nl = Kernel::Lib::Math::sqrt(face_normal.dot(face_normal));
    if (nl > 0.0001f)
      face_normal = face_normal * (1.0f / nl);

    const float intensity = std::max(0.2f, -face_normal.dot(light_dir_));
    const uint8_t r =
        static_cast<uint8_t>(((v0.color >> 16) & 0xFF) * intensity);
    const uint8_t g =
        static_cast<uint8_t>(((v0.color >> 8) & 0xFF) * intensity);
    const uint8_t b = static_cast<uint8_t>((v0.color & 0xFF) * intensity);
    const uint32_t lit_color = (r << 16) | (g << 8) | b;

    s0.color = s1.color = s2.color = lit_color;

    // 7. Rasterization
    rasterize_triangle(vfb, s0, s1, s2);
  }
}

// Fixed-point Barycentric Edge-Function Rasterizer
void Renderer3D::rasterize_triangle(VirtualFramebuffer &vfb, const Vertex &v0,
                                    const Vertex &v1, const Vertex &v2) {
  // Convert 2D screen positions to 16.16 Fixed Point
  const int32_t x0 = float_to_fp(v0.position.x);
  const int32_t y0 = float_to_fp(v0.position.y);
  const int32_t x1 = float_to_fp(v1.position.x);
  const int32_t y1 = float_to_fp(v1.position.y);
  const int32_t x2 = float_to_fp(v2.position.x);
  const int32_t y2 = float_to_fp(v2.position.y);

  // Bounding Box Integer Bounds
  int32_t min_x = std::min({x0, x1, x2}) >> FP_SHIFT;
  int32_t max_x = (std::max({x0, x1, x2}) + FP_ONE - 1) >> FP_SHIFT;
  int32_t min_y = std::min({y0, y1, y2}) >> FP_SHIFT;
  int32_t max_y = (std::max({y0, y1, y2}) + FP_ONE - 1) >> FP_SHIFT;

  // Clip Screen Bounds
  min_x = std::max(min_x, static_cast<int32_t>(vp_x_));
  max_x = std::min(max_x, static_cast<int32_t>(vp_x_ + vp_w_ - 1));
  min_y = std::max(min_y, static_cast<int32_t>(vp_y_));
  max_y = std::min(max_y, static_cast<int32_t>(vp_y_ + vp_h_ - 1));

  if (min_x > max_x || min_y > max_y)
    return;

  // Triangle Area Constant
  const int64_t area = static_cast<int64_t>(x1 - x0) * (y2 - y0) -
                       static_cast<int64_t>(y1 - y0) * (x2 - x0);
  if (area <= 0)
    return;

  const float inv_area = 1.0f / static_cast<float>(area);

  // Edge Setup
  const int32_t A01 = y0 - y1, B01 = x1 - x0;
  const int32_t A12 = y1 - y2, B12 = x2 - x1;
  const int32_t A20 = y2 - y0, B20 = x0 - x2;

  // Start position evaluation
  const int32_t px = (min_x << FP_SHIFT) + (FP_ONE >> 1);
  const int32_t py = (min_y << FP_SHIFT) + (FP_ONE >> 1);

  int64_t w0_row = static_cast<int64_t>(x2 - x1) * (py - y1) -
                   static_cast<int64_t>(y2 - y1) * (px - x1);
  int64_t w1_row = static_cast<int64_t>(x0 - x2) * (py - y2) -
                   static_cast<int64_t>(y0 - y2) * (px - x2);
  int64_t w2_row = static_cast<int64_t>(x1 - x0) * (py - y0) -
                   static_cast<int64_t>(y1 - y0) * (px - x0);

  for (int32_t y = min_y; y <= max_y; ++y) {
    int64_t w0 = w0_row;
    int64_t w1 = w1_row;
    int64_t w2 = w2_row;

    for (int32_t x = min_x; x <= max_x; ++x) {
      if ((w0 | w1 | w2) >= 0) {
        const float b0 = static_cast<float>(w0) * inv_area;
        const float b1 = static_cast<float>(w1) * inv_area;
        const float b2 = static_cast<float>(w2) * inv_area;

        const float z =
            b0 * v0.position.z + b1 * v1.position.z + b2 * v2.position.z;

        if (depth_buffer_.test_and_set(x, y, z)) {
          vfb.put_pixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                        v0.color);
        }
      }
      w0 += static_cast<int64_t>(A12) << FP_SHIFT;
      w1 += static_cast<int64_t>(A20) << FP_SHIFT;
      w2 += static_cast<int64_t>(A01) << FP_SHIFT;
    }

    w0_row += static_cast<int64_t>(B12) << FP_SHIFT;
    w1_row += static_cast<int64_t>(B20) << FP_SHIFT;
    w2_row += static_cast<int64_t>(B01) << FP_SHIFT;
  }
}

} // namespace Kernel::Graphics::K3D