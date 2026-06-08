#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp32_smartdisplay.h>
#include <lvgl.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"

#ifndef PAIRING_BASE_URL
#define PAIRING_BASE_URL ""
#endif

LV_FONT_DECLARE(chess_symbols_42);

namespace {
constexpr int kScreenPixels = 480;
constexpr int kStatusHeight = 32;
constexpr int kBoardPixels = kScreenPixels - kStatusHeight;
constexpr int kSquarePixels = kBoardPixels / 8;
constexpr int kBoardOffsetX = (kScreenPixels - kBoardPixels) / 2;
constexpr int kBoardOffsetY = 0;
constexpr int kHudOffsetY = kBoardOffsetY + kBoardPixels;

lv_obj_t *board_frame = nullptr;
lv_obj_t *squares[8][8];
lv_obj_t *move_markers[8][8];
lv_obj_t *piece_labels[8][8];
lv_obj_t *rank_labels[8];
lv_obj_t *file_labels[8];
lv_obj_t *status_label = nullptr;
lv_obj_t *resign_overlay = nullptr;
lv_obj_t *network_body_label = nullptr;
lv_obj_t *network_detail_label = nullptr;
lv_obj_t *lichess_progress_stage_label = nullptr;
lv_obj_t *lichess_progress_detail_label = nullptr;
lv_obj_t *lichess_progress_log_label = nullptr;
uint8_t square_ids[64];

enum class GameMode { Local, Ai, Lichess };
enum class AiDifficulty { Easy, Normal, Hard };

struct ChessMove {
  int from_row = -1;
  int from_col = -1;
  int to_row = -1;
  int to_col = -1;
  int score = -32000;
};

void show_start_screen();
void create_chessboard();
void show_game_over_overlay();
void begin_lichess_match();
void start_local_game();
void start_ai_game();
void show_ai_difficulty_screen();
void clear_selection();
void repaint_board();
void show_wifi_scan_screen();
void show_wifi_scan_results_screen();
void show_wifi_password_screen();
void show_lichess_pair_screen();
void show_lichess_progress_screen();
void update_lichess_progress(const char *stage, const char *detail);

int selected_row = -1;
int selected_col = -1;
int last_from_row = -1;
int last_from_col = -1;
int last_to_row = -1;
int last_to_col = -1;
bool white_turn = true;
bool game_over = false;
GameMode game_mode = GameMode::Local;
bool lichess_match_pending = false;
uint32_t lichess_match_start_at = 0;
const char *status_message = "White to move";
const char *game_over_title = "Game Over";
const char *game_over_subtitle = "";
String game_over_subtitle_storage;

WiFiClientSecure lichess_stream_client;
Preferences preferences;
String configured_wifi_ssid;
String configured_wifi_password;
String configured_lichess_token;
String scanned_ssids[20];
int32_t scanned_rssi[20];
int scanned_network_count = 0;
int wifi_page = 0;
uint8_t wifi_button_ids[6];
String selected_wifi_ssid;
lv_obj_t *wifi_password_textarea = nullptr;
lv_obj_t *pair_status_label = nullptr;
lv_timer_t *pair_poll_timer = nullptr;
String pair_device_id;
String lichess_username;
String lichess_game_id;
String lichess_last_moves;
String lichess_line_buffer;
String lichess_stream_error_detail;
String lichess_status_text;
String lichess_progress_log_text;
String lichess_move_error_detail;
String lichess_pending_uci;
uint32_t lichess_pending_since = 0;
uint32_t lichess_next_reconnect_at = 0;
int lichess_last_move_http_code = 0;
bool lichess_stream_headers_done = false;
bool lichess_is_white = true;
bool lichess_my_turn = false;
AiDifficulty ai_difficulty = AiDifficulty::Normal;
int ai_search_depth = 2;
int ai_min_think_ms = 800;
uint32_t ai_search_nodes = 0;

enum class SeekStatus {
  Matched,
  EventStreamFailed,
  SeekBeginFailed,
  SeekRejected,
  SeekUnauthorized,
  SeekForbidden,
  SeekHttpError,
  SeekStreamClosed,
  EventStreamClosed,
  Timeout,
};

struct SeekResult {
  SeekStatus status = SeekStatus::Timeout;
  String game_id;
  String detail;
  int http_code = 0;
};

constexpr int kMaxAiMoves = 128;
constexpr int kMaxAiSearchDepth = 3;
ChessMove ai_move_buffers[kMaxAiSearchDepth + 2][kMaxAiMoves];

const char kInitialBoard[8][8] = {
    {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'},
    {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'},
    {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'},
};

char board[8][8];

void load_config() {
  preferences.begin("localchess", true);
  configured_wifi_ssid = preferences.getString("wifi_ssid", WIFI_SSID);
  configured_wifi_password = preferences.getString("wifi_pass", WIFI_PASSWORD);
  configured_lichess_token = preferences.getString("lichess", LICHESS_TOKEN);
  preferences.end();
}

void save_config(const String &ssid, const String &password, const String &token) {
  preferences.begin("localchess", false);
  preferences.putString("wifi_ssid", ssid);
  preferences.putString("wifi_pass", password);
  preferences.putString("lichess", token);
  preferences.end();
  configured_wifi_ssid = ssid;
  configured_wifi_password = password;
  configured_lichess_token = token;
}

void reset_board_position() {
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      board[row][col] = kInitialBoard[row][col];
    }
  }
}

void reset_game() {
  reset_board_position();
  selected_row = -1;
  selected_col = -1;
  last_from_row = -1;
  last_from_col = -1;
  last_to_row = -1;
  last_to_col = -1;
  white_turn = true;
  game_over = false;
  game_mode = GameMode::Local;
  status_message = "White to move";
  game_over_title = "Game Over";
  game_over_subtitle = "";
  game_over_subtitle_storage = "";
  lichess_stream_client.stop();
  lichess_game_id = "";
  lichess_last_moves = "";
  lichess_line_buffer = "";
  lichess_status_text = "";
  lichess_move_error_detail = "";
  lichess_pending_uci = "";
  lichess_pending_since = 0;
  lichess_next_reconnect_at = 0;
  lichess_last_move_http_code = 0;
  lichess_stream_headers_done = false;
  lichess_is_white = true;
  lichess_my_turn = false;
}

void start_new_game() {
  const GameMode previous_mode = game_mode;
  if (previous_mode == GameMode::Lichess) {
    begin_lichess_match();
  } else if (previous_mode == GameMode::Ai) {
    start_ai_game();
  } else {
    start_local_game();
  }
}

void start_local_game() {
  reset_game();
  game_mode = GameMode::Local;
  create_chessboard();
}

const char *ai_thinking_text() {
  switch (ai_difficulty) {
  case AiDifficulty::Easy:
    return "AI thinking... Easy";
  case AiDifficulty::Normal:
    return "AI thinking... Normal";
  case AiDifficulty::Hard:
    return "AI thinking... Hard";
  }
  return "AI thinking...";
}

void set_ai_difficulty(AiDifficulty difficulty) {
  ai_difficulty = difficulty;
  switch (difficulty) {
  case AiDifficulty::Easy:
    ai_search_depth = 1;
    ai_min_think_ms = 300;
    break;
  case AiDifficulty::Normal:
    ai_search_depth = 2;
    ai_min_think_ms = 800;
    break;
  case AiDifficulty::Hard:
    ai_search_depth = 3;
    ai_min_think_ms = 1500;
    break;
  }
}

void start_ai_game() {
  reset_game();
  game_mode = GameMode::Ai;
  switch (ai_difficulty) {
  case AiDifficulty::Easy:
    status_message = "White vs AI Easy";
    break;
  case AiDifficulty::Normal:
    status_message = "White vs AI Normal";
    break;
  case AiDifficulty::Hard:
    status_message = "White vs AI Hard";
    break;
  }
  create_chessboard();
}

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

String lower_string(String value) {
  value.toLowerCase();
  return value;
}

String json_string_value(const String &json, const char *key) {
  String needle = String("\"") + key + "\":\"";
  int start = json.indexOf(needle);
  if (start < 0) {
    return "";
  }
  start += needle.length();
  const int end = json.indexOf('"', start);
  if (end < 0) {
    return "";
  }
  return json.substring(start, end);
}

bool json_has_string_key(const String &json, const char *key) {
  return json.indexOf(String("\"") + key + "\":\"") >= 0;
}

bool json_has_status(const String &json, const char *status) {
  return json.indexOf(String("\"status\":\"") + status + "\"") >= 0;
}

void set_dynamic_status(const String &message) {
  lichess_status_text = message;
  status_message = lichess_status_text.c_str();
}

void set_lichess_turn_status() {
  const char *side = lichess_is_white ? "White" : "Black";
  const char *turn = white_turn ? "White" : "Black";
  if (lichess_my_turn) {
    set_dynamic_status(String("You: ") + side + " | Your move");
  } else {
    set_dynamic_status(String("You: ") + side + " | " + turn + " to move");
  }
}

String compact_error_detail(const String &body) {
  String detail = json_string_value(body, "error");
  if (detail.length() == 0) {
    detail = json_string_value(body, "message");
  }
  if (detail.length() == 0) {
    detail = body;
  }
  detail.replace("\n", " ");
  detail.replace("\r", " ");
  detail.trim();
  if (detail.length() > 90) {
    detail = detail.substring(0, 90);
  }
  return detail;
}

bool wifi_configured() {
  return configured_wifi_ssid.length() > 0;
}

bool lichess_token_configured() {
  return configured_lichess_token.length() > 0;
}

bool board_flipped() {
  return game_mode == GameMode::Lichess && !lichess_is_white;
}

int board_to_display_row(int row) {
  return board_flipped() ? 7 - row : row;
}

int board_to_display_col(int col) {
  return board_flipped() ? 7 - col : col;
}

const char *configured_text(bool configured) {
  return configured ? "configured" : "missing";
}

int file_to_col(char file) {
  return file - 'a';
}

int rank_to_row(char rank) {
  return '8' - rank;
}

void clear_last_move() {
  last_from_row = -1;
  last_from_col = -1;
  last_to_row = -1;
  last_to_col = -1;
}

void set_last_move(int from_row, int from_col, int to_row, int to_col) {
  last_from_row = from_row;
  last_from_col = from_col;
  last_to_row = to_row;
  last_to_col = to_col;
}

void set_last_move_from_uci(const String &move) {
  if (move.length() < 4) {
    clear_last_move();
    return;
  }

  const int from_col = file_to_col(move[0]);
  const int from_row = rank_to_row(move[1]);
  const int to_col = file_to_col(move[2]);
  const int to_row = rank_to_row(move[3]);
  if (from_row < 0 || from_row > 7 || from_col < 0 || from_col > 7 || to_row < 0 || to_row > 7 || to_col < 0 || to_col > 7) {
    clear_last_move();
    return;
  }

  set_last_move(from_row, from_col, to_row, to_col);
}

bool is_last_move_from_square(int row, int col) {
  return row == last_from_row && col == last_from_col;
}

bool is_last_move_to_square(int row, int col) {
  return row == last_to_row && col == last_to_col;
}

String uci_from_move(int from_row, int from_col, int to_row, int to_col) {
  String move;
  move += char('a' + from_col);
  move += char('8' - from_row);
  move += char('a' + to_col);
  move += char('8' - to_row);
  const char piece = board[from_row][from_col];
  if (lower_piece(piece) == 'p' && (to_row == 0 || to_row == 7)) {
    move += 'q';
  }
  return move;
}

void apply_uci_move(const String &move) {
  if (move.length() < 4) {
    return;
  }

  const int from_col = file_to_col(move[0]);
  const int from_row = rank_to_row(move[1]);
  const int to_col = file_to_col(move[2]);
  const int to_row = rank_to_row(move[3]);
  if (from_row < 0 || from_row > 7 || from_col < 0 || from_col > 7 || to_row < 0 || to_row > 7 || to_col < 0 || to_col > 7) {
    return;
  }

  char piece = board[from_row][from_col];
  if (piece == '.') {
    return;
  }

  if (lower_piece(piece) == 'p' && from_col != to_col && board[to_row][to_col] == '.') {
    board[from_row][to_col] = '.';
  }

  if (lower_piece(piece) == 'k' && abs_int(to_col - from_col) == 2) {
    if (to_col == 6) {
      board[from_row][5] = board[from_row][7];
      board[from_row][7] = '.';
    } else if (to_col == 2) {
      board[from_row][3] = board[from_row][0];
      board[from_row][0] = '.';
    }
  }

  if (move.length() >= 5 && lower_piece(piece) == 'p') {
    const char promotion = lower_piece(move[4]);
    piece = is_white_piece(piece) ? promotion - 'a' + 'A' : promotion;
  }

  board[to_row][to_col] = piece;
  board[from_row][from_col] = '.';
}

void apply_lichess_moves(const String &moves) {
  reset_board_position();
  int move_count = 0;
  int start = 0;
  String last_move;

  while (start < moves.length()) {
    while (start < moves.length() && moves[start] == ' ') {
      ++start;
    }
    if (start >= moves.length()) {
      break;
    }
    int end = moves.indexOf(' ', start);
    if (end < 0) {
      end = moves.length();
    }
    last_move = moves.substring(start, end);
    apply_uci_move(last_move);
    ++move_count;
    start = end + 1;
  }

  clear_selection();
  if (move_count > 0) {
    set_last_move_from_uci(last_move);
  } else {
    clear_last_move();
  }
  white_turn = (move_count % 2) == 0;
  lichess_my_turn = white_turn == lichess_is_white;
  set_lichess_turn_status();
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

int piece_value(char piece) {
  switch (lower_piece(piece)) {
  case 'p':
    return 100;
  case 'n':
    return 320;
  case 'b':
    return 330;
  case 'r':
    return 500;
  case 'q':
    return 900;
  case 'k':
    return 20000;
  default:
    return 0;
  }
}

int center_bonus(int row, int col) {
  const int row_dist = abs_int(3 - row) < abs_int(4 - row) ? abs_int(3 - row) : abs_int(4 - row);
  const int col_dist = abs_int(3 - col) < abs_int(4 - col) ? abs_int(3 - col) : abs_int(4 - col);
  return 14 - (row_dist + col_dist) * 3;
}

int evaluate_position_for(bool white) {
  int score = 0;
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      const char piece = board[row][col];
      if (piece == '.') {
        continue;
      }
      int value = piece_value(piece);
      if (lower_piece(piece) != 'k') {
        value += center_bonus(row, col);
      }
      score += is_white_piece(piece) == white ? value : -value;
    }
  }
  return score;
}

char apply_temp_move(const ChessMove &move) {
  const char captured = board[move.to_row][move.to_col];
  board[move.to_row][move.to_col] = board[move.from_row][move.from_col];
  board[move.from_row][move.from_col] = '.';
  return captured;
}

void undo_temp_move(const ChessMove &move, char captured) {
  board[move.from_row][move.from_col] = board[move.to_row][move.to_col];
  board[move.to_row][move.to_col] = captured;
}

int generate_legal_moves(bool white, ChessMove moves[], int max_moves) {
  int count = 0;
  for (int from_row = 0; from_row < 8; ++from_row) {
    for (int from_col = 0; from_col < 8; ++from_col) {
      const char piece = board[from_row][from_col];
      if (piece == '.' || is_white_piece(piece) != white) {
        continue;
      }
      for (int to_row = 0; to_row < 8; ++to_row) {
        for (int to_col = 0; to_col < 8; ++to_col) {
          if (!is_legal_move(from_row, from_col, to_row, to_col)) {
            continue;
          }
          moves[count].from_row = from_row;
          moves[count].from_col = from_col;
          moves[count].to_row = to_row;
          moves[count].to_col = to_col;
          moves[count].score = piece_value(board[to_row][to_col]);
          ++count;
          if (count >= max_moves) {
            return count;
          }
        }
      }
    }
  }
  return count;
}

int micromax_search(bool white, int depth, int alpha, int beta, int ply) {
  ++ai_search_nodes;
  if ((ai_search_nodes & 0x1FF) == 0) {
    lv_timer_handler();
    delay(0);
  }

  if (depth == 0) {
    return evaluate_position_for(white);
  }

  ChessMove *moves = ai_move_buffers[ply];
  const int count = generate_legal_moves(white, moves, kMaxAiMoves);
  if (count == 0) {
    return is_in_check(white) ? -30000 - depth : 0;
  }

  for (int i = 0; i < count; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (moves[j].score > moves[i].score) {
        const ChessMove tmp = moves[i];
        moves[i] = moves[j];
        moves[j] = tmp;
      }
    }

    const char captured = apply_temp_move(moves[i]);
    const int score = -micromax_search(!white, depth - 1, -beta, -alpha, ply + 1);
    undo_temp_move(moves[i], captured);
    if (score > alpha) {
      alpha = score;
    }
    if (alpha >= beta) {
      break;
    }
  }
  return alpha;
}

