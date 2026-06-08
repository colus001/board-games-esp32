# AGENTS.md

Guidance for AI agents and contributors working in this repository.

## Project Goal

This repository is an ESP32-S3 touchscreen board-games project for Sunton/CYD-style displays.

Current product:
- 480x480 touch chess app for `esp32-4848S040CIY1`
- Local two-player chess
- On-device Wi-Fi setup
- Lichess OAuth pairing through a Cloudflare Worker
- Lichess matchmaking and Board API play
- Simple on-device chess AI

Longer-term goal:
- Keep chess working while adding other board games, including Korean janggi, Go/baduk, and Japanese shogi.
- Shared app shell, display setup, Wi-Fi setup, pairing patterns, and common UI widgets should be reusable across games.

## Build Environment

Use PlatformIO with Arduino framework.

Primary commands:
```bash
/opt/homebrew/bin/pio run
/opt/homebrew/bin/pio run --target upload
```

Current upload port is usually:
```text
/dev/cu.usbserial-10
```

Important PlatformIO settings:
- Board: `esp32-4848S040CIY1`
- Platform: `espressif32@6.5.0`
- Framework: Arduino
- LVGL is used through `esp32-smartdisplay`
- `upload_speed = 115200`
- `board_upload.use_1200bps_touch = false`

Known warnings:
- `SMARTDISPLAY_DMA_BUFFER_SIZE` redefinition warning is currently expected.
- LVGL `Possible failure to include lv_conf.h` pragma message has appeared during successful builds.

## Hardware Notes

Target display:
- Sunton ESP32-4848S040C family
- Board definition: `boards/esp32-4848S040CIY1.json`
- Resolution: 480x480
- Touch controller: GT911 capacitive touch

Important GT911 board definition values:
```text
GT911_TOUCH_CONFIG_X_MAX=1085
GT911_TOUCH_CONFIG_Y_MAX=600
```

Do not replace this with runtime calibration unless there is a clear reason. The current board definition fix is intentional.

## Current Source Layout

```text
src/
  main.cpp
  chess_symbols_42.c

  common/
    utils.h
    utils.cpp
    ui_widgets.h
    ui_widgets.cpp

  games/
    chess/
      chess_core.h
      chess_core.cpp
      chess_ai.h
      chess_ai.cpp

worker/
  src/index.js
  wrangler.jsonc
```

`main.cpp` still contains the app flow, screens, Wi-Fi screens, Lichess flow, and chess UI/controller. It has already been partially refactored, but more separation is expected.

## Architectural Direction

Prefer adding new games under:
```text
src/games/<game-name>/
```

For example:
```text
src/games/janggi/
src/games/baduk/
src/games/shogi/
```

Each game should eventually own:
- Board state and rules
- Move representation
- Touch/controller logic
- Rendering or board-specific UI
- AI, if any
- Online integration, if any

Shared code belongs in:
```text
src/common/
```

App-level state and navigation should eventually move into:
```text
src/app/
```

Network setup should eventually move into:
```text
src/network/
```

Lichess-specific code should eventually move into:
```text
src/lichess/
```

## Common Code Rules

Use `src/common/` only for code that is likely reusable by multiple games.

Good common candidates:
- JSON string helpers
- HTTP line/status helpers
- UI widgets such as standard buttons/cards
- Display constants or generic screen utilities
- `pump_ui()` style LVGL yield helpers

Do not put chess, Lichess, or game-specific assumptions in `common/`.

## Chess Code

Current chess modules:
- `games/chess/chess_core.*`
- `games/chess/chess_ai.*`

`chess_core` owns:
- `board[8][8]`
- Initial position
- Piece helpers
- UCI move application
- Legal move checks
- Check/checkmate/stalemate support helpers
- Evaluation helpers used by AI

`chess_ai` owns:
- AI difficulty
- Thinking text
- Negamax/alpha-beta search
- Move selection

Chess UI and controller logic still live in `main.cpp` and should be extracted later into:
```text
src/games/chess/chess_ui.*
src/games/chess/chess_controller.*
```

When changing chess rules, preserve the current behavior unless intentionally improving it:
- Checkmate/stalemate should be based on legal moves and check state.
- Do not revert to “king capture” game-ending logic.
- Lichess server stream remains authoritative for online games.
- In Lichess mode, do not optimistically mutate the board after sending a move. Wait for stream sync.

Known chess limitations:
- Castling is only partially handled for UCI application, not fully available through local rules/UI.
- En passant is not fully implemented in local rules.
- Promotion UI is incomplete; current default is queen promotion.

## Lichess Integration

Lichess uses Board API with OAuth token scope:
```text
board:play
```

Important Lichess decisions:
- Use Cloudflare Worker OAuth pairing rather than token entry on device.
- Use Board API random seek first.
- Current seek is casual `10+0` rapid with random color.
- Keep seek/event stream behavior conservative; do not hammer Lichess.
- Reconnects should use backoff.
- Server stream `moves` is authoritative.
- Ignore stream lines with no `moves` field for board state updates.

