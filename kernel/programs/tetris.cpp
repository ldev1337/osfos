#include <cstdint>
#include <kernel/drivers/keyboard.h>
#include <kernel/drivers/pit.h>
#include <kernel/graphics/vfb.h>
#include <kernel/lib/appcommon.h>
#include <kernel/lib/random.h>
#include <kernel/scheduler.h>

#include <algorithm>

namespace Kernel::Games::Tetris {
namespace {
static Kernel::Graphics::VirtualFramebuffer vfb;
static Kernel::Lib::Random rng(Kernel::Lib::Random::rdtsc());
constexpr int BOARD_W = 10;
constexpr int BOARD_H = 20;
int block_size = 26;
constexpr uint32_t COLOR_BG = 0x101018;
constexpr uint32_t COLOR_BOARD_BG = 0x08080C;
constexpr uint32_t COLOR_GRID = 0x222233;
constexpr uint32_t COLOR_BORDER = 0x444466;
constexpr uint32_t COLOR_TEXT = 0xFFFFFF;
constexpr uint32_t COLOR_DIM_TEXT = 0xAAAAAA;
constexpr uint32_t TETROMINO_COLORS[8] = {0x000000, 0x00F0F0, 0x0000F0,
                                          0xF0A000, 0xF0F000, 0x00F000,
                                          0xA000F0, 0xF00000};

const int TETROMINOES[8][4][4][4] = {
    {{{0}}},
    {{{0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0}},
     {{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}},
     {{0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}}},
    {{{2, 0, 0, 0}, {2, 2, 2, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 2, 2, 0}, {0, 2, 0, 0}, {0, 2, 0, 0}, {0, 0, 0, 0}},
     {{0, 0, 0, 0}, {2, 2, 2, 0}, {0, 0, 2, 0}, {0, 0, 0, 0}},
     {{0, 2, 0, 0}, {0, 2, 0, 0}, {2, 2, 0, 0}, {0, 0, 0, 0}}},
    {{{0, 0, 3, 0}, {3, 3, 3, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 3, 0, 0}, {0, 3, 0, 0}, {0, 3, 3, 0}, {0, 0, 0, 0}},
     {{0, 0, 0, 0}, {3, 3, 3, 0}, {3, 0, 0, 0}, {0, 0, 0, 0}},
     {{3, 3, 0, 0}, {0, 3, 0, 0}, {0, 3, 0, 0}, {0, 0, 0, 0}}},
    {{{0, 4, 4, 0}, {0, 4, 4, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 4, 4, 0}, {0, 4, 4, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 4, 4, 0}, {0, 4, 4, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 4, 4, 0}, {0, 4, 4, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}},
    {{{0, 5, 5, 0}, {5, 5, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 5, 0, 0}, {0, 5, 5, 0}, {0, 0, 5, 0}, {0, 0, 0, 0}},
     {{0, 0, 0, 0}, {0, 5, 5, 0}, {5, 5, 0, 0}, {0, 0, 0, 0}},
     {{5, 0, 0, 0}, {5, 5, 0, 0}, {0, 5, 0, 0}, {0, 0, 0, 0}}},
    {{{0, 6, 0, 0}, {6, 6, 6, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 6, 0, 0}, {0, 6, 6, 0}, {0, 6, 0, 0}, {0, 0, 0, 0}},
     {{0, 0, 0, 0}, {6, 6, 6, 0}, {0, 6, 0, 0}, {0, 0, 0, 0}},
     {{0, 6, 0, 0}, {6, 6, 0, 0}, {0, 6, 0, 0}, {0, 0, 0, 0}}},
    {{{7, 7, 0, 0}, {0, 7, 7, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
     {{0, 0, 7, 0}, {0, 7, 7, 0}, {0, 7, 0, 0}, {0, 0, 0, 0}},
     {{0, 0, 0, 0}, {7, 7, 0, 0}, {0, 7, 7, 0}, {0, 0, 0, 0}},
     {{0, 7, 0, 0}, {7, 7, 0, 0}, {7, 0, 0, 0}, {0, 0, 0, 0}}}};

int bag[7];
int bag_index = 7;

void init_bag() {
  for (int i = 0; i < 7; ++i) {
    bag[i] = i + 1;
  }
  for (int i = 6; i > 0; --i) {
    uint32_t j = rng.next_range(0, i);
    int temp = bag[i];
    bag[i] = bag[j];
    bag[j] = temp;
  }
  bag_index = 0;
}

int get_next_piece_from_bag() {
  if (bag_index >= 7) {
    init_bag();
  }
  return bag[bag_index++];
}

int board[BOARD_H][BOARD_W];
int score = 0;
int lines = 0;
int level = 1;
bool game_over = false;

struct Piece {
  int type;
  int rot;
  int x, y;
} current_piece, next_piece;

void reset_board() {
  for (int r = 0; r < BOARD_H; ++r) {
    for (int c = 0; c < BOARD_W; ++c) {
      board[r][c] = 0;
    }
  }
}

void spawn_piece() {
  current_piece = next_piece;
  next_piece.type = get_next_piece_from_bag();
  next_piece.rot = 0;
  current_piece.x = BOARD_W / 2 - 2;
  current_piece.y = 0;

  auto &shape = TETROMINOES[current_piece.type][current_piece.rot];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (shape[r][c] != 0) {
        int bx = current_piece.x + c;
        int by = current_piece.y + r;
        if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
          if (board[by][bx] != 0) {
            game_over = true;
          }
        }
      }
    }
  }
}

bool check_collision(const Piece &p) {
  auto &shape = TETROMINOES[p.type][p.rot];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (shape[r][c] != 0) {
        int bx = p.x + c;
        int by = p.y + r;
        if (bx < 0 || bx >= BOARD_W || by >= BOARD_H) {
          return true;
        }
        if (by >= 0 && board[by][bx] != 0) {
          return true;
        }
      }
    }
  }
  return false;
}