ChessMove choose_ai_move(bool white) {
  ChessMove best;
  ai_search_nodes = 0;
  ChessMove *moves = ai_move_buffers[0];
  const int count = generate_legal_moves(white, moves, kMaxAiMoves);
  for (int i = 0; i < count; ++i) {
    const char captured = apply_temp_move(moves[i]);
    int score = -micromax_search(!white, ai_search_depth, -32000, 32000, 1);
    undo_temp_move(moves[i], captured);
    if (ai_difficulty == AiDifficulty::Easy) {
      score += random(-80, 81);
    }
    if (score > best.score) {
      best = moves[i];
      best.score = score;
    }
  }
  return best;
}

bool connect_wifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(configured_wifi_ssid.c_str(), configured_wifi_password.c_str());
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    lv_timer_handler();
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

void stop_pair_poll_timer() {
  if (!pair_poll_timer) {
    return;
  }
  lv_timer_delete(pair_poll_timer);
  pair_poll_timer = nullptr;
}

bool start_pairing_session(String &device_id, String &pair_code, String &pair_url) {
  if (String(PAIRING_BASE_URL).length() == 0) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(PAIRING_BASE_URL) + "/api/pair/start")) {
    return false;
  }
  http.addHeader("Accept", "application/json");
  const int code = http.POST("");
  const String body = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    return false;
  }

  device_id = json_string_value(body, "deviceId");
  pair_code = json_string_value(body, "pairCode");
  pair_url = json_string_value(body, "pairUrl");
  return device_id.length() > 0 && pair_code.length() > 0 && pair_url.length() > 0;
}

