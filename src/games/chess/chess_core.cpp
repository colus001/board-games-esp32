#include "chess_core.h"

#include "../../common/utils.h"

namespace {
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
  const int col_delta = abs_int(to_col - from_col);

  if (col_delta == 0) {
    if (row_delta == direction && board[to_row][to_col] == '.') {
      return true;
    }
    return from_row == start_row && row_delta == 2 * direction && board[from_row + direction][from_col] == '.' && board[to_row][to_col] == '.';
  }

  return col_delta == 1 && row_delta == direction && board[to_row][to_col] != '.' && !same_side(piece, board[to_row][to_col]);
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
  if (from_row < 0 || from_row > 7 || from_col < 0 || from_col > 7 || to_row < 0 || to_row > 7 || to_col < 0 || to_col > 7) {
    return false;
  }
  if (from_row == to_row && from_col == to_col) {
    return false;
  }

  const char piece = board[from_row][from_col];
  const char target = board[to_row][to_col];
  if (piece == '.' || same_side(piece, target)) {
    return false;
  }
  if (lower_piece(target) == 'k') {
    return false;
  }

  const int row_delta = to_row - from_row;
  const int col_delta = to_col - from_col;
  switch (lower_piece(piece)) {
  case 'p':
    return is_legal_pawn_move(piece, from_row, from_col, to_row, to_col);
  case 'r':
    return (row_delta == 0 || col_delta == 0) && is_path_clear(from_row, from_col, to_row, to_col);
  case 'n':
    return (abs_int(row_delta) == 2 && abs_int(col_delta) == 1) || (abs_int(row_delta) == 1 && abs_int(col_delta) == 2);
  case 'b':
    return abs_int(row_delta) == abs_int(col_delta) && is_path_clear(from_row, from_col, to_row, to_col);
  case 'q':
    return ((row_delta == 0 || col_delta == 0) || abs_int(row_delta) == abs_int(col_delta)) && is_path_clear(from_row, from_col, to_row, to_col);
  case 'k':
    return abs_int(row_delta) <= 1 && abs_int(col_delta) <= 1;
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

  if (piece == '.') {
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
    return ((row_delta == 0 || col_delta == 0) || (abs_row == abs_col)) && is_path_clear(from_row, from_col, to_row, to_col);
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
      if (piece == '.' || is_white_piece(piece) != by_white) {
        continue;
      }
      if (piece_attacks_square(from_row, from_col, row, col)) {
        return true;
      }
    }
  }
  return false;
}

bool would_leave_king_in_check(int from_row, int from_col, int to_row, int to_col) {
  const char moving_piece = board[from_row][from_col];
  const bool moving_white = is_white_piece(moving_piece);
  const char captured = board[to_row][to_col];
  board[to_row][to_col] = moving_piece;
  board[from_row][from_col] = '.';

  const bool in_check = is_in_check(moving_white);

  board[from_row][from_col] = moving_piece;
  board[to_row][to_col] = captured;
  return in_check;
}

int center_bonus(int row, int col) {
  const int row_dist = abs_int(3 - row) < abs_int(4 - row) ? abs_int(3 - row) : abs_int(4 - row);
  const int col_dist = abs_int(3 - col) < abs_int(4 - col) ? abs_int(3 - col) : abs_int(4 - col);
  return 14 - (row_dist + col_dist) * 3;
}
} // namespace

char board[8][8];

void reset_board_position() {
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      board[row][col] = kInitialBoard[row][col];
    }
  }
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

bool same_side(char a, char b) {
  return (is_white_piece(a) && is_white_piece(b)) || (is_black_piece(a) && is_black_piece(b));
}

char lower_piece(char piece) {
  if (piece >= 'A' && piece <= 'Z') {
    return piece - 'A' + 'a';
  }
  return piece;
}

int file_to_col(char file) {
  return file - 'a';
}

int rank_to_row(char rank) {
  return '8' - rank;
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

bool is_in_check(bool white) {
  int king_row = -1;
  int king_col = -1;
  return find_king(white, &king_row, &king_col) && is_square_attacked(king_row, king_col, !white);
}

bool is_legal_move(int from_row, int from_col, int to_row, int to_col) {
  return is_legal_basic_move(from_row, from_col, to_row, to_col) && !would_leave_king_in_check(from_row, from_col, to_row, to_col);
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
