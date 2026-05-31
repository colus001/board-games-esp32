#include <Arduino.h>
#include <esp32_smartdisplay.h>
#include <lvgl.h>

LV_FONT_DECLARE(chess_symbols_42);

namespace {
constexpr int kScreenPixels = 480;
constexpr int kStatusHeight = 24;
constexpr int kBoardPixels = 456;
constexpr int kSquarePixels = kBoardPixels / 8;
constexpr int kBoardOffsetX = (kScreenPixels - kBoardPixels) / 2;
constexpr int kBoardOffsetY = 0;

lv_obj_t *board_frame = nullptr;
lv_obj_t *squares[8][8];
lv_obj_t *move_markers[8][8];
lv_obj_t *piece_labels[8][8];
lv_obj_t *rank_labels[8];
lv_obj_t *file_labels[8];
lv_obj_t *status_label = nullptr;
uint8_t square_ids[64];

int selected_row = -1;
int selected_col = -1;
bool white_turn = true;
bool game_over = false;
const char *status_message = "White to move";

char board[8][8] = {
    {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'},
    {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'},
    {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'},
};

bool is_light_square(int row, int col) {
  return ((row + col) % 2) == 0;
}

bool is_white_piece(char piece) {
  return piece >= 'A' && piece <= 'Z';
}

bool is_black_piece(char piece) {
  return piece >= 'a' && piece <= 'z';
}

bool is_current_turn_piece(char piece) {
  return white_turn ? is_white_piece(piece) : is_black_piece(piece);
}

bool same_side(char a, char b) {
  return (is_white_piece(a) && is_white_piece(b)) || (is_black_piece(a) && is_black_piece(b));
}

char lower_piece(char piece) {
  if (piece >= 'A' && piece <= 'Z') {
    return piece - 'A' + 'a';
  }
  return piece;
}

int abs_int(int value) {
  return value < 0 ? -value : value;
}

int sign_int(int value) {
  if (value > 0) {
    return 1;
  }
  if (value < 0) {
    return -1;
  }
  return 0;
}

const char *piece_text(char piece) {
  switch (piece) {
  case 'K':
    return "♔";
  case 'Q':
    return "♕";
  case 'R':
    return "♖";
  case 'B':
    return "♗";
  case 'N':
    return "♘";
  case 'P':
    return "♙";
  case 'k':
    return "♚";
  case 'q':
    return "♛";
  case 'r':
    return "♜";
  case 'b':
    return "♝";
  case 'n':
    return "♞";
  case 'p':
    return "♟";
  default:
    return "";
  }
}

bool is_path_clear(int from_row, int from_col, int to_row, int to_col) {
  const int row_step = sign_int(to_row - from_row);
  const int col_step = sign_int(to_col - from_col);
  int row = from_row + row_step;
  int col = from_col + col_step;

  while (row != to_row || col != to_col) {
    if (board[row][col] != '.') {
      return false;
    }
    row += row_step;
    col += col_step;
  }

  return true;
}

bool is_legal_pawn_move(char piece, int from_row, int from_col, int to_row, int to_col) {
  const int direction = is_white_piece(piece) ? -1 : 1;
  const int start_row = is_white_piece(piece) ? 6 : 1;
  const int row_delta = to_row - from_row;
  const int col_delta = to_col - from_col;
  const char target = board[to_row][to_col];

  if (col_delta == 0 && row_delta == direction && target == '.') {
    return true;
  }

  if (col_delta == 0 && from_row == start_row && row_delta == direction * 2 && target == '.') {
    return board[from_row + direction][from_col] == '.';
  }

  if (abs_int(col_delta) == 1 && row_delta == direction && target != '.' && !same_side(piece, target)) {
    return true;
  }

  return false;
}

bool find_king(bool white, int *king_row, int *king_col) {
  const char king = white ? 'K' : 'k';
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      if (board[row][col] == king) {
        *king_row = row;
        *king_col = col;
        return true;
      }
    }
  }
  return false;
}

bool is_legal_basic_move(int from_row, int from_col, int to_row, int to_col) {
  const char piece = board[from_row][from_col];
  const char target = board[to_row][to_col];
  const int row_delta = to_row - from_row;
  const int col_delta = to_col - from_col;
  const int abs_row = abs_int(row_delta);
  const int abs_col = abs_int(col_delta);

  if (piece == '.' || (from_row == to_row && from_col == to_col)) {
    return false;
  }

  if (target != '.' && same_side(piece, target)) {
    return false;
  }

  if (lower_piece(target) == 'k') {
    return false;
  }

  switch (lower_piece(piece)) {
  case 'p':
    return is_legal_pawn_move(piece, from_row, from_col, to_row, to_col);
  case 'r':
    return (row_delta == 0 || col_delta == 0) && is_path_clear(from_row, from_col, to_row, to_col);
  case 'n':
    return (abs_row == 2 && abs_col == 1) || (abs_row == 1 && abs_col == 2);
  case 'b':
    return abs_row == abs_col && is_path_clear(from_row, from_col, to_row, to_col);
  case 'q':
    return ((row_delta == 0 || col_delta == 0) || (abs_row == abs_col)) &&
           is_path_clear(from_row, from_col, to_row, to_col);
  case 'k':
    return abs_row <= 1 && abs_col <= 1;
  default:
    return false;
  }
}

bool piece_attacks_square(int from_row, int from_col, int to_row, int to_col) {
  const char piece = board[from_row][from_col];
  const int row_delta = to_row - from_row;
  const int col_delta = to_col - from_col;
  const int abs_row = abs_int(row_delta);
  const int abs_col = abs_int(col_delta);

  if (piece == '.' || (from_row == to_row && from_col == to_col)) {
    return false;
  }

  switch (lower_piece(piece)) {
  case 'p': {
    const int direction = is_white_piece(piece) ? -1 : 1;
    return row_delta == direction && abs_col == 1;
  }
  case 'r':
    return (row_delta == 0 || col_delta == 0) && is_path_clear(from_row, from_col, to_row, to_col);
  case 'n':
    return (abs_row == 2 && abs_col == 1) || (abs_row == 1 && abs_col == 2);
  case 'b':
    return abs_row == abs_col && is_path_clear(from_row, from_col, to_row, to_col);
  case 'q':
    return ((row_delta == 0 || col_delta == 0) || (abs_row == abs_col)) &&
           is_path_clear(from_row, from_col, to_row, to_col);
  case 'k':
    return abs_row <= 1 && abs_col <= 1;
  default:
    return false;
  }
}

bool is_square_attacked(int row, int col, bool by_white) {
  for (int from_row = 0; from_row < 8; ++from_row) {
    for (int from_col = 0; from_col < 8; ++from_col) {
      const char piece = board[from_row][from_col];
      if (piece == '.') {
        continue;
      }
      if (by_white != is_white_piece(piece)) {
        continue;
      }
      if (piece_attacks_square(from_row, from_col, row, col)) {
        return true;
      }
    }
  }
  return false;
}

bool is_in_check(bool white) {
  int king_row = -1;
  int king_col = -1;
  if (!find_king(white, &king_row, &king_col)) {
    return true;
  }
  return is_square_attacked(king_row, king_col, !white);
}

bool would_leave_king_in_check(int from_row, int from_col, int to_row, int to_col) {
  const char moving_piece = board[from_row][from_col];
  const char captured_piece = board[to_row][to_col];
  const bool moving_white = is_white_piece(moving_piece);

  board[to_row][to_col] = moving_piece;
  board[from_row][from_col] = '.';
  const bool in_check = is_in_check(moving_white);
  board[from_row][from_col] = moving_piece;
  board[to_row][to_col] = captured_piece;

  return in_check;
}

bool is_legal_move(int from_row, int from_col, int to_row, int to_col) {
  return is_legal_basic_move(from_row, from_col, to_row, to_col) &&
         !would_leave_king_in_check(from_row, from_col, to_row, to_col);
}

bool has_any_legal_move(bool white) {
  for (int from_row = 0; from_row < 8; ++from_row) {
    for (int from_col = 0; from_col < 8; ++from_col) {
      const char piece = board[from_row][from_col];
      if (piece == '.' || is_white_piece(piece) != white) {
        continue;
      }
      for (int to_row = 0; to_row < 8; ++to_row) {
        for (int to_col = 0; to_col < 8; ++to_col) {
          if (is_legal_move(from_row, from_col, to_row, to_col)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool has_selection() {
  return selected_row >= 0 && selected_col >= 0;
}

bool is_legal_destination(int row, int col) {
  return has_selection() && is_legal_move(selected_row, selected_col, row, col);
}

void update_status() {
  lv_label_set_text_fmt(status_label, "%s", status_message);
}

void paint_square(int row, int col) {
  const bool selected = row == selected_row && col == selected_col;
  const bool legal_destination = is_legal_destination(row, col);
  const bool capture_destination = legal_destination && board[row][col] != '.';
  const bool light_square = is_light_square(row, col);
  const lv_color_t color = selected
                               ? light_square ? lv_color_hex(0xC9D19D) : lv_color_hex(0x6F8C70)
                               : light_square
                                     ? lv_color_hex(0xEEEED2)
                                     : lv_color_hex(0x4F7EA5);

  lv_obj_set_style_bg_color(squares[row][col], color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(squares[row][col], LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(squares[row][col], 0, LV_PART_MAIN);

  if (legal_destination) {
    lv_obj_remove_flag(move_markers[row][col], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(move_markers[row][col], LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_color(move_markers[row][col], lv_color_hex(0x5D6F3A), LV_PART_MAIN);
    lv_obj_set_style_border_opa(move_markers[row][col], LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(move_markers[row][col], lv_color_hex(0x5D6F3A), LV_PART_MAIN);

    if (capture_destination) {
      lv_obj_set_size(move_markers[row][col], 50, 50);
      lv_obj_set_style_bg_opa(move_markers[row][col], LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(move_markers[row][col], 4, LV_PART_MAIN);
    } else {
      lv_obj_set_size(move_markers[row][col], 15, 15);
      lv_obj_set_style_bg_opa(move_markers[row][col], LV_OPA_60, LV_PART_MAIN);
      lv_obj_set_style_border_width(move_markers[row][col], 0, LV_PART_MAIN);
    }
    lv_obj_center(move_markers[row][col]);
  } else {
    lv_obj_add_flag(move_markers[row][col], LV_OBJ_FLAG_HIDDEN);
  }

  const char piece = board[row][col];
  const bool white_piece = is_white_piece(piece);

  lv_label_set_text(piece_labels[row][col], piece_text(piece));
  lv_obj_set_style_text_color(piece_labels[row][col],
                              white_piece ? lv_color_hex(0xFFD166) : lv_color_hex(0x1F2933),
                              LV_PART_MAIN);

  if (col == 0) {
    lv_obj_set_style_text_color(rank_labels[row],
                                light_square ? lv_color_hex(0x4F7EA5) : lv_color_hex(0xEEEED2),
                                LV_PART_MAIN);
  }
  if (row == 7) {
    lv_obj_set_style_text_color(file_labels[col],
                                light_square ? lv_color_hex(0x4F7EA5) : lv_color_hex(0xEEEED2),
                                LV_PART_MAIN);
  }
}

void repaint_board() {
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      paint_square(row, col);
    }
  }
  update_status();
}

void clear_selection() {
  selected_row = -1;
  selected_col = -1;
}

void set_turn_status() {
  if (is_in_check(white_turn)) {
    status_message = white_turn ? "White in check" : "Black in check";
  } else {
    status_message = white_turn ? "White to move" : "Black to move";
  }
}

void select_square(int row, int col) {
  selected_row = row;
  selected_col = col;
  status_message = white_turn ? "White selected" : "Black selected";
}

void move_piece(int from_row, int from_col, int to_row, int to_col) {
  board[to_row][to_col] = board[from_row][from_col];
  board[from_row][from_col] = '.';
  white_turn = !white_turn;
  clear_selection();

  const bool in_check = is_in_check(white_turn);
  const bool has_move = has_any_legal_move(white_turn);
  if (!has_move && in_check) {
    game_over = true;
    status_message = white_turn ? "Checkmate - Black wins" : "Checkmate - White wins";
  } else if (!has_move) {
    game_over = true;
    status_message = "Stalemate - Draw";
  } else {
    set_turn_status();
  }
}

void on_square_clicked(lv_event_t *event) {
  if (game_over) {
    return;
  }

  const auto id = *static_cast<uint8_t *>(lv_event_get_user_data(event));
  const int row = id / 8;
  const int col = id % 8;
  const char touched_piece = board[row][col];

  if (selected_row < 0) {
    if (is_current_turn_piece(touched_piece)) {
      select_square(row, col);
    } else {
      status_message = white_turn ? "White to move" : "Black to move";
    }
    repaint_board();
    return;
  }

  if (row == selected_row && col == selected_col) {
    clear_selection();
    set_turn_status();
    repaint_board();
    return;
  }

  if (is_current_turn_piece(touched_piece)) {
    select_square(row, col);
    repaint_board();
    return;
  }

  if (is_legal_move(selected_row, selected_col, row, col)) {
    move_piece(selected_row, selected_col, row, col);
  } else {
    status_message = "Illegal move";
  }

  repaint_board();
}

void create_chessboard() {
  Serial.println("Creating chessboard UI");
#ifdef BOARD_HAS_TOUCH
  touch_calibration_data.valid = false;
#endif

  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x2F4F3D), LV_PART_MAIN);

  board_frame = lv_obj_create(screen);
  lv_obj_remove_style_all(board_frame);
  lv_obj_set_size(board_frame, kBoardPixels, kBoardPixels);
  lv_obj_set_pos(board_frame, kBoardOffsetX, kBoardOffsetY);
  lv_obj_set_style_bg_color(board_frame, lv_color_hex(0xEEEED2), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(board_frame, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(board_frame, 0, LV_PART_MAIN);
  lv_obj_remove_flag(board_frame, LV_OBJ_FLAG_CLICKABLE);

  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      const int id = row * 8 + col;
      square_ids[id] = id;

      lv_obj_t *square = lv_obj_create(board_frame);
      squares[row][col] = square;
      lv_obj_remove_style_all(square);
      lv_obj_set_size(square, kSquarePixels, kSquarePixels);
      lv_obj_set_pos(square, col * kSquarePixels, row * kSquarePixels);
      lv_obj_set_style_radius(square, 0, LV_PART_MAIN);
      lv_obj_add_flag(square, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(square, on_square_clicked, LV_EVENT_CLICKED, &square_ids[id]);

      lv_obj_t *marker = lv_obj_create(square);
      move_markers[row][col] = marker;
      lv_obj_remove_style_all(marker);
      lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *label = lv_label_create(square);
      piece_labels[row][col] = label;
      lv_obj_set_style_text_font(label, &chess_symbols_42, LV_PART_MAIN);
      lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_center(label);

      if (col == 0) {
        lv_obj_t *rank = lv_label_create(square);
        rank_labels[row] = rank;
        lv_label_set_text_fmt(rank, "%d", 8 - row);
        lv_obj_set_style_text_font(rank, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(rank, LV_ALIGN_TOP_LEFT, 3, 1);
        lv_obj_remove_flag(rank, LV_OBJ_FLAG_CLICKABLE);
      }

      if (row == 7) {
        lv_obj_t *file = lv_label_create(square);
        file_labels[col] = file;
        lv_label_set_text_fmt(file, "%c", 'a' + col);
        lv_obj_set_style_text_font(file, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(file, LV_ALIGN_BOTTOM_RIGHT, -3, -1);
        lv_obj_remove_flag(file, LV_OBJ_FLAG_CLICKABLE);
      }
    }
  }

  status_label = lv_label_create(screen);
  lv_obj_set_width(status_label, kScreenPixels);
  lv_obj_set_style_bg_color(status_label, lv_color_hex(0x2F4F3D), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -7);
  lv_obj_remove_flag(status_label, LV_OBJ_FLAG_CLICKABLE);

  repaint_board();
  Serial.println("Chessboard UI created");
}
} // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-S3 local chess starting");

  smartdisplay_init();
  smartdisplay_lcd_set_backlight(1.0f);

  auto display = lv_display_get_default();
  lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);

  create_chessboard();
}

void loop() {
  static auto lv_last_tick = millis();
  const auto now = millis();

  lv_tick_inc(now - lv_last_tick);
  lv_last_tick = now;
  lv_timer_handler();

  delay(5);
}