String pairing_status_body() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(PAIRING_BASE_URL) + "/api/pair/status?deviceId=" + pair_device_id)) {
    return "";
  }
  http.addHeader("Accept", "application/json");
  const int code = http.GET();
  const String body = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    return "";
  }
  return body;
}

void update_network_status(const char *message, const char *detail) {
  if (network_body_label) {
    lv_label_set_text(network_body_label, message);
  }
  if (network_detail_label) {
    lv_label_set_text(network_detail_label, detail);
  }
  if (lichess_progress_stage_label) {
    lv_label_set_text(lichess_progress_stage_label, message);
  }
  if (lichess_progress_detail_label) {
    lv_label_set_text(lichess_progress_detail_label, detail);
  }
  lv_timer_handler();
}

void lichess_log(const String &message) {
  Serial.println(message);
  if (lichess_progress_log_text.length() > 0) {
    lichess_progress_log_text += "\n";
  }
  lichess_progress_log_text += message;

  int line_count = 0;
  for (int i = lichess_progress_log_text.length() - 1; i >= 0; --i) {
    if (lichess_progress_log_text[i] == '\n') {
      ++line_count;
      if (line_count >= 7) {
        lichess_progress_log_text = lichess_progress_log_text.substring(i + 1);
        break;
      }
    }
  }

  if (lichess_progress_log_label) {
    lv_label_set_text(lichess_progress_log_label, lichess_progress_log_text.c_str());
  }
  lv_timer_handler();
}

void update_lichess_progress(const char *stage, const char *detail) {
  update_network_status(stage, detail);
  lichess_log(String(stage) + " - " + detail);
}

void pump_ui(uint32_t milliseconds) {
  const uint32_t end_at = millis() + milliseconds;
  while (millis() < end_at) {
    lv_timer_handler();
    delay(5);
  }
}

bool load_lichess_account() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, "https://lichess.org/api/account")) {
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + configured_lichess_token);
  http.addHeader("Accept", "application/json");
  const int code = http.GET();
  const String body = http.getString();
  http.end();

  Serial.printf("Lichess account HTTP code: %d\n", code);

  if (code != 200) {
    return false;
  }

  lichess_username = lower_string(json_string_value(body, "username"));
  return lichess_username.length() > 0;
}

const char *seek_status_text(const SeekResult &result) {
  switch (result.status) {
  case SeekStatus::EventStreamFailed:
    return "Event stream failed";
  case SeekStatus::SeekBeginFailed:
    return "Could not start seek";
  case SeekStatus::SeekRejected:
    return "Seek rejected by Lichess";
  case SeekStatus::SeekUnauthorized:
    return "Lichess token unauthorized";
  case SeekStatus::SeekForbidden:
    return "Token missing board:play";
  case SeekStatus::SeekHttpError:
    return "Lichess seek error";
  case SeekStatus::SeekStreamClosed:
    return "Seek stream closed";
  case SeekStatus::EventStreamClosed:
    return "Event stream closed";
  case SeekStatus::Timeout:
    return "No match after 90 sec";
  case SeekStatus::Matched:
    return "Matched";
  }
  return "Match failed";
}

bool read_http_line(WiFiClientSecure &client, String &line, unsigned long timeout_ms) {
  line = "";
  const unsigned long started = millis();
  while (millis() - started < timeout_ms) {
    while (client.available()) {
      const char c = client.read();
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        return true;
      }
      if (line.length() < 300) {
        line += c;
      }
    }
    lv_timer_handler();
    delay(5);
  }
  return false;
}

int parse_http_status_code(const String &status_line) {
  const int first_space = status_line.indexOf(' ');
  if (first_space < 0 || first_space + 4 > status_line.length()) {
    return 0;
  }
  return status_line.substring(first_space + 1, first_space + 4).toInt();
}

SeekResult seek_lichess_game() {
  SeekResult result;
  update_network_status("Opening event stream...", "Waiting for Lichess events");
  WiFiClientSecure event_client;
  event_client.setInsecure();
  event_client.setTimeout(5);
  if (!event_client.connect("lichess.org", 443)) {
    Serial.println("Lichess event stream connect failed");
    result.status = SeekStatus::EventStreamFailed;
    return result;
  }
  Serial.println("Lichess event stream connected");

  event_client.print("GET /api/stream/event HTTP/1.1\r\n");
  event_client.print("Host: lichess.org\r\n");
  event_client.print(String("Authorization: Bearer ") + configured_lichess_token + "\r\n");
  event_client.print("Accept: application/x-ndjson\r\n");
  event_client.print("Connection: keep-alive\r\n\r\n");

  update_network_status("Creating seek...", "Casual 10+0 / random color");
  WiFiClientSecure seek_client;
  seek_client.setInsecure();
  seek_client.setTimeout(5);
  if (!seek_client.connect("lichess.org", 443)) {
    event_client.stop();
    Serial.println("Lichess seek stream connect failed");
    result.status = SeekStatus::SeekBeginFailed;
    return result;
  }

  const String seek_body = "rated=false&time=10&increment=0&color=random";
  seek_client.print("POST /api/board/seek HTTP/1.1\r\n");
  seek_client.print("Host: lichess.org\r\n");
  seek_client.print(String("Authorization: Bearer ") + configured_lichess_token + "\r\n");
  seek_client.print("Accept: application/x-ndjson\r\n");
  seek_client.print("Content-Type: application/x-www-form-urlencoded\r\n");
  seek_client.print(String("Content-Length: ") + seek_body.length() + "\r\n");
  seek_client.print("Connection: keep-alive\r\n\r\n");
  seek_client.print(seek_body);

  String status_line;
  if (!read_http_line(seek_client, status_line, 15000)) {
    event_client.stop();
    seek_client.stop();
    Serial.println("Lichess seek status timeout");
    result.status = SeekStatus::SeekHttpError;
    result.detail = "No seek HTTP response";
    return result;
  }

  const int code = parse_http_status_code(status_line);
  result.http_code = code;
  Serial.printf("Lichess seek status: %s\n", status_line.c_str());
  Serial.printf("Lichess seek HTTP code: %d\n", code);

  String header_line;
  while (read_http_line(seek_client, header_line, 5000)) {
    if (header_line.length() == 0) {
      break;
    }
  }

  if (code < 200 || code >= 300) {
    String error_body;
    const unsigned long error_started = millis();
    while (millis() - error_started < 2000) {
      while (seek_client.available()) {
        const char c = seek_client.read();
        if (error_body.length() < 300) {
          error_body += c;
        }
      }
      if (!seek_client.connected()) {
        break;
      }
      delay(10);
    }
    result.detail = compact_error_detail(error_body);
    Serial.printf("Lichess seek error body: %.220s\n", error_body.c_str());
  }
  if (code < 200 || code >= 300) {
    event_client.stop();
    seek_client.stop();
    if (code == 400) {
      result.status = SeekStatus::SeekRejected;
    } else if (code == 401) {
      result.status = SeekStatus::SeekUnauthorized;
    } else if (code == 403) {
      result.status = SeekStatus::SeekForbidden;
    } else {
      result.status = SeekStatus::SeekHttpError;
    }
    return result;
  }

  update_network_status("Waiting for opponent...", "This can take up to 90 seconds");
  bool headers_done = false;
  bool seek_closed = false;
  String line;
  const unsigned long started = millis();
  unsigned long last_update = 0;
  unsigned long seek_closed_at = 0;
  while (millis() - started < 90000) {
    if (!event_client.connected()) {
      event_client.stop();
      seek_client.stop();
      Serial.println("Lichess event stream closed before match");
      result.status = SeekStatus::EventStreamClosed;
      return result;
    }

    if (!seek_closed) {
      while (seek_client.available()) {
        seek_client.read();
      }
      if (!seek_client.connected()) {
        seek_closed = true;
        seek_closed_at = millis();
        Serial.println("Lichess seek stream closed before gameStart event");
        update_network_status("Waiting for opponent...", "Seek closed; checking events...");
      }
    } else if (millis() - seek_closed_at > 5000) {
      event_client.stop();
      seek_client.stop();
      result.status = SeekStatus::SeekStreamClosed;
      result.detail = "Seek expired or was canceled";
      return result;
    }

    const unsigned long elapsed = (millis() - started) / 1000;
    if (millis() - last_update > 5000) {
      last_update = millis();
      String detail = String("Still waiting... ") + String(elapsed) + "s / 90s";
      update_network_status("Waiting for opponent...", detail.c_str());
    }

    while (event_client.available()) {
      const char c = event_client.read();
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        if (!headers_done) {
          headers_done = line.length() == 0;
        } else if (line.length() > 0) {
          Serial.printf("Lichess event: %.220s\n", line.c_str());
          if (line.indexOf("gameStart") >= 0) {
            const String game_id = json_string_value(line, "gameId");
            if (game_id.length() > 0) {
              const String color = json_string_value(line, "color");
              if (color == "white") {
                lichess_is_white = true;
              } else if (color == "black") {
                lichess_is_white = false;
              }
              event_client.stop();
              seek_client.stop();
              Serial.printf("Lichess matched game id: %s\n", game_id.c_str());
              result.status = SeekStatus::Matched;
              result.game_id = game_id;
              return result;
            }
          }
        }
        line = "";
      } else if (line.length() < 1200) {
        line += c;
      }
    }

    lv_timer_handler();
    delay(20);
  }

  event_client.stop();
  seek_client.stop();
  Serial.println("Lichess seek timed out after 90 seconds");
  result.status = SeekStatus::Timeout;
  return result;
}