void lock_piece() {
  auto &shape = TETROMINOES[current_piece.type][current_piece.rot];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (shape[r][c] != 0) {
        int bx = current_piece.x + c;
        int by = current_piece.y + r;
        if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
          board[by][bx] = current_piece.type;
        }
      }
    }
  }

  int cleared = 0;
  for (int r = BOARD_H - 1; r >= 0; --r) {
    bool full = true;
    for (int c = 0; c < BOARD_W; ++c) {
      if (board[r][c] == 0) {
        full = false;
        break;
      }
    }
    if (full) {
      cleared++;
      for (int move_r = r; move_r > 0; --move_r) {
        for (int c = 0; c < BOARD_W; ++c) {
          board[move_r][c] = board[move_r - 1][c];
        }
      }
      for (int c = 0; c < BOARD_W; ++c) {
        board[0][c] = 0;
      }
      r++;
    }
  }

  if (cleared > 0) {
    lines += cleared;
    level = 1 + lines / 10;
    int pts[5] = {0, 100, 300, 500, 800};
    score += pts[cleared > 4 ? 4 : cleared] * level;
  }

  spawn_piece();
}

void draw_block(int x, int y, int type, int ox, int oy) {
  uint32_t col = TETROMINO_COLORS[type];
  int px = ox + x * block_size;
  int py = oy + y * block_size;
  vfb.fill_rect(px + 1, py + 1, block_size - 2, block_size - 2, col);
  vfb.draw_rect(px, py, block_size, block_size, COLOR_GRID);
}

