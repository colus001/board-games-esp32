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
uint8_t square_ids[64];

enum class GameMode { Local, Lichess };

void show_start_screen();
void create_chessboard();
void show_game_over_overlay();
void begin_lichess_match();
void start_local_game();
void clear_selection();
void repaint_board();
void show_wifi_scan_screen();
void show_wifi_scan_results_screen();
void show_wifi_password_screen();
void show_lichess_pair_screen();

int selected_row = -1;
int selected_col = -1;
bool white_turn = true;
bool game_over = false;
GameMode game_mode = GameMode::Local;
const char *status_message = "White to move";
const char *game_over_title = "Game Over";
const char *game_over_subtitle = "";

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
bool lichess_stream_headers_done = false;
bool lichess_is_white = true;
bool lichess_my_turn = false;

enum class SeekStatus {
  Matched,
  EventStreamFailed,
  SeekBeginFailed,
  SeekRejected,
  SeekUnauthorized,
  SeekForbidden,
  SeekHttpError,
  EventStreamClosed,
  Timeout,
};

struct SeekResult {
  SeekStatus status = SeekStatus::Timeout;
  String game_id;
  int http_code = 0;
};

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
  white_turn = true;
  game_over = false;
  game_mode = GameMode::Local;
  status_message = "White to move";
  game_over_title = "Game Over";
  game_over_subtitle = "";
  lichess_stream_client.stop();
  lichess_game_id = "";
  lichess_last_moves = "";
  lichess_line_buffer = "";
  lichess_stream_headers_done = false;
  lichess_is_white = true;
  lichess_my_turn = false;
}

void start_new_game() {
  const GameMode previous_mode = game_mode;
  if (previous_mode == GameMode::Lichess) {
    begin_lichess_match();
  } else {
    start_local_game();
  }
}

void start_local_game() {
  reset_game();
  game_mode = GameMode::Local;
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

bool json_has_status(const String &json, const char *status) {
  return json.indexOf(String("\"status\":\"") + status + "\"") >= 0;
}

bool wifi_configured() {
  return configured_wifi_ssid.length() > 0;
}

bool lichess_token_configured() {
  return configured_lichess_token.length() > 0;
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
    apply_uci_move(moves.substring(start, end));
    ++move_count;
    start = end + 1;
  }

  clear_selection();
  white_turn = (move_count % 2) == 0;
  lichess_my_turn = white_turn == lichess_is_white;
  status_message = lichess_my_turn ? "Your move" : "Waiting for opponent";
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
  lv_timer_handler();
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
  case SeekStatus::EventStreamClosed:
    return "Event stream closed";
  case SeekStatus::Timeout:
    return "No match after 90 sec";
  case SeekStatus::Matched:
    return "Matched";
  }
  return "Match failed";
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

  update_network_status("Creating seek...", "Casual 5+3 / random color");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, "https://lichess.org/api/board/seek")) {
    event_client.stop();
    Serial.println("Lichess seek http.begin failed");
    result.status = SeekStatus::SeekBeginFailed;
    return result;
  }
  http.setTimeout(15000);
  http.addHeader("Authorization", String("Bearer ") + configured_lichess_token);
  http.addHeader("Accept", "application/x-ndjson");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  const int code = http.POST("rated=false&time=5&increment=3&color=random&variant=standard");
  result.http_code = code;
  Serial.printf("Lichess seek HTTP code: %d\n", code);
  if (code < 200 || code >= 300) {
    const String error_body = http.getString();
    Serial.printf("Lichess seek error body: %.220s\n", error_body.c_str());
  }
  http.end();
  if (code < 200 || code >= 300) {
    event_client.stop();
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
  String line;
  const unsigned long started = millis();
  unsigned long last_update = 0;
  while (millis() - started < 90000) {
    if (!event_client.connected()) {
      event_client.stop();
      Serial.println("Lichess event stream closed before match");
      result.status = SeekStatus::EventStreamClosed;
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
          int game_start = line.indexOf("\"game\"");
          if (game_start < 0) {
            game_start = line.indexOf("gameStart");
          }
          const int id_key = line.indexOf("\"id\":\"", game_start);
          if (id_key >= 0) {
            const int id_start = id_key + 6;
            const int id_end = line.indexOf('"', id_start);
            if (id_end > id_start) {
              const String game_id = line.substring(id_start, id_end);
              event_client.stop();
              Serial.printf("Lichess matched game id: %s\n", game_id.c_str());
              result.status = SeekStatus::Matched;
              result.game_id = game_id;
              return result;
            }
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
  Serial.println("Lichess seek timed out after 90 seconds");
  result.status = SeekStatus::Timeout;
  return result;
}

bool lichess_post(const String &path) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String("https://lichess.org") + path)) {
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + configured_lichess_token);
  const int code = http.POST("");
  http.end();
  return code >= 200 && code < 300;
}

bool connect_lichess_stream() {
  lichess_stream_client.stop();
  lichess_stream_client.setInsecure();
  lichess_stream_client.setTimeout(5);
  if (!lichess_stream_client.connect("lichess.org", 443)) {
    return false;
  }

  lichess_stream_client.print(String("GET /api/board/game/stream/") + lichess_game_id + " HTTP/1.1\r\n");
  lichess_stream_client.print("Host: lichess.org\r\n");
  lichess_stream_client.print(String("Authorization: Bearer ") + configured_lichess_token + "\r\n");
  lichess_stream_client.print("Accept: application/x-ndjson\r\n");
  lichess_stream_client.print("Connection: keep-alive\r\n\r\n");
  lichess_stream_headers_done = false;
  lichess_line_buffer = "";
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

  const bool game_full = line.indexOf("gameFull") >= 0;
  if (game_full) {
    update_lichess_side_from_game(line);
  }

  String moves = json_string_value(line, "moves");
  if (game_full || moves != lichess_last_moves) {
    lichess_last_moves = moves;
    apply_lichess_moves(moves);
    repaint_board();
  }

  const String status = json_string_value(line, "status");
  if (status == "mate" || status == "resign" || status == "timeout" || status == "draw" || status == "stalemate") {
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
    } else {
      game_over_subtitle = "Draw";
    }
    status_message = "Lichess game over";
    repaint_board();
    show_game_over_overlay();
  }
}