bool lichess_post(const String &path) {
  lichess_move_error_detail = "";
  lichess_last_move_http_code = 0;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String("https://lichess.org") + path)) {
    lichess_move_error_detail = "HTTP begin failed";
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + configured_lichess_token);
  const int code = http.POST("");
  const String body = http.getString();
  http.end();
  lichess_last_move_http_code = code;
  Serial.printf("Lichess POST %s -> HTTP %d\n", path.c_str(), code);
  if (body.length() > 0) {
    Serial.printf("Lichess POST body: %.220s\n", body.c_str());
  }
  if (code < 200 || code >= 300) {
    lichess_move_error_detail = String("HTTP ") + String(code);
    const String compact = compact_error_detail(body);
    if (compact.length() > 0) {
      lichess_move_error_detail += String(": ") + compact;
    }
  }
  return code >= 200 && code < 300;
}

bool connect_lichess_stream() {
  lichess_stream_error_detail = "";
  lichess_stream_client.stop();
  lichess_stream_client.setInsecure();
  lichess_stream_client.setTimeout(5);
  if (!lichess_stream_client.connect("lichess.org", 443)) {
    Serial.println("Lichess game stream TCP connect failed");
    lichess_stream_error_detail = "TCP connect failed";
    return false;
  }

  lichess_stream_client.print(String("GET /api/board/game/stream/") + lichess_game_id + " HTTP/1.1\r\n");
  lichess_stream_client.print("Host: lichess.org\r\n");
  lichess_stream_client.print(String("Authorization: Bearer ") + configured_lichess_token + "\r\n");
  lichess_stream_client.print("Accept: application/x-ndjson\r\n");
  lichess_stream_client.print("Connection: keep-alive\r\n\r\n");

  String status_line;
  if (!read_http_line(lichess_stream_client, status_line, 15000)) {
    Serial.println("Lichess game stream status timeout");
    lichess_stream_error_detail = "HTTP status timeout";
    lichess_stream_client.stop();
    return false;
  }

  const int status_code = parse_http_status_code(status_line);
  Serial.printf("Lichess game stream status: %s\n", status_line.c_str());
  if (status_code != 200) {
    String header_line;
    while (read_http_line(lichess_stream_client, header_line, 1000)) {
      if (header_line.length() == 0) {
        break;
      }
    }
    String error_body;
    const unsigned long started = millis();
    while (millis() - started < 1000) {
      while (lichess_stream_client.available()) {
        const char c = lichess_stream_client.read();
        if (error_body.length() < 220) {
          error_body += c;
        }
      }
      delay(10);
    }
    Serial.printf("Lichess game stream error body: %.220s\n", error_body.c_str());
    lichess_stream_error_detail = String("HTTP ") + String(status_code);
    const String compact = compact_error_detail(error_body);
    if (compact.length() > 0) {
      lichess_stream_error_detail += String(": ") + compact;
    }
    lichess_stream_client.stop();
    return false;
  }

  String header_line;
  while (read_http_line(lichess_stream_client, header_line, 5000)) {
    if (header_line.length() == 0) {
      break;
    }
  }

  lichess_stream_headers_done = true;
  lichess_line_buffer = "";
  lichess_next_reconnect_at = 0;
  return true;
}

void update_lichess_side_from_game(const String &line) {
  const String lower_line = lower_string(line);
  const int white_pos = lower_line.indexOf("\"white\"");
  const int black_pos = lower_line.indexOf("\"black\"");
  const String id_needle = String("\"id\":\"") + lichess_username + "\"";
  const String name_needle = String("\"name\":\"") + lichess_username + "\"";

  if (white_pos >= 0 && (black_pos < 0 || white_pos < black_pos)) {
    const String white_section = lower_line.substring(white_pos, black_pos < 0 ? lower_line.length() : black_pos);
    if (white_section.indexOf(id_needle) >= 0 || white_section.indexOf(name_needle) >= 0) {
      lichess_is_white = true;
    }
  }
  if (black_pos >= 0) {
    const String black_section = lower_line.substring(black_pos);
    if (black_section.indexOf(id_needle) >= 0 || black_section.indexOf(name_needle) >= 0) {
      lichess_is_white = false;
    }
  }
}

void handle_lichess_stream_line(const String &line) {
  if (line.length() == 0) {
    return;
  }

  Serial.printf("Lichess game stream: %.220s\n", line.c_str());

  const String event_type = json_string_value(line, "type");
  const bool game_full = event_type == "gameFull";
  const bool game_state = event_type == "gameState";
  const bool opponent_gone = event_type == "opponentGone";

  if (opponent_gone) {
    set_dynamic_status("Opponent disconnected");
    repaint_board();
    return;
  }

  if (game_full) {
    const bool was_flipped = board_flipped();
    update_lichess_side_from_game(line);
    if (board_flipped() != was_flipped) {
      create_chessboard();
    }
    set_dynamic_status(String("Game connected | You: ") + (lichess_is_white ? "White" : "Black"));
  }

  const bool has_moves = json_has_string_key(line, "moves");
  String moves = has_moves ? json_string_value(line, "moves") : "";
  if (has_moves && (game_full || game_state || moves != lichess_last_moves)) {
    Serial.printf("Applying Lichess moves: %.220s\n", moves.c_str());
    lichess_last_moves = moves;
    apply_lichess_moves(moves);
    if (lichess_pending_uci.length() > 0 && (String(" ") + moves + " ").indexOf(String(" ") + lichess_pending_uci + " ") >= 0) {
      Serial.printf("Lichess move synced: %s\n", lichess_pending_uci.c_str());
      lichess_pending_uci = "";
      lichess_pending_since = 0;
    }
    repaint_board();
  } else if (!has_moves) {
    Serial.println("Lichess stream line has no moves; board unchanged");
  }

  const String status = json_string_value(line, "status");
  if (status == "started" || status.length() == 0) {
    return;
  }

  if (status == "mate" || status == "resign" || status == "timeout" || status == "draw" || status == "stalemate" || status == "aborted") {
    game_over = true;
    game_over_title = status == "draw" || status == "stalemate" ? "Game drawn" : "Game over";
    if (status == "mate") {
      game_over_subtitle = "Checkmate";
    } else if (status == "resign") {
      game_over_subtitle = "Resignation";
    } else if (status == "timeout") {
      game_over_subtitle = "Timeout";
    } else if (status == "stalemate") {
      game_over_subtitle = "Stalemate";
    } else if (status == "aborted") {
      if (lichess_pending_uci.length() > 0) {
        game_over_subtitle_storage = String("Aborted before sync: ") + lichess_pending_uci;
        game_over_subtitle = game_over_subtitle_storage.c_str();
      } else {
        game_over_subtitle = "Aborted";
      }
    } else {
      game_over_subtitle = "Draw";
    }
    set_dynamic_status(String("Lichess game over: ") + status);
    repaint_board();
    show_game_over_overlay();
  }
}