Important bug history:
- Parsing `gameStart` by generic `id` once picked `opponent.id` instead of `gameId`. Always use `gameId`.
- Closing seek stream too early can break matchmaking behavior.
- Applying `moves=""` from events without a `moves` field reset the board incorrectly. Only apply moves when the JSON line actually contains a string `moves` key.
- Local optimistic moves caused apparent reverts. For Lichess, POST success should set pending status only; stream update applies the board.

When sending moves:
- Display/log the UCI move.
- Preserve HTTP status and body for diagnostics.
- On success, mark pending UCI and wait for stream sync.
- Clear pending once stream `moves` includes that UCI.

## Cloudflare Worker

Worker files:
```text
worker/src/index.js
worker/wrangler.jsonc
```

Current preferred Wrangler config format:
```text
wrangler.jsonc
```

Do not reintroduce `wrangler.toml` unless explicitly requested.

Worker custom domain:
```text
https://board-games.seokjun.kim
```

Deployment commands generally use `npx wrangler`, for example:
```bash
cd worker
npx wrangler kv namespace create PAIRINGS
npx wrangler secret put LICHESS_CLIENT_ID
npx wrangler deploy
```

Secrets and account-specific IDs should not be committed unless they are already intentional public config. Be careful with `include/secrets.h`.

## Wi-Fi Setup

The user strongly rejected SoftAP/web portal setup.

Keep Wi-Fi setup on-device:
- Scan SSIDs
- Select SSID on display
- Enter password using LVGL textarea/keyboard
- Save credentials to Preferences

Do not add a SoftAP portal unless explicitly requested.

## UI/UX Rules

Display is small, square, and touch-only. Feedback matters.

Current chess UI conventions:
- 448px board plus 32px HUD
- Status text at bottom
- Resign button in HUD
- Lichess progress screen before blocking network calls
- Last move shown with subtle borders, not background fills
- Move hints use Lichess-style dot/ring markers

For new games:
- Keep touch targets large enough.
- Avoid tiny text.
- Prefer immediate visual feedback before blocking network calls.
- Use shared buttons from `common/ui_widgets` where possible.
- Avoid relying on audio feedback.

## Coding Guidelines

General:
- Keep behavior stable during refactors.
- Prefer small, buildable extraction steps.
- Run `/opt/homebrew/bin/pio run` after each meaningful refactor.
- Do not mix feature changes and broad refactors unless needed.
- Preserve existing visual style unless explicitly redesigning.

Memory/device constraints:
- Be careful with stack allocations.
- Large move buffers should be static/global, as already done for chess AI.
- Avoid large dynamic JSON parsing dependencies; current code intentionally uses small string helpers.

Style:
- Use ASCII for new code/comments unless existing file context requires otherwise.
- Avoid unnecessary abstraction layers on ESP32 firmware.
- Prefer plain functions and small modules over complex class hierarchies for now.

## Git Workflow

Before committing:
```bash
git status --short
git diff
git log --oneline -10
/opt/homebrew/bin/pio run
```

Commit style used so far:
```text
feat(...): ...
fix(...): ...
refactor(...): ...
chore(...): ...
```

Examples:
```text
feat(ui): highlight last move squares
feat(firmware): add AI mode and stabilize Lichess gameplay
chore(worker): migrate wrangler config to jsonc
```

Do not amend or force-push unless explicitly requested.

## Suggested Next Refactors

High-value next steps:
1. Extract chess UI into `src/games/chess/chess_ui.*`.
2. Extract chess controller/touch handling into `src/games/chess/chess_controller.*`.
3. Extract Wi-Fi setup screens into `src/network/wifi_setup.*`.
4. Extract Lichess pairing/game client into `src/lichess/`.
5. Reduce `main.cpp` to setup, loop, app navigation, and game selection.

Suggested future structure:
```text
src/app/app_state.*
src/app/app_shell.*
src/network/wifi_setup.*
src/lichess/lichess_pairing.*
src/lichess/lichess_client.*
src/games/chess/chess_ui.*
src/games/chess/chess_controller.*
src/games/janggi/...
src/games/baduk/...
src/games/shogi/...
```

## Adding Future Games

For janggi, baduk, or shogi:
- Start with local/offline play first.
- Keep game state independent from UI objects.
- Define a small move type for the game.
- Implement board rendering separately from rules.
- Use the common app menu to launch the game.
- Reuse `common/ui_widgets` for menus and dialogs.
- Add online service integration only after local play is stable.

Recommended per-game module shape:
```text
src/games/<game>/<game>_core.h
src/games/<game>/<game>_core.cpp
src/games/<game>/<game>_ui.h
src/games/<game>/<game>_ui.cpp
src/games/<game>/<game>_controller.h
src/games/<game>/<game>_controller.cpp
```

For games with AI:
```text
src/games/<game>/<game>_ai.h
src/games/<game>/<game>_ai.cpp
```

Keep each game independently testable as much as possible, even though this is embedded firmware.