void poll_lichess_stream() {
  if (game_mode != GameMode::Lichess || lichess_game_id.length() == 0 || game_over) {
    return;
  }
  if (!lichess_stream_client.connected()) {
    connect_lichess_stream();
    return;
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

void on_start_clicked(lv_event_t *event) {
  (void)event;
  start_local_game();
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
  begin_lichess_match();
}

void on_cancel_network_clicked(lv_event_t *event) {
  (void)event;
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

void show_lichess_status_screen(const char *message, bool show_back_button) {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x17202A), LV_PART_MAIN);

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 392, 300);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xF1E1BB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x9FBAD0), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Lichess Match");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

  lv_obj_t *body = lv_label_create(card);
  lv_label_set_text(body, message);
  lv_obj_set_width(body, 320);
  lv_obj_set_style_text_font(body, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(body, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, 8);

  if (show_back_button) {
    lv_obj_t *back = create_menu_button(card, "Title", 160, 54);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(back, on_title_clicked, LV_EVENT_CLICKED, nullptr);
  }

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

void show_matchmaking_screen() {
  show_network_status_screen("Finding Match", "Casual 5+3", "Random color / Standard chess", false);
}

void begin_lichess_match() {
  if (!wifi_configured()) {
    show_wifi_scan_screen();
    return;
  }

  if (!lichess_token_configured()) {
    show_lichess_pair_screen();
    return;
  }

  show_wifi_connect_screen();
  if (!connect_wifi()) {
    show_wifi_failed_screen("Check SSID, password, or signal strength");
    return;
  }

  show_lichess_status_screen("Checking Lichess account...", false);
  if (!load_lichess_account()) {
    show_lichess_error_screen("Token failed. Need board:play scope.");
    return;
  }

  show_matchmaking_screen();
  const SeekResult seek_result = seek_lichess_game();
  if (seek_result.status != SeekStatus::Matched) {
    String detail = String("HTTP ") + String(seek_result.http_code);
    if (seek_result.http_code == 0) {
      detail = "Check Wi-Fi, token, or Lichess status";
    }
    show_network_status_screen("Finding Match", seek_status_text(seek_result), detail.c_str(), true);
    return;
  }
  lichess_game_id = seek_result.game_id;

  reset_board_position();
  clear_selection();
  game_over = false;
  game_mode = GameMode::Lichess;
  white_turn = true;
  lichess_last_moves = "";
  status_message = "Syncing Lichess...";
  create_chessboard();
  connect_lichess_stream();
}

void on_lichess_match_clicked(lv_event_t *event) {
  (void)event;
  begin_lichess_match();
}

void on_square_clicked(lv_event_t *event) {
  if (game_over) {
    return;
  }

  if (game_mode == GameMode::Lichess && !lichess_my_turn) {
    status_message = "Waiting for opponent";
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
      if (lichess_post(String("/api/board/game/") + lichess_game_id + "/move/" + uci)) {
        move_piece(selected_row, selected_col, row, col);
        lichess_my_turn = false;
        status_message = "Waiting for opponent";
      } else {
        status_message = "Lichess rejected move";
      }
    } else {
      move_piece(selected_row, selected_col, row, col);
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
  lv_obj_set_size(card, 408, 400);
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
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *subtitle = lv_label_create(card);
  lv_label_set_text(subtitle, "Two-player chess on ESP32");
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x5E3F2A), LV_PART_MAIN);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 76);

  lv_obj_t *pieces = lv_label_create(card);
  lv_label_set_text(pieces, "♔♕♖♚♛♜");
  lv_obj_set_style_text_font(pieces, &chess_symbols_42, LV_PART_MAIN);
  lv_obj_set_style_text_color(pieces, lv_color_hex(0x263526), LV_PART_MAIN);
  lv_obj_align(pieces, LV_ALIGN_TOP_MID, 0, 124);

  lv_obj_t *local = create_menu_button(card, "Local Game", 168, 56);
  lv_obj_align(local, LV_ALIGN_TOP_LEFT, 24, 190);
  lv_obj_add_event_cb(local, on_start_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *lichess = create_menu_button(card, "Lichess", 168, 56);
  lv_obj_align(lichess, LV_ALIGN_TOP_RIGHT, -24, 190);
  lv_obj_add_event_cb(lichess, on_lichess_match_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *setup = create_menu_button(card, "Wi-Fi Setup", 168, 52);
  lv_obj_align(setup, LV_ALIGN_TOP_LEFT, 24, 264);
  lv_obj_add_event_cb(setup, on_wifi_setup_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *login = create_menu_button(card, "Lichess Login", 168, 52);
  lv_obj_align(login, LV_ALIGN_TOP_RIGHT, -24, 264);
  lv_obj_add_event_cb(login, on_lichess_pair_clicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *hint = lv_label_create(card);
  String readiness = String("Wi-Fi: ") + configured_text(wifi_configured()) + "  Lichess: " + configured_text(lichess_token_configured());
  lv_label_set_text(hint, readiness.c_str());
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);
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
  poll_lichess_stream();

  delay(5);
}