void poll_lichess_stream() {
  if (game_mode != GameMode::Lichess || lichess_game_id.length() == 0 || game_over) {
    return;
  }
  if (!lichess_stream_client.connected()) {
    const uint32_t now = millis();
    if (lichess_next_reconnect_at != 0 && now < lichess_next_reconnect_at) {
      return;
    }
    lichess_next_reconnect_at = now + 5000;
    set_dynamic_status("Stream reconnecting...");
    repaint_board();
    if (!connect_lichess_stream()) {
      if (lichess_stream_error_detail.length() > 0) {
        set_dynamic_status(lichess_stream_error_detail);
      } else {
        set_dynamic_status("Stream reconnect failed");
      }
      repaint_board();
    } else {
      set_dynamic_status("Stream reconnected");
      repaint_board();
    }
    return;
  }

  if (lichess_pending_uci.length() > 0 && millis() - lichess_pending_since > 8000) {
    set_dynamic_status(String("Still waiting sync: ") + lichess_pending_uci);
    repaint_board();
    lichess_pending_since = millis();
  }

  while (lichess_stream_client.available()) {
    const char c = lichess_stream_client.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (!lichess_stream_headers_done) {
        if (lichess_line_buffer.length() == 0) {
          lichess_stream_headers_done = true;
        }
      } else {
        handle_lichess_stream_line(lichess_line_buffer);
      }
      lichess_line_buffer = "";
    } else if (lichess_line_buffer.length() < 1200) {
      lichess_line_buffer += c;
    }
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
  const bool last_move_from = is_last_move_from_square(row, col);
  const bool last_move_to = is_last_move_to_square(row, col);
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
  if (!selected && (last_move_from || last_move_to)) {
    lv_obj_set_style_border_width(squares[row][col], last_move_to ? 4 : 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(squares[row][col], lv_color_hex(0xE6C85C), LV_PART_MAIN);
    lv_obj_set_style_border_opa(squares[row][col], last_move_to ? LV_OPA_90 : LV_OPA_70, LV_PART_MAIN);
  } else {
    lv_obj_set_style_border_width(squares[row][col], 0, LV_PART_MAIN);
  }

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

  if (board_to_display_col(col) == 0) {
    lv_obj_set_style_text_color(rank_labels[row],
                                light_square ? lv_color_hex(0x4F7EA5) : lv_color_hex(0xEEEED2),
                                LV_PART_MAIN);
  }
  if (board_to_display_row(row) == 7) {
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
  set_last_move(from_row, from_col, to_row, to_col);
  white_turn = !white_turn;
  clear_selection();

  const bool in_check = is_in_check(white_turn);
  const bool has_move = has_any_legal_move(white_turn);
  if (!has_move && in_check) {
    game_over = true;
    game_over_title = "Checkmate";
    game_over_subtitle = white_turn ? "Black wins" : "White wins";
    status_message = white_turn ? "Checkmate - Black wins" : "Checkmate - White wins";
  } else if (!has_move) {
    game_over = true;
    game_over_title = "Stalemate";
    game_over_subtitle = "Draw";
    status_message = "Stalemate - Draw";
  } else {
    set_turn_status();
  }
}

void make_ai_move_if_needed() {
  if (game_mode != GameMode::Ai || game_over || white_turn) {
    return;
  }

  status_message = ai_thinking_text();
  repaint_board();
  lv_timer_handler();

  const unsigned long started = millis();
  const ChessMove ai_move = choose_ai_move(false);
  while (millis() - started < static_cast<unsigned long>(ai_min_think_ms)) {
    lv_timer_handler();
    delay(20);
  }
  if (ai_move.from_row < 0) {
    game_over = true;
    game_over_title = is_in_check(false) ? "Checkmate" : "Stalemate";
    game_over_subtitle = is_in_check(false) ? "White wins" : "Draw";
    status_message = is_in_check(false) ? "Checkmate - White wins" : "Stalemate - Draw";
    return;
  }

  move_piece(ai_move.from_row, ai_move.from_col, ai_move.to_row, ai_move.to_col);
}

void on_start_clicked(lv_event_t *event) {
  (void)event;
  start_local_game();
}

void on_ai_game_clicked(lv_event_t *event) {
  (void)event;
  show_ai_difficulty_screen();
}

void on_ai_easy_clicked(lv_event_t *event) {
  (void)event;
  set_ai_difficulty(AiDifficulty::Easy);
  start_ai_game();
}

void on_ai_normal_clicked(lv_event_t *event) {
  (void)event;
  set_ai_difficulty(AiDifficulty::Normal);
  start_ai_game();
}

void on_ai_hard_clicked(lv_event_t *event) {
  (void)event;
  set_ai_difficulty(AiDifficulty::Hard);
  start_ai_game();
}

void on_new_game_clicked(lv_event_t *event) {
  (void)event;
  start_new_game();
}

void on_title_clicked(lv_event_t *event) {
  (void)event;
  stop_pair_poll_timer();
  reset_game();
  show_start_screen();
}

lv_obj_t *create_menu_button(lv_obj_t *parent, const char *text, int width, int height) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(0x9FBAD0), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_y(button, 3, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(button, lv_color_hex(0x0B1117), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x22313F), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(0xD5E7F7), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button, 2, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_ofs_y(button, 1, LV_STATE_PRESSED);
  lv_obj_set_style_translate_y(button, 2, LV_STATE_PRESSED);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(0xF8F1DC), LV_PART_MAIN);
  lv_obj_center(label);
  return button;
}

lv_obj_t *create_small_button(lv_obj_t *parent, const char *text, int width, int height) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(0x9FBAD0), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 4, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_y(button, 2, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(button, lv_color_hex(0x0B1117), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x22313F), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(0xD5E7F7), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button, 1, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_ofs_y(button, 0, LV_STATE_PRESSED);
  lv_obj_set_style_translate_y(button, 1, LV_STATE_PRESSED);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(0xF8F1DC), LV_PART_MAIN);
  lv_obj_center(label);
  return button;
}

void show_ai_difficulty_screen() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 392, 392);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Choose AI Level");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *subtitle = lv_label_create(card);
  lv_label_set_text(subtitle, "You play White");
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 76);

  lv_obj_t *easy = create_menu_button(card, "Easy", 300, 54);
  lv_obj_align(easy, LV_ALIGN_TOP_MID, 0, 126);
  lv_obj_add_event_cb(easy, on_ai_easy_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *normal = create_menu_button(card, "Normal", 300, 54);
  lv_obj_align(normal, LV_ALIGN_TOP_MID, 0, 192);
  lv_obj_add_event_cb(normal, on_ai_normal_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *hard = create_menu_button(card, "Hard", 300, 54);
  lv_obj_align(hard, LV_ALIGN_TOP_MID, 0, 258);
  lv_obj_add_event_cb(hard, on_ai_hard_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *back = create_menu_button(card, "Back", 150, 46);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_add_event_cb(back, on_title_clicked, LV_EVENT_CLICKED, nullptr);
}

void show_game_over_overlay() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *overlay = lv_obj_create(screen);
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, kScreenPixels, kScreenPixels);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x11110F), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *panel = lv_obj_create(overlay);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 348, 224);
  lv_obj_center(panel);
  lv_obj_set_style_radius(panel, 20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x34495E), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, game_over_title);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  lv_obj_t *subtitle = lv_label_create(panel);
  lv_label_set_text(subtitle, game_over_subtitle);
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 66);

  lv_obj_t *new_game = create_menu_button(panel, "New Game", 142, 52);
  lv_obj_align(new_game, LV_ALIGN_BOTTOM_LEFT, 26, -24);
  lv_obj_add_event_cb(new_game, on_new_game_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *title_button = create_menu_button(panel, "Title", 142, 52);
  lv_obj_align(title_button, LV_ALIGN_BOTTOM_RIGHT, -26, -24);
  lv_obj_add_event_cb(title_button, on_title_clicked, LV_EVENT_CLICKED, nullptr);
}