void render(int ox, int oy) {
  vfb.fill(COLOR_BG);

  int board_w_px = BOARD_W * block_size;
  int board_h_px = BOARD_H * block_size;
  vfb.fill_rect(ox - 2, oy - 2, board_w_px + 4, board_h_px + 4, COLOR_BORDER);
  vfb.fill_rect(ox, oy, board_w_px, board_h_px, COLOR_BOARD_BG);

  for (int r = 0; r < BOARD_H; ++r) {
    for (int c = 0; c < BOARD_W; ++c) {
      if (board[r][c] != 0) {
        draw_block(c, r, board[r][c], ox, oy);
      } else {
        vfb.draw_rect(ox + c * block_size, oy + r * block_size, block_size,
                      block_size, COLOR_GRID);
      }
    }
  }

  auto &shape = TETROMINOES[current_piece.type][current_piece.rot];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (shape[r][c] != 0) {
        int bx = current_piece.x + c;
        int by = current_piece.y + r;
        if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
          draw_block(bx, by, current_piece.type, ox, oy);
        }
      }
    }
  }

  int sb_x = ox + board_w_px + 20;
  int sb_y = oy;

  Kernel::AppCommon::text(vfb, sb_x, sb_y, "TETRIS", 0x00FFFF);
  vfb.draw_line(sb_x, sb_y + 12, sb_x + 140, sb_y + 12, COLOR_BORDER);
  Kernel::AppCommon::text(vfb, sb_x, sb_y + 22, "SCORE:", COLOR_DIM_TEXT);
  char score_buf[32];
  int s = score;
  int idx = 0;
  if (s == 0) {
    score_buf[0] = '0';
    score_buf[1] = 0;
  } else {
    char temp[32];
    int ti = 0;
    while (s > 0) {
      temp[ti++] = '0' + (s % 10);
      s /= 10;
    }
    while (ti > 0) {
      score_buf[idx++] = temp[--ti];
    }
    score_buf[idx] = 0;
  }
  Kernel::AppCommon::text(vfb, sb_x, sb_y + 34, score_buf, COLOR_TEXT);

  Kernel::AppCommon::text(vfb, sb_x, sb_y + 58, "NEXT:", COLOR_DIM_TEXT);
  vfb.draw_rect(sb_x, sb_y + 72, 110, 80, COLOR_BORDER);
  vfb.fill_rect(sb_x + 1, sb_y + 73, 108, 78, COLOR_BOARD_BG);

  auto &next_shape = TETROMINOES[next_piece.type][0];
  int p_ox = sb_x + 20;
  int p_oy = sb_y + 84;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      if (next_shape[r][c] != 0) {
        int px = p_ox + c * 16;
        int py = p_oy + r * 16;
        vfb.fill_rect(px + 1, py + 1, 14, 14,
                      TETROMINO_COLORS[next_piece.type]);
        vfb.draw_rect(px, py, 16, 16, COLOR_GRID);
      }
    }
  }
  Kernel::AppCommon::text(vfb, sb_x, sb_y + 162, "LEVEL:", COLOR_DIM_TEXT);
  char lvl_buf[16] = {static_cast<char>('0' + (level / 10)),
                      static_cast<char>('0' + (level % 10)), 0};
  Kernel::AppCommon::text(vfb, sb_x, sb_y + 174, lvl_buf, COLOR_TEXT);

  Kernel::AppCommon::text(vfb, sb_x, sb_y + 200, "LINES:", COLOR_DIM_TEXT);
  char lns_buf[16];
  int l = lines;
  int li = 0;
  if (l == 0) {
    lns_buf[0] = '0';
    lns_buf[1] = 0;
  } else {
    char temp[16];
    int ti = 0;
    while (l > 0) {
      temp[ti++] = '0' + (l % 10);
      l /= 10;
    }
    while (ti > 0) {
      lns_buf[li++] = temp[--ti];
    }
    lns_buf[li] = 0;
  }
  Kernel::AppCommon::text(vfb, sb_x, sb_y + 212, lns_buf, COLOR_TEXT);

  int ctrl_y = sb_y + 245;
  Kernel::AppCommon::text(vfb, sb_x, ctrl_y, "CONTROLS:", 0x8888AA);
  Kernel::AppCommon::text(vfb, sb_x, ctrl_y + 16, "LEFT/RIGHT: Move",
                          COLOR_DIM_TEXT);
  Kernel::AppCommon::text(vfb, sb_x, ctrl_y + 30, "UP/X: Rotate CW",
                          COLOR_DIM_TEXT);
  Kernel::AppCommon::text(vfb, sb_x, ctrl_y + 44, "Z: Rotate CCW",
                          COLOR_DIM_TEXT);
  Kernel::AppCommon::text(vfb, sb_x, ctrl_y + 58, "DOWN: Soft Drop",
                          COLOR_DIM_TEXT);
  Kernel::AppCommon::text(vfb, sb_x, ctrl_y + 72, "SPACE: Hard Drop",
                          COLOR_DIM_TEXT);
  Kernel::AppCommon::text(vfb, sb_x, ctrl_y + 86, "ESC: Quit", COLOR_DIM_TEXT);

  if (game_over) {
    int ov_w = 260;
    int ov_h = 100;
    int ov_x = ox + (board_w_px - ov_w) / 2;
    int ov_y = oy + (board_h_px - ov_h) / 2;
    vfb.fill_rect(ov_x, ov_y, ov_w, ov_h, 0x1A1A26);
    vfb.draw_rect(ov_x, ov_y, ov_w, ov_h, 0xFF0000);
    Kernel::AppCommon::text(vfb, ov_x + 75, ov_y + 20, "GAME OVER", 0xFF4444);
    Kernel::AppCommon::text(vfb, ov_x + 35, ov_y + 50, "PRESS R TO RESTART",
                            COLOR_TEXT);
    Kernel::AppCommon::text(vfb, ov_x + 35, ov_y + 70, "PRESS ESC TO EXIT",
                            COLOR_DIM_TEXT);
  }

  vfb.flush();
}
} // namespace

