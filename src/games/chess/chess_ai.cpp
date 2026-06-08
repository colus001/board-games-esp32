#include "chess_ai.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {
constexpr int kMaxAiMoves = 128;
constexpr int kMaxAiSearchDepth = 3;

AiDifficulty ai_difficulty = AiDifficulty::Normal;
int ai_search_depth = 2;
int ai_min_think_ms = 800;
uint32_t ai_search_nodes = 0;
ChessMove ai_move_buffers[kMaxAiSearchDepth + 2][kMaxAiMoves];

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
} // namespace

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

AiDifficulty current_ai_difficulty() {
  return ai_difficulty;
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

const char *ai_game_status_text() {
  switch (ai_difficulty) {
  case AiDifficulty::Easy:
    return "White vs AI Easy";
  case AiDifficulty::Normal:
    return "White vs AI Normal";
  case AiDifficulty::Hard:
    return "White vs AI Hard";
  }
  return "White vs AI";
}

int ai_minimum_think_ms() {
  return ai_min_think_ms;
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