void finish_by_resignation() {
  const bool white_resigned = white_turn;
  if (game_mode == GameMode::Lichess && lichess_game_id.length() > 0) {
    lichess_post(String("/api/board/game/") + lichess_game_id + "/resign");
  }
  clear_selection();
  game_over = true;
  game_over_title = "Resignation";
  game_over_subtitle = white_resigned ? "Black wins" : "White wins";
  status_message = white_resigned ? "White resigned" : "Black resigned";

  if (resign_overlay != nullptr) {
    lv_obj_delete(resign_overlay);
    resign_overlay = nullptr;
  }

  repaint_board();
  show_game_over_overlay();
}

void on_cancel_resign_clicked(lv_event_t *event) {
  (void)event;
  if (resign_overlay != nullptr) {
    lv_obj_delete(resign_overlay);
    resign_overlay = nullptr;
  }
}

void on_confirm_resign_clicked(lv_event_t *event) {
  (void)event;
  finish_by_resignation();
}

void show_resign_confirm_overlay() {
  if (game_over || resign_overlay != nullptr) {
    return;
  }

  lv_obj_t *screen = lv_screen_active();
  resign_overlay = lv_obj_create(screen);
  lv_obj_remove_style_all(resign_overlay);
  lv_obj_set_size(resign_overlay, kScreenPixels, kScreenPixels);
  lv_obj_set_pos(resign_overlay, 0, 0);
  lv_obj_set_style_bg_color(resign_overlay, lv_color_hex(0x11110F), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(resign_overlay, LV_OPA_50, LV_PART_MAIN);
  lv_obj_add_flag(resign_overlay, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *panel = lv_obj_create(resign_overlay);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 340, 202);
  lv_obj_center(panel);
  lv_obj_set_style_radius(panel, 18, LV_PART_MAIN);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x34495E), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "Resign?");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *message = lv_label_create(panel);
  lv_label_set_text(message, white_turn ? "White resigns" : "Black resigns");
  lv_obj_set_style_text_font(message, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(message, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
  lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 64);

  lv_obj_t *cancel = create_menu_button(panel, "Cancel", 132, 50);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 28, -22);
  lv_obj_add_event_cb(cancel, on_cancel_resign_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *resign = create_menu_button(panel, "Resign", 132, 50);
  lv_obj_align(resign, LV_ALIGN_BOTTOM_RIGHT, -28, -22);
  lv_obj_set_style_bg_color(resign, lv_color_hex(0x8A4B3D), LV_PART_MAIN);
  lv_obj_add_event_cb(resign, on_confirm_resign_clicked, LV_EVENT_CLICKED, nullptr);
}

void on_resign_clicked(lv_event_t *event) {
  (void)event;
  show_resign_confirm_overlay();
}

void on_retry_lichess_clicked(lv_event_t *event) {
  (void)event;
  show_lichess_progress_screen();
  update_lichess_progress("Starting...", "Preparing screen");
  lichess_match_pending = true;
  lichess_match_start_at = millis() + 250;
}

void on_cancel_network_clicked(lv_event_t *event) {
  (void)event;
  lichess_match_pending = false;
  WiFi.disconnect(true);
  reset_game();
  show_start_screen();
}

void on_wifi_setup_clicked(lv_event_t *event) {
  (void)event;
  stop_pair_poll_timer();
  show_wifi_scan_screen();
}

void on_lichess_pair_clicked(lv_event_t *event) {
  (void)event;
  show_lichess_pair_screen();
}

