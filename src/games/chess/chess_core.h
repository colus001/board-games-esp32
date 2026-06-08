#pragma once

#include <Arduino.h>

struct ChessMove {
  int from_row = -1;
  int from_col = -1;
  int to_row = -1;
  int to_col = -1;
  int score = -32000;
};

extern char board[8][8];

void reset_board_position();
bool is_light_square(int row, int col);
bool is_white_piece(char piece);
bool is_black_piece(char piece);
bool same_side(char a, char b);
char lower_piece(char piece);
int file_to_col(char file);
int rank_to_row(char rank);
void apply_uci_move(const String &move);
bool is_legal_move(int from_row, int from_col, int to_row, int to_col);
bool has_any_legal_move(bool white);
bool is_in_check(bool white);
int piece_value(char piece);
int evaluate_position_for(bool white);
char apply_temp_move(const ChessMove &move);
void undo_temp_move(const ChessMove &move, char captured);
int generate_legal_moves(bool white, ChessMove moves[], int max_moves);
