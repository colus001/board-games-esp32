#pragma once

#include "chess_core.h"

enum class AiDifficulty { Easy, Normal, Hard };

void set_ai_difficulty(AiDifficulty difficulty);
AiDifficulty current_ai_difficulty();
const char *ai_thinking_text();
const char *ai_game_status_text();
int ai_minimum_think_ms();
ChessMove choose_ai_move(bool white);
