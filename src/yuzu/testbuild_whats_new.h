// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// Shown once, on the first start of this chuppatricky-amd-testbuild version (Markdown).
inline constexpr const char* TestbuildWhatsNew = R"(**Very experimental.** This is an unofficial test build of Lichii for Ocarina of Time. Don't expect better performance than earlier versions, and expect bugs.

## What's new

- **Updates in the app:** Lichii checks for new versions when it starts and asks whether to update or stay.
- **30/60 FPS lock:** new button next to FAST/ACCURATE in the bottom bar.
- **Frame generation:** smoother when moving the camera (no more double image).
- **Fixed:** missing grass and plants.
- **Fixed:** fire trailing behind a lit Deku Stick.
- **Fixed:** flickering or white textures with GPU Clocks on Overclock.
- **Fixed:** AMD on Windows closing while building shaders (not confirmed yet, please report).
- **Fewer stutters** from shaders being rebuilt.
- **Windows:** runs on any 64-bit CPU (no AVX2 needed), and the first boot no longer looks frozen.
- **New start screen** with the game's cover and a loading bar.

## If the picture flickers or turns white

Set **GPU Clocks** to **Boost**, turn off **Force maximum clocks**, and set **GPU Fence Behavior** to **Default** or **Balanced**.

## Credits

zortos293 and the Lichii contributors (Frizti, Dr. Sergio, Jonas, nyakuoff, Duck912, 999sian, Kajcsi, kraibse, Zarigis, Franseven), Dark (Dynamic FPS), Nemyu / Uesleizito Zin, Erick, mja00 (Matt Artist), sharkiix, and everyone who sent logs and saves.
)";
