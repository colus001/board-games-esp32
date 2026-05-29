#include <Arduino.h>
#include <esp32_smartdisplay.h>
#include <lvgl.h>

namespace {
constexpr int kScreenPixels = 480;
constexpr int kStatusHeight = 36;
constexpr int kBoardPixels = kScreenPixels - kStatusHeight;
constexpr int kSquarePixels = kBoardPixels / 8;
constexpr int kBoardOffsetX = (kScreenPixels - kBoardPixels) / 2;
constexpr int kBoardOffsetY = 0;

lv_obj_t *squares[8][8];
lv_obj_t *piece_badges[8][8];
lv_obj_t *piece_labels[8][8];
lv_obj_t *status_label = nullptr;
uint8_t square_ids[64];

int selected_row = -1;
int selected_col = -1;
bool white_turn = true;
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
  static char text[2];
  if (piece == '.') {
    return "";
  }

  text[0] = piece;
  text[1] = '\0';
  return text;
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

bool is_legal_move(int from_row, int from_col, int to_row, int to_col) {
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
  const lv_color_t color = selected
                               ? lv_color_hex(0xFFE066)
                               : capture_destination
                                     ? lv_color_hex(0xFCA5A5)
                               : legal_destination
                                     ? lv_color_hex(0xBBF7D0)
                               : is_light_square(row, col)
                                     ? lv_color_hex(0xF3E7C9)
                                     : lv_color_hex(0xB88758);

  lv_obj_set_style_bg_color(squares[row][col], color, LV_PART_MAIN);
  lv_obj_set_style_border_width(squares[row][col], selected ? 5 : capture_destination ? 4 : legal_destination ? 3 : 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(squares[row][col],
                                selected          ? lv_color_hex(0x00D1FF)
                                : capture_destination ? lv_color_hex(0xE11D48)
                                : legal_destination   ? lv_color_hex(0x16A34A)
                                                      : lv_color_hex(0x000000),
                                LV_PART_MAIN);
  lv_obj_set_style_border_opa(squares[row][col], (selected || legal_destination) ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);

  const char piece = board[row][col];
  const bool has_piece = piece != '.';
  const bool white_piece = is_white_piece(piece);

  if (has_piece) {
    lv_obj_remove_flag(piece_badges[row][col], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(piece_badges[row][col],
                              selected ? lv_color_hex(0x0F766E) : white_piece ? lv_color_hex(0x1F2937) : lv_color_hex(0xFFF3D6),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(piece_badges[row][col],
                                  selected ? lv_color_hex(0xCCFBF1) : white_piece ? lv_color_hex(0xF8FAFC) : lv_color_hex(0x111827),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(piece_badges[row][col], selected ? 4 : 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(piece_badges[row][col], selected ? 16 : 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(piece_badges[row][col], selected ? LV_OPA_60 : LV_OPA_30, LV_PART_MAIN);
  } else {
    lv_obj_add_flag(piece_badges[row][col], LV_OBJ_FLAG_HIDDEN);
  }

  lv_label_set_text(piece_labels[row][col], piece_text(piece));
  lv_obj_set_style_text_color(piece_labels[row][col],
                              white_piece ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x050505),
                              LV_PART_MAIN);
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
  status_message = white_turn ? "White to move" : "Black to move";
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
  set_turn_status();
}

void on_square_clicked(lv_event_t *event) {
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
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x111827), LV_PART_MAIN);

  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      const int id = row * 8 + col;
      square_ids[id] = id;

      lv_obj_t *square = lv_obj_create(screen);
      squares[row][col] = square;
      lv_obj_remove_style_all(square);
      lv_obj_set_size(square, kSquarePixels, kSquarePixels);
      lv_obj_set_pos(square, kBoardOffsetX + col * kSquarePixels, kBoardOffsetY + row * kSquarePixels);
      lv_obj_add_flag(square, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(square, on_square_clicked, LV_EVENT_CLICKED, &square_ids[id]);

      lv_obj_t *badge = lv_obj_create(square);
      piece_badges[row][col] = badge;
      lv_obj_remove_style_all(badge);
      lv_obj_set_size(badge, 44, 44);
      lv_obj_center(badge);
      lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(badge, 2, LV_PART_MAIN);
      lv_obj_set_style_shadow_width(badge, 8, LV_PART_MAIN);
      lv_obj_set_style_shadow_opa(badge, LV_OPA_30, LV_PART_MAIN);
      lv_obj_set_style_shadow_color(badge, lv_color_hex(0x000000), LV_PART_MAIN);
      lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *label = lv_label_create(badge);
      piece_labels[row][col] = label;
      lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
      lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_center(label);
    }
  }

  status_label = lv_label_create(screen);
  lv_obj_set_width(status_label, kScreenPixels);
  lv_obj_set_style_bg_color(status_label, lv_color_hex(0x111827), LV_PART_MAIN);
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