void run() {
  if (!vfb.open_window(500, 560, 260, 180))
    return;
  const int sw = static_cast<int>(vfb.width());
  const int sh = static_cast<int>(vfb.height());

  block_size = (sw - 220) / BOARD_W;
  block_size = std::min(block_size, sh / BOARD_H);
  block_size = std::min(block_size, 26);
  block_size = std::max(block_size, 8);
  int board_w_px = BOARD_W * block_size;
  int board_h_px = BOARD_H * block_size;
  int total_w = board_w_px + 180 + 30;
  int ox = (sw - total_w) / 2;
  int oy = (sh - board_h_px) / 2;
  ox = std::max(ox, 10);
  oy = std::max(oy, 10);

  init_bag();
  reset_board();

  next_piece.type = get_next_piece_from_bag();
  next_piece.rot = 0;
  spawn_piece();
  score = 0;
  lines = 0;
  level = 1;
  game_over = false;

  uint32_t last_drop_tick = Kernel::Drivers::PIT::get_ticks();
  uint32_t last_key_tick = 0;

  for (;;) {
    if (Kernel::Scheduler::should_stop())
      return;
    uint32_t current_tick = Kernel::Drivers::PIT::get_ticks();

    if (Kernel::Drivers::Keyboard::is_pressed(0x01)) {
      break;
    }

    if (game_over) {
      render(ox, oy);
      if (Kernel::Drivers::Keyboard::is_pressed(0x13)) {
        reset_board();
        score = 0;
        lines = 0;
        level = 1;
        game_over = false;
        init_bag();
        next_piece.type = get_next_piece_from_bag();
        spawn_piece();
      }
      Kernel::AppCommon::delay(5);
      continue;
    }

    if (current_tick - last_key_tick > 4) {
      bool moved = false;
      Piece test_p = current_piece;

      if (Kernel::Drivers::Keyboard::is_extended_pressed(0x4B)) {
        test_p.x--;
        if (!check_collision(test_p)) {
          current_piece = test_p;
          moved = true;
        }
      }

      else if (Kernel::Drivers::Keyboard::is_extended_pressed(0x4D)) {
        test_p.x++;
        if (!check_collision(test_p)) {
          current_piece = test_p;
          moved = true;
        }
      }

      else if (Kernel::Drivers::Keyboard::is_extended_pressed(0x50)) {
        test_p.y++;
        if (!check_collision(test_p)) {
          current_piece = test_p;
          score += 1;
          moved = true;
        } else {
          lock_piece();
        }
      }

      else if (Kernel::Drivers::Keyboard::is_extended_pressed(0x48) ||
               Kernel::Drivers::Keyboard::is_pressed(0x2D)) {
        test_p.rot = (test_p.rot + 1) % 4;
        if (!check_collision(test_p)) {
          current_piece = test_p;
          moved = true;
        } else {

          test_p.x--;
          if (!check_collision(test_p)) {
            current_piece = test_p;
            moved = true;
          } else {
            test_p.x += 2;
            if (!check_collision(test_p)) {
              current_piece = test_p;
              moved = true;
            }
          }
        }
      }

      else if (Kernel::Drivers::Keyboard::is_pressed(0x2C)) {
        test_p.rot = (test_p.rot + 3) % 4;
        if (!check_collision(test_p)) {
          current_piece = test_p;
          moved = true;
        }
      }

      else if (Kernel::Drivers::Keyboard::is_pressed(0x39)) {
        int drop_dist = 0;
        while (!check_collision(test_p)) {
          test_p.y++;
          drop_dist++;
        }
        test_p.y--;
        current_piece = test_p;
        score += drop_dist * 2;
        lock_piece();
        moved = true;
      }

      if (moved) {
        last_key_tick = current_tick;
      }
    }

    uint32_t drop_interval = (22 - (level * 2)) * 5;
    drop_interval = std::max<uint32_t>(drop_interval, 10);

    if (current_tick - last_drop_tick >= drop_interval) {
      Piece test_p = current_piece;
      test_p.y++;
      if (!check_collision(test_p)) {
        current_piece = test_p;
      } else {
        lock_piece();
      }
      last_drop_tick = current_tick;
    }

    render(ox, oy);
    Kernel::AppCommon::delay(2);
  }
}

} // namespace Kernel::Games::Tetris