void show_lichess_progress_screen() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  network_body_label = nullptr;
  network_detail_label = nullptr;
  lichess_progress_stage_label = nullptr;
  lichess_progress_detail_label = nullptr;
  lichess_progress_log_label = nullptr;
  lichess_progress_log_text = "";
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Lichess Match");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF8F1DC), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

  lv_obj_t *panel = lv_obj_create(screen);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, 420, 330);
  lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 64);
  lv_obj_set_style_radius(panel, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *board_hint = lv_obj_create(panel);
  lv_obj_remove_style_all(board_hint);
  lv_obj_set_size(board_hint, 108, 108);
  lv_obj_align(board_hint, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_set_style_radius(board_hint, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(board_hint, lv_color_hex(0xD9CDAE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(board_hint, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(board_hint, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(board_hint, lv_color_hex(0x4F7EA5), LV_PART_MAIN);
  lv_obj_remove_flag(board_hint, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *pieces = lv_label_create(board_hint);
  lv_label_set_text(pieces, "♔♜");
  lv_obj_set_style_text_font(pieces, &chess_symbols_42, LV_PART_MAIN);
  lv_obj_set_style_text_color(pieces, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_center(pieces);

  lichess_progress_stage_label = lv_label_create(panel);
  lv_label_set_text(lichess_progress_stage_label, "Starting...");
  lv_obj_set_width(lichess_progress_stage_label, 360);
  lv_obj_set_style_text_font(lichess_progress_stage_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lichess_progress_stage_label, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_set_style_text_align(lichess_progress_stage_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(lichess_progress_stage_label, LV_ALIGN_TOP_MID, 0, 140);

  lichess_progress_detail_label = lv_label_create(panel);
  lv_label_set_text(lichess_progress_detail_label, "Preparing Lichess flow");
  lv_obj_set_width(lichess_progress_detail_label, 360);
  lv_obj_set_style_text_font(lichess_progress_detail_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lichess_progress_detail_label, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
  lv_obj_set_style_text_align(lichess_progress_detail_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(lichess_progress_detail_label, LV_ALIGN_TOP_MID, 0, 174);

  lichess_progress_log_label = lv_label_create(panel);
  lv_label_set_text(lichess_progress_log_label, "");
  lv_obj_set_width(lichess_progress_log_label, 360);
  lv_obj_set_style_text_font(lichess_progress_log_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lichess_progress_log_label, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_text_align(lichess_progress_log_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_align(lichess_progress_log_label, LV_ALIGN_TOP_LEFT, 30, 212);

  lv_obj_t *cancel = create_menu_button(screen, "Cancel", 150, 46);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_event_cb(cancel, on_cancel_network_clicked, LV_EVENT_CLICKED, nullptr);

  lv_timer_handler();
}

void show_network_status_screen(const char *title_text, const char *message, const char *detail, bool show_retry) {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  network_body_label = nullptr;
  network_detail_label = nullptr;
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 404, 332);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, title_text);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *body = lv_label_create(card);
  network_body_label = body;
  lv_label_set_text(body, message);
  lv_obj_set_width(body, 344);
  lv_obj_set_style_text_font(body, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(body, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 86);

  lv_obj_t *detail_label = lv_label_create(card);
  network_detail_label = detail_label;
  lv_label_set_text(detail_label, detail);
  lv_obj_set_width(detail_label, 344);
  lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(detail_label, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
  lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(detail_label, LV_ALIGN_TOP_MID, 0, 148);

  if (show_retry) {
    lv_obj_t *retry = create_menu_button(card, "Retry", 150, 52);
    lv_obj_align(retry, LV_ALIGN_BOTTOM_LEFT, 36, -24);
    lv_obj_add_event_cb(retry, on_retry_lichess_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title_button = create_menu_button(card, "Title", 150, 52);
    lv_obj_align(title_button, LV_ALIGN_BOTTOM_RIGHT, -36, -24);
    lv_obj_add_event_cb(title_button, on_title_clicked, LV_EVENT_CLICKED, nullptr);
  } else {
    lv_obj_t *cancel = create_menu_button(card, "Cancel", 170, 52);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(cancel, on_cancel_network_clicked, LV_EVENT_CLICKED, nullptr);
  }

  lv_timer_handler();
}

void show_wifi_connect_screen() {
  String detail = String("SSID: ") + configured_wifi_ssid;
  show_network_status_screen("Wi-Fi", "Connecting...", detail.c_str(), false);
}

void show_wifi_failed_screen(const char *reason) {
  show_network_status_screen("Wi-Fi", "Connection failed", reason, true);
}

void on_wifi_network_clicked(lv_event_t *event) {
  const auto id = *static_cast<uint8_t *>(lv_event_get_user_data(event));
  const int index = wifi_page * 6 + id;
  if (index < 0 || index >= scanned_network_count) {
    return;
  }
  selected_wifi_ssid = scanned_ssids[index];
  show_wifi_password_screen();
}

void on_wifi_prev_clicked(lv_event_t *event) {
  (void)event;
  if (wifi_page > 0) {
    --wifi_page;
  }
  show_wifi_scan_results_screen();
}

void on_wifi_next_clicked(lv_event_t *event) {
  (void)event;
  if ((wifi_page + 1) * 6 < scanned_network_count) {
    ++wifi_page;
  }
  show_wifi_scan_results_screen();
}

void on_wifi_rescan_clicked(lv_event_t *event) {
  (void)event;
  show_wifi_scan_screen();
}

void on_wifi_password_back_clicked(lv_event_t *event) {
  (void)event;
  show_wifi_scan_results_screen();
}

void on_wifi_password_connect_clicked(lv_event_t *event) {
  (void)event;
  const char *password = lv_textarea_get_text(wifi_password_textarea);
  save_config(selected_wifi_ssid, password ? password : "", configured_lichess_token);
  WiFi.disconnect(true);
  show_wifi_connect_screen();
  if (connect_wifi()) {
    show_start_screen();
  } else {
    show_wifi_password_screen();
  }
}

void show_wifi_scan_screen() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 410, 356);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Wi-Fi Scan");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  lv_obj_t *body = lv_label_create(card);
  lv_label_set_text(body, "Scanning nearby networks...");
  lv_obj_set_width(body, 350);
  lv_obj_set_style_text_font(body, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(body, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, -14);

  lv_timer_handler();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  const int found = WiFi.scanNetworks();
  scanned_network_count = found > 0 ? min(found, 20) : 0;
  for (int i = 0; i < scanned_network_count; ++i) {
    scanned_ssids[i] = WiFi.SSID(i);
    scanned_rssi[i] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  wifi_page = 0;
  show_wifi_scan_results_screen();
}

void show_wifi_scan_results_screen() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 430, 432);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Select Wi-Fi");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

  if (scanned_network_count == 0) {
    lv_obj_t *empty = lv_label_create(card);
    lv_label_set_text(empty, "No networks found");
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x34495E), LV_PART_MAIN);
    lv_obj_align(empty, LV_ALIGN_CENTER, 0, -10);
  }

  const int start = wifi_page * 6;
  const int end = min(start + 6, scanned_network_count);
  for (int i = start; i < end; ++i) {
    const int slot = i - start;
    wifi_button_ids[slot] = slot;
    String label = scanned_ssids[i] + "  " + String(scanned_rssi[i]) + " dBm";
    lv_obj_t *network = create_menu_button(card, label.c_str(), 366, 42);
    lv_obj_align(network, LV_ALIGN_TOP_MID, 0, 64 + slot * 48);
    lv_obj_add_event_cb(network, on_wifi_network_clicked, LV_EVENT_CLICKED, &wifi_button_ids[slot]);
  }

  lv_obj_t *prev = create_menu_button(card, "Prev", 96, 44);
  lv_obj_align(prev, LV_ALIGN_BOTTOM_LEFT, 18, -18);
  lv_obj_add_event_cb(prev, on_wifi_prev_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *rescan = create_menu_button(card, "Rescan", 118, 44);
  lv_obj_align(rescan, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_add_event_cb(rescan, on_wifi_rescan_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *next = create_menu_button(card, "Next", 96, 44);
  lv_obj_align(next, LV_ALIGN_BOTTOM_RIGHT, -18, -18);
  lv_obj_add_event_cb(next, on_wifi_next_clicked, LV_EVENT_CLICKED, nullptr);
}

void show_wifi_password_screen() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(screen);
  String title_text = String("Password for ") + selected_wifi_ssid;
  lv_label_set_text(title, title_text.c_str());
  lv_obj_set_width(title, 440);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

  wifi_password_textarea = lv_textarea_create(screen);
  lv_obj_set_size(wifi_password_textarea, 420, 54);
  lv_obj_align(wifi_password_textarea, LV_ALIGN_TOP_MID, 0, 52);
  lv_textarea_set_password_mode(wifi_password_textarea, true);
  lv_textarea_set_one_line(wifi_password_textarea, true);
  lv_textarea_set_placeholder_text(wifi_password_textarea, "Wi-Fi password");

  lv_obj_t *back = create_menu_button(screen, "Back", 130, 46);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 30, 118);
  lv_obj_add_event_cb(back, on_wifi_password_back_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *connect = create_menu_button(screen, "Connect", 170, 46);
  lv_obj_align(connect, LV_ALIGN_TOP_RIGHT, -30, 118);
  lv_obj_add_event_cb(connect, on_wifi_password_connect_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *keyboard = lv_keyboard_create(screen);
  lv_obj_set_size(keyboard, 480, 290);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(keyboard, wifi_password_textarea);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
}

void on_pair_poll_timer(lv_timer_t *timer) {
  (void)timer;
  if (pair_device_id.length() == 0) {
    return;
  }
  const String body = pairing_status_body();
  if (body.length() == 0) {
    if (pair_status_label) {
      lv_label_set_text(pair_status_label, "Connection error. Retrying...");
    }
    return;
  }

  if (json_has_status(body, "pending")) {
    if (pair_status_label) {
      lv_label_set_text(pair_status_label, "Waiting for Lichess approval...");
    }
    return;
  }

  if (json_has_status(body, "expired")) {
    stop_pair_poll_timer();
    if (pair_status_label) {
      lv_label_set_text(pair_status_label, "Code expired. Tap Login again.");
    }
    return;
  }

  if (!json_has_status(body, "linked")) {
    if (pair_status_label) {
      lv_label_set_text(pair_status_label, "Unexpected response. Retrying...");
    }
    return;
  }

  const String token = json_string_value(body, "accessToken");
  const String username = json_string_value(body, "username");
  if (token.length() == 0) {
    if (pair_status_label) {
      lv_label_set_text(pair_status_label, "Linked, but token was missing.");
    }
    return;
  }

  stop_pair_poll_timer();
  save_config(configured_wifi_ssid, configured_wifi_password, token);
  lichess_username = username;
  show_start_screen();
}

void show_lichess_pair_screen() {
  stop_pair_poll_timer();
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 428, 400);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Lichess Login");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  pair_status_label = lv_label_create(card);
  lv_label_set_text(pair_status_label, "Starting secure login...");
  lv_obj_set_width(pair_status_label, 360);
  lv_obj_set_style_text_font(pair_status_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(pair_status_label, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_text_align(pair_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(pair_status_label, LV_ALIGN_TOP_MID, 0, 76);

  lv_timer_handler();

  if (!wifi_configured()) {
    lv_label_set_text(pair_status_label, "Set up Wi-Fi first.");
  } else if (String(PAIRING_BASE_URL).length() == 0) {
    lv_label_set_text(pair_status_label, "PAIRING_BASE_URL is missing.");
  } else if (!connect_wifi()) {
    lv_label_set_text(pair_status_label, "Wi-Fi connection failed.");
  } else {
    String pair_code;
    String pair_url;
    if (start_pairing_session(pair_device_id, pair_code, pair_url)) {
      lv_label_set_text(pair_status_label, "Open this on your phone:");

      lv_obj_t *url = lv_label_create(card);
      lv_label_set_text(url, pair_url.c_str());
      lv_obj_set_width(url, 370);
      lv_obj_set_style_text_font(url, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_style_text_color(url, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
      lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 134);

      lv_obj_t *code = lv_label_create(card);
      lv_label_set_text(code, pair_code.c_str());
      lv_obj_set_style_text_font(code, &lv_font_montserrat_28, LV_PART_MAIN);
      lv_obj_set_style_text_color(code, lv_color_hex(0x263526), LV_PART_MAIN);
      lv_obj_align(code, LV_ALIGN_CENTER, 0, 34);

      lv_obj_t *hint = lv_label_create(card);
      lv_label_set_text(hint, "Approve board:play on Lichess.\nThe board will finish automatically.");
      lv_obj_set_width(hint, 360);
      lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_style_text_color(hint, lv_color_hex(0x34495E), LV_PART_MAIN);
      lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -82);

      pair_poll_timer = lv_timer_create(on_pair_poll_timer, 2500, nullptr);
    } else {
      lv_label_set_text(pair_status_label, "Could not start login.");
    }
  }

  lv_obj_t *wifi = create_menu_button(card, "Wi-Fi", 116, 48);
  lv_obj_align(wifi, LV_ALIGN_BOTTOM_LEFT, 28, -22);
  lv_obj_add_event_cb(wifi, on_wifi_setup_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *retry = create_menu_button(card, "Retry", 116, 48);
  lv_obj_align(retry, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_add_event_cb(retry, on_lichess_pair_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *title_button = create_menu_button(card, "Title", 116, 48);
  lv_obj_align(title_button, LV_ALIGN_BOTTOM_RIGHT, -28, -22);
  lv_obj_add_event_cb(title_button, on_title_clicked, LV_EVENT_CLICKED, nullptr);
}

void show_lichess_error_screen(const char *reason) {
  show_network_status_screen("Lichess", "Cannot continue", reason, true);
}

void begin_lichess_match() {
  if (!lichess_progress_stage_label) {
    show_lichess_progress_screen();
  }
  update_lichess_progress("Starting...", "Checking saved settings");
  pump_ui(120);

  if (!wifi_configured()) {
    lichess_log("Wi-Fi is not configured");
    show_wifi_scan_screen();
    return;
  }

  if (!lichess_token_configured()) {
    lichess_log("Lichess login is missing");
    show_lichess_pair_screen();
    return;
  }

  update_lichess_progress("Connecting Wi-Fi...", configured_wifi_ssid.c_str());
  if (!connect_wifi()) {
    show_wifi_failed_screen("Check SSID, password, or signal strength");
    return;
  }
  update_lichess_progress("Wi-Fi connected", WiFi.localIP().toString().c_str());

  update_lichess_progress("Checking account...", "Validating Lichess token");
  if (!load_lichess_account()) {
    show_lichess_error_screen("Token failed. Need board:play scope.");
    return;
  }
  update_lichess_progress("Account OK", lichess_username.c_str());

  update_lichess_progress("Finding match...", "Casual 10+0 rapid");
  const SeekResult seek_result = seek_lichess_game();
  if (seek_result.status != SeekStatus::Matched) {
    String detail = seek_result.detail;
    if (detail.length() == 0 && seek_result.http_code != 0) {
      detail = String("HTTP ") + String(seek_result.http_code);
    }
    if (detail.length() == 0) {
      detail = "Check Wi-Fi, token, or Lichess status";
    }
    show_network_status_screen("Finding Match", seek_status_text(seek_result), detail.c_str(), true);
    return;
  }
  lichess_game_id = seek_result.game_id;
  update_lichess_progress("Game found", lichess_game_id.c_str());

  reset_board_position();
  clear_selection();
  game_over = false;
  game_mode = GameMode::Lichess;
  white_turn = true;
  lichess_last_moves = "";
  set_dynamic_status(String("Game found | You: ") + (lichess_is_white ? "White" : "Black"));
  update_lichess_progress("Opening game stream...", lichess_is_white ? "You are White" : "You are Black");
  create_chessboard();
  if (!connect_lichess_stream()) {
    if (lichess_stream_error_detail.length() > 0) {
      status_message = lichess_stream_error_detail.c_str();
    } else {
      status_message = "Lichess stream failed";
    }
    repaint_board();
  }
}

void on_lichess_match_clicked(lv_event_t *event) {
  (void)event;
  show_lichess_progress_screen();
  update_lichess_progress("Starting...", "Preparing screen");
  lichess_match_pending = true;
  lichess_match_start_at = millis() + 250;
}

void on_square_clicked(lv_event_t *event) {
  if (game_over) {
    return;
  }

  if (game_mode == GameMode::Lichess && !lichess_my_turn) {
    set_lichess_turn_status();
    repaint_board();
    return;
  }

  if (game_mode == GameMode::Ai && !white_turn) {
    status_message = "AI thinking...";
    repaint_board();
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
    if (game_mode == GameMode::Lichess) {
      const String uci = uci_from_move(selected_row, selected_col, row, col);
      Serial.printf("Sending Lichess move: %s\n", uci.c_str());
      set_dynamic_status(String("Sending ") + uci + "...");
      repaint_board();
      if (lichess_post(String("/api/board/game/") + lichess_game_id + "/move/" + uci)) {
        clear_selection();
        lichess_my_turn = false;
        lichess_pending_uci = uci;
        lichess_pending_since = millis();
        set_dynamic_status(String("Move sent ") + uci + " | Waiting sync");
      } else {
        if (lichess_move_error_detail.length() > 0) {
          set_dynamic_status(String("Move failed ") + uci + " | " + lichess_move_error_detail);
        } else {
          set_dynamic_status(String("Move failed ") + uci);
        }
      }
    } else {
      move_piece(selected_row, selected_col, row, col);
      make_ai_move_if_needed();
    }
  } else {
    status_message = "Illegal move";
  }

  repaint_board();
  if (game_over) {
    show_game_over_overlay();
  }
}

void show_start_screen() {
  stop_pair_poll_timer();
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 424, 432);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Local Chess");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *subtitle = lv_label_create(card);
  lv_label_set_text(subtitle, "Touch chess on ESP32");
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 64);

  lv_obj_t *pieces = lv_label_create(card);
  lv_label_set_text(pieces, "♔♕♖♚♛♜");
  lv_obj_set_style_text_font(pieces, &chess_symbols_42, LV_PART_MAIN);
  lv_obj_set_style_text_color(pieces, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(pieces, LV_ALIGN_TOP_MID, 0, 106);

  lv_obj_t *local = create_menu_button(card, "Local Game", 168, 56);
  lv_obj_align(local, LV_ALIGN_TOP_LEFT, 28, 164);
  lv_obj_add_event_cb(local, on_start_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *ai = create_menu_button(card, "AI Game", 168, 56);
  lv_obj_align(ai, LV_ALIGN_TOP_RIGHT, -28, 164);
  lv_obj_add_event_cb(ai, on_ai_game_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *lichess = create_menu_button(card, "Lichess", 168, 54);
  lv_obj_align(lichess, LV_ALIGN_TOP_LEFT, 28, 234);
  lv_obj_add_event_cb(lichess, on_lichess_match_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *setup = create_menu_button(card, "Wi-Fi Setup", 168, 52);
  lv_obj_align(setup, LV_ALIGN_TOP_RIGHT, -28, 234);
  lv_obj_add_event_cb(setup, on_wifi_setup_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *login = create_menu_button(card, "Lichess Login", 364, 52);
  lv_obj_align(login, LV_ALIGN_TOP_MID, 0, 304);
  lv_obj_add_event_cb(login, on_lichess_pair_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *hint = lv_label_create(card);
  String readiness = String("Wi-Fi: ") + configured_text(wifi_configured()) + "  Lichess: " + configured_text(lichess_token_configured());
  lv_label_set_text(hint, readiness.c_str());
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
}

void create_chessboard() {
  Serial.println("Creating chessboard UI");
#ifdef BOARD_HAS_TOUCH
  touch_calibration_data.valid = false;
#endif

  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

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
      const int display_row = board_to_display_row(row);
      const int display_col = board_to_display_col(col);
      square_ids[id] = id;

      lv_obj_t *square = lv_obj_create(board_frame);
      squares[row][col] = square;
      lv_obj_remove_style_all(square);
      lv_obj_set_size(square, kSquarePixels, kSquarePixels);
      lv_obj_set_pos(square, display_col * kSquarePixels, display_row * kSquarePixels);
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

      if (display_col == 0) {
        lv_obj_t *rank = lv_label_create(square);
        rank_labels[row] = rank;
        lv_label_set_text_fmt(rank, "%d", 8 - row);
        lv_obj_set_style_text_font(rank, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(rank, LV_ALIGN_TOP_LEFT, 3, 1);
        lv_obj_remove_flag(rank, LV_OBJ_FLAG_CLICKABLE);
      }

      if (display_row == 7) {
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
  lv_obj_set_width(status_label, 372);
  lv_obj_set_style_bg_color(status_label, lv_color_hex(0x17202A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_pos(status_label, 12, kHudOffsetY + 7);
  lv_obj_remove_flag(status_label, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *resign_button = create_small_button(screen, "Resign", 84, 24);
  lv_obj_set_pos(resign_button, kScreenPixels - 94, kHudOffsetY + 4);
  lv_obj_add_event_cb(resign_button, on_resign_clicked, LV_EVENT_CLICKED, nullptr);

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

  load_config();
  reset_game();
  show_start_screen();
}

void loop() {
  static auto lv_last_tick = millis();
  const auto now = millis();

  lv_tick_inc(now - lv_last_tick);
  lv_last_tick = now;
  lv_timer_handler();

  if (lichess_match_pending && now >= lichess_match_start_at) {
    lichess_match_pending = false;
    begin_lichess_match();
  }

  poll_lichess_stream();

  delay(5);
}
