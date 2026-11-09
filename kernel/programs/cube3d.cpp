#include <arch/x86_64/string.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/mouse.h>
#include <kernel/graphics/k3d.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/uicommon.h>
#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>

#include <utility>

namespace Kernel::Apps::Cube3D {

using namespace Kernel::Graphics::K3D;

void run() {
  Graphics::VirtualFramebuffer vfb;
  if (!vfb.open_window(1024, 720))
    return;

  const auto sw = static_cast<int32_t>(vfb.width());
  const auto sh = static_cast<int32_t>(vfb.height());
  const uint32_t canvas_w = sw;
  const uint32_t canvas_h = sh > 120 ? sh - 120 : sh;

  const uint32_t COLOR_BG = 0xFF1E1E2E;
  const uint32_t COLOR_TEXT = 0xFFCDD6F4;

  Renderer3D renderer;
  if (!renderer.init(canvas_w, canvas_h))
    return;

  // Define 3D Cube Geometry (8 vertices, 12 triangles / 36 indices)
  Vertex cube_vertices[8] = {
      {{-1.0f, -1.0f, -1.0f}, {0, 0, 0}, 0xFF89B4FA}, // 0: -x, -y, -z
      {{1.0f, -1.0f, -1.0f}, {0, 0, 0}, 0xFFA6E3A1},  // 1: +x, -y, -z
      {{1.0f, 1.0f, -1.0f}, {0, 0, 0}, 0xFFF9E2AF},   // 2: +x, +y, -z
      {{-1.0f, 1.0f, -1.0f}, {0, 0, 0}, 0xFFFAB387},  // 3: -x, +y, -z
      {{-1.0f, -1.0f, 1.0f}, {0, 0, 0}, 0xFFF38BA8},  // 4: -x, -y, +z
      {{1.0f, -1.0f, 1.0f}, {0, 0, 0}, 0xFFCBA6F7},   // 5: +x, -y, +z
      {{1.0f, 1.0f, 1.0f}, {0, 0, 0}, 0xFF94E2D5},    // 6: +x, +y, +z
      {{-1.0f, 1.0f, 1.0f}, {0, 0, 0}, 0xFF89DCEB}    // 7: -x, +y, +z
  };

  uint32_t cube_indices[36] = {// Front face
                               4, 5, 6, 4, 6, 7,
                               // Back face
                               1, 0, 3, 1, 3, 2,
                               // Left face
                               0, 4, 7, 0, 7, 3,
                               // Right face
                               5, 1, 2, 5, 2, 6,
                               // Top face
                               3, 7, 6, 3, 6, 2,
                               // Bottom face
                               0, 1, 5, 0, 5, 4};

  BoundingSphere cube_bounds{{0.0f, 0.0f, 0.0f},
                             1.732f}; // Bounding sphere for culling

  // --- UI Widgets in Control Panel ---
  Kernel::UICommon::Button btn_auto(10, canvas_h + 8, 95, 24, "Toggle Auto");
  Kernel::UICommon::Button btn_reset(115, canvas_h + 8, 85, 24, "Reset View");
  Kernel::UICommon::Button btn_quit(210, canvas_h + 8, 65, 24, "Quit",
                                    0xFFF38BA8, 0xFF1E1E2E, 0);

  Kernel::UICommon::Label lbl_rotx(290, canvas_h + 14, "Rot X:");
  Kernel::UICommon::Slider slider_rotx(335, canvas_h + 12, 110, 20, 0, 360, 45);

  Kernel::UICommon::Label lbl_roty(460, canvas_h + 14, "Rot Y:");
  Kernel::UICommon::Slider slider_roty(505, canvas_h + 12, 110, 20, 0, 360, 45);

  Kernel::UICommon::Label lbl_zoom(635, canvas_h + 14, "Zoom:");
  Kernel::UICommon::Slider slider_zoom(680, canvas_h + 12, 110, 20, 10, 100,
                                       45);

  char status_msg[64] =
      "3D Software Renderer Active. Frustum & Backface Culling Enabled";
  Kernel::UICommon::StatusBar status_bar(10, canvas_h + 50, sw - 20, 28,
                                         status_msg);

  bool auto_rotate = true;
  bool running = true;
  bool old_left = false;
  int prev_mx = -1;
  int prev_my = -1;
  bool panel_redraw_needed = true;

  float angle_x = 0.0f;
  float angle_y = 0.0f;

  while (running && !Kernel::Scheduler::should_stop()) {
    int mx = Kernel::AppCommon::get_mouse_x();
    int my = Kernel::AppCommon::get_mouse_y();
    bool m_left = Kernel::AppCommon::is_left_pressed();
    bool m_released = old_left && !m_left;

    if (mx != prev_mx || my != prev_my) {
      panel_redraw_needed = true;
      prev_mx = mx;
      prev_my = my;
    }

    if (my >= static_cast<int>(canvas_h)) {
      // --- UI Panel Interaction ---
      if (btn_auto.handle_mouse(mx, my, m_left, m_released)) {
        auto_rotate = !auto_rotate;
        strcpy(status_msg, auto_rotate ? "Auto-rotation enabled."
                                       : "Auto-rotation paused.");
        status_bar.set_status(status_msg);
        panel_redraw_needed = true;
      }
      if (btn_reset.handle_mouse(mx, my, m_left, m_released)) {
        slider_rotx.set_value(45);
        slider_roty.set_value(45);
        slider_zoom.set_value(45);
        strcpy(status_msg, "View reset to defaults.");
        status_bar.set_status(status_msg);
        panel_redraw_needed = true;
      }
      if (btn_quit.handle_mouse(mx, my, m_left, m_released)) {
        running = false;
      }

      if (slider_rotx.handle_mouse(mx, my, m_left, m_released))
        panel_redraw_needed = true;
      if (slider_roty.handle_mouse(mx, my, m_left, m_released))
        panel_redraw_needed = true;
      if (slider_zoom.handle_mouse(mx, my, m_left, m_released))
        panel_redraw_needed = true;
    } else {
      // --- 3D Viewport Mouse Interaction (Drag to Orbit) ---
      static int last_drag_x = mx;
      static int last_drag_y = my;
      if (m_left) {
        if (!old_left) {
          last_drag_x = mx;
          last_drag_y = my;
        } else {
          int dx = mx - last_drag_x;
          int dy = my - last_drag_y;
          int cur_x_val = slider_rotx.get_value() - dy;
          int cur_y_val = slider_roty.get_value() - dx;
          slider_rotx.set_value((cur_x_val % 360 + 360) % 360);
          slider_roty.set_value((cur_y_val % 360 + 360) % 360);
          last_drag_x = mx;
          last_drag_y = my;
          panel_redraw_needed = true;
        }
      }
    }

    if (auto_rotate && my < static_cast<int>(canvas_h)) {
      int ry = (slider_roty.get_value() + 1) % 360;
      slider_roty.set_value(ry);
      panel_redraw_needed = true;
    }

    old_left = m_left;

    // --- Render Frame ---
    vfb.clear(COLOR_BG);
    renderer.clear_depth(1.0f);

    angle_x =
        static_cast<float>(slider_rotx.get_value()) * (3.14159265f / 180.0f);
    angle_y =
        static_cast<float>(slider_roty.get_value()) * (3.14159265f / 180.0f);
    float zoom = static_cast<float>(slider_zoom.get_value()) * 0.1f;

    Mat4 model = Mat4::rotation_x(angle_x) * Mat4::rotation_y(angle_y);
    Mat4 view = Mat4::translation(0.0f, 0.0f, -4.0f - zoom);
    Mat4 proj = Mat4::perspective(1.04719755f,
                                  static_cast<float>(canvas_w) /
                                      static_cast<float>(canvas_h),
                                  0.1f, 100.0f);

    renderer.set_viewport(0, 0, canvas_w, canvas_h);
    renderer.set_matrices(model, view, proj);
    renderer.set_light_direction(Vec3(0.577f, 0.577f, -0.577f));

    // Render mesh with aggressive culling enabled
    renderer.render_mesh(vfb, cube_vertices, 8, cube_indices, 36, cube_bounds);

    // --- Draw UI Control Panel Background ---
    vfb.fill_rect(0, canvas_h, sw, 120, 0xFF313244);
    vfb.draw_rect(0, canvas_h, sw, 120, 0xFF45475A);

    btn_auto.draw(vfb);
    btn_reset.draw(vfb);
    btn_quit.draw(vfb);

    lbl_rotx.draw(vfb);
    slider_rotx.draw(vfb);

    lbl_roty.draw(vfb);
    slider_roty.draw(vfb);

    lbl_zoom.draw(vfb);
    slider_zoom.draw(vfb);

    status_bar.draw(vfb);

    vfb.flush();

    if (!auto_rotate && !panel_redraw_needed) {
      Kernel::Scheduler::thread_sleep_ms(16); // Idle frame cap (~60 FPS)
    } else {
      Kernel::Scheduler::thread_sleep_ms(5);
    }
  }
}

} // namespace Kernel::Apps::Cube3D