# Vinyl Vault Screen: working agreement

Rules for anyone (human or Claude) changing this repo. Read before touching code. The same
agreement as [Vinyl Value Vault](https://github.com/JakubMini/vinyl-value-vault), adapted to a
board and a small Worker.

## Branches, commits, pull requests

- **Never commit to main.** Every piece of work, however small, gets its own branch cut from an
  up-to-date main and lands through a pull request.
- Branch names say what the branch is for: `feat/<thing>`, `fix/<thing>`, `chore/<thing>`, `docs/<thing>`.
- Start every task with: `git switch main && git pull --ff-only && git switch -c <branch>`.
- One PR, one purpose. Commit subjects are imperative and under 72 characters.
- Before pushing: `npm run check` in `gateway/`, and `pio test -e native` plus `pio run -e screen`
  in `firmware/`. CI runs the same.
- The PR body says what changed, how it was verified (on the board, if it is firmware), and
  whether the README needed updating.

## The README is for humans

- Written for someone deciding in two minutes whether this project is worth their attention.
  Keep it true: any PR that changes the architecture, the login, the sync, the setup steps or the
  roadmap updates the README in the same PR. Do not inflate.

## Cloudflare account: check it first

- The gateway lives only on the **personal account, jakub.m.szypicyn@gmail.com**, next to the
  vault. The work account must never be used. Before `wrangler deploy`, `tail` or `secret`, run
  `npx wrangler whoami` in `gateway/` and confirm the account. If it is wrong, stop and ask Jakub
  to switch. Never log in on his behalf.
- The gateway must stay public (no Access) and must only ever forward its short allowlist.

## Where things live

- `gateway/src/index.ts`: the public Worker. An allowlist of five routes, forwarded to the vault
  over a service binding, passing only `vvd_` device tokens. Tests in `gateway/test/`.
- `firmware/` (PlatformIO, Arduino core 3, LVGL 9):
  - `src/board.cpp`: panel, touch, LVGL glue. Pins and the ST7701 init live here only.
  - `src/vault.cpp`: Wi-Fi (remembered in NVS), the device login, gateway calls.
  - `src/sync.cpp`: the background task that keeps the board's copy fresh, and makes covers.
  - `src/store.cpp`: the copy on LittleFS. Write only what changed: flash writes stall the panel.
  - `src/covers.cpp`: cover JPEGs held in PSRAM, decoded on a background task.
  - `src/nfc.cpp`: the PN532 on the relay header (UART on IO40/IO2, reset on IO1), polled on core 0.
    Not I2C: there it refused every poll while busy, a check took seconds, and an ESP32 restart
    mid-transfer left it holding SCL low until it lost power.
  - `src/ui.cpp`: the screens. The wheel draws itself straight into the panel; LVGL does the rest.
  - `include/wheel.h`: the wheel's geometry, pure and tested on the laptop (`test/test_wheel`).
  - `include/config.h`: the gateway URL. Nothing secret lives in the firmware.
- Logs are JSON lines on the serial port: `{"event":"..."}`.

## On the board

- The board is an ESP32-4848S040 on a CH340 serial port (`/dev/cu.usbserial-*`).
- Flash and watch: `pio run -e screen -t upload -t monitor`.
- Never block the UI core on the network. Sync and covers run on core 0; core 1 is LVGL's.
- Don't touch flash while the wheel is on screen. Don't let a loop on core 0 run for seconds
  without yielding: the task watchdog resets the board.
- Measure before and after a performance change: a `-DWHEEL_BENCH` build spins the wheel by
  itself for 12 s and logs `{"event":"profile",...}` every second. A `-DGRADE_BENCH` build opens a
  record and saves its sleeve grade as it already is, four times (one during a sync), and logs
  `{"event":"grade_saved",...,"ms":...}` from tap to answer.
