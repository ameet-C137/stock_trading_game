# Stock Tycoon — Flipper Zero Game

A fast-paced stock trading game built for the Flipper Zero. Random-walk price
simulation with market events (crashes/booms), a live auto-scaling price
chart, buy/sell with selectable quantity (1 / 3 / 5 shares), floating
+/- cash popups, haptic + LED feedback on trades, a toggleable chiptune
background loop, difficulty settings, and a persistent high score.
Credits ("ameet") are shown bottom-right on the splash screen.

## Files
- `application.fam` — the app manifest (name, category, entry point, icon)
- `stock_tycoon.c` — the entire game (single file, ~1000 lines)
- `icon.png` — 10x10 app icon shown in the Flipper's app menu

## Important note on how this was built

I don't have network access to Flipper's SDK servers from this sandbox
(`update.flipperzero.one` is blocked), so I could not run the actual
Flipper build toolchain here to produce a ready-to-flash `.fap` file.

To compensate, I downloaded the **real, current Flipper firmware source**
from GitHub and diffed every API call in this file (canvas drawing,
input handling, timers, mutexes, message queues, the speaker HAL, and the
storage HAL) against the actual headers to make sure signatures, enum
names, and macros are correct. I also ran the file through a strict C
syntax check (`-Wall -Wextra -Wshadow`) against stub headers that mirror
the real ones, and it compiled with zero warnings. So the code should build
cleanly, but since I couldn't produce the final `.fap` binary myself, please
follow the build steps below (they take about 5 minutes) and let me know
if you hit any errors — paste them back to me and I'll fix them immediately.

## How to build the .fap file

You have three options. Option 1 is easiest.

### Option 1: ufbt (recommended, ~5 minutes)

`ufbt` ("micro Flipper Build Tool") is a small Python tool that downloads
the official SDK and compiles your app into a `.fap` you can copy to the
Flipper.

1. Install Python 3.8+ if you don't have it.
2. Install ufbt:
   ```
   pip install ufbt
   ```
3. Put `application.fam`, `stock_tycoon.c`, and `icon.png` in a folder,
   e.g. `stock_tycoon/`.
4. Open a terminal in that folder and run:
   ```
   ufbt update
   ufbt build
   ```
   The first `ufbt update` downloads the SDK (needs internet access to
   `update.flipperzero.one` and GitHub — this is what my sandbox couldn't
   reach). `ufbt build` compiles the app.
5. Your compiled game will be at:
   ```
   dist/stock_tycoon.fap
   ```
6. Copy that `.fap` to your Flipper. Two ways:
   - Plug the Flipper in by USB and run `ufbt launch` — it builds, copies
     to the Flipper, and launches the app immediately.
   - Or connect with **qFlipper** (Anthropic doesn't make qFlipper — it's
     Flipper Devices' official desktop app, download from
     flipperzero.one) and drag `stock_tycoon.fap` into
     `SD Card / apps / Games /` on the Flipper's file browser.

### Option 2: VS Code + the official Flipper extension

1. Install VS Code.
2. Install the "Flipper Zero" extension from Flipper Devices (or clone
   the `flipperzero-firmware` repo and open it in VS Code, which prompts
   you to set up the dev environment via `./fbt`).
3. Drop this app's folder into `applications_user/stock_tycoon/` inside
   the firmware repo.
4. Run the build task (or `./fbt fap_stock_tycoon` from a terminal in the
   repo root).
5. The `.fap` appears under `build/f7-firmware-D/.extapps/stock_tycoon.fap`.

### Option 3: Flipper mobile app ("Lab" / App catalog build)

The official Flipper mobile app can install `.fap` files you already
have but doesn't compile C source from scratch — so you'll still need
Option 1 or 2 to produce the `.fap` first, then you can transfer it via
Bluetooth from the mobile app, or via qFlipper/SD card as above.

## Installing the built .fap on your Flipper

Once you have `stock_tycoon.fap`:

1. **Via qFlipper**: connect Flipper by USB, open qFlipper, open the
   file browser, navigate to `SD Card/apps/Games/`, and drag the `.fap`
   in.
2. **Via SD card directly**: pull the microSD card, copy the `.fap` to
   `apps/Games/` on it, reinsert.
3. **Via ufbt**: `ufbt launch` does the copy + launch for you over USB.
4. On the Flipper: `Menu → Apps → Games → Stock Tycoon`.

## Controls

- **Menu**: Up/Down to move, OK to select, Back to exit
- **In-game**: Right = open Buy menu, Left = open Sell menu, Back = quit
  (with confirmation)
- **Buy/Sell screen**: Left/Right to pick quantity (1 / 3 / 5), OK to
  confirm, Back to cancel
- **Settings**: Up/Down to pick a row, OK to toggle Sound or cycle
  Difficulty (Easy/Normal/Hard — controls price volatility)

## If the build fails

Paste me the exact error text and I'll patch the source. The most likely
source of an issue, if any, is a small API drift in whatever exact
firmware version your ufbt SDK resolves to (Flipper's SDK does evolve),
since I verified against the `dev` branch but you may build against a
tagged release. These are typically one- or two-line fixes.

— Game by ameet
