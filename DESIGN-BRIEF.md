# Project Brief — ROTR "Path C" dual-mode ZMK firmware

Paste this as the opening instruction to Claude Code, working inside a fork of MOTR.

---

## Role & goal
Build aftermarket ZMK firmware for the **Polarity Works ROTR** macropad. The defining feature is a **layer-aware dual-mode rotary encoder**: depending on the active keymap layer, the single knob behaves either as a standard ZMK **sensor** (keyboard-style functions) or as a smooth **mouse wheel** (pointing input). Build on **upstream ZMK** by forking the MOTR project. Do not change this architecture or substitute a simpler one without flagging it first.

## Hardware facts (do not re-derive)
- Board: Polarity Works ROTR
- MCU: nRF52840 (QIAA), Adafruit/UF2 bootloader (double-tap PCB reset to enter bootloader)
- Encoder: **MA730 16-bit absolute magnetic encoder** — reports absolute angle, not relative movement
- Inputs: 3 buttons (left / middle / right) + the encoder
- 12-LED RGB underglow
- Connectivity: USB + Bluetooth (BLE)

## Base to fork (attribution required)
- Fork `https://github.com/martial-cc/MOTR` (MIT, © Carl Henriksson). Reuse its MA730 driver, board definition (`boards/polarityworks/rotr`), DTS sensor bindings, and the upstream-ZMK pin in `config/west.yml`.
- Keep MOTR's MIT license and attribution intact; record our additions in the README and a short `DESIGN.md`.
- MOTR's driver already solves the absolute-encoder problem (computes wrapped deltas locally, emits `INPUT_REL_WHEEL` through Zephyr's input subsystem, clears its accumulator on a confirmed reversal). **Extend this driver — do not rewrite the angle maths.**

## Architecture — Path C, layer-aware dual mode
One physical encoder must serve two ZMK subsystems depending on the active layer:
- **Sensor-world layers** → rotation drives the keyboard sensor path (`sensor-bindings`, e.g. `&inc_dec_kp`). Layer-aware by nature.
- **Scroll-world layers** → rotation drives the pointing/input subsystem as smooth wheel events: `INPUT_REL_WHEEL` (vertical) or `INPUT_REL_HWHEEL` (horizontal), MOTR-style.
- Each poll, the driver consults the **highest active layer** and emits into the correct world per a **per-layer mode table**.
- For scroll-world, select vertical vs horizontal by layer — either via `zmk,input-listener` nodes gated with `layers = <...>`, or driver-side selection. Pick one approach and document it.

### Critical edge case (must handle)
On **any** mode change (sensor↔scroll) or layer change, **clear/reset the rotation accumulator** so a single turn cannot leak a phantom scroll tick or cause a double layer-jump across the boundary. Build this on top of MOTR's existing reversal-clearing logic.

## Layer map (exact)
Buttons on every base layer:
- Left = Copy `&kp LC(C)`
- Middle = tap **Select-All** `&kp LC(A)`; **hold = momentary selector layer**
- Right = Paste `&kp LC(V)`

| Layer | Name | Knob world | Knob function |
|------:|------|-----------|---------------|
| 0 | default | sensor | **inactive** (no rotation output) |
| 1 | brightness | sensor | F13 (CW) / F14 (CCW) — drives Twinkle Tray hotkeys |
| 2 | volume | sensor | Vol Up / Vol Down |
| 3 | scroll-v | scroll | smooth **vertical** wheel |
| 4 | scroll-h | scroll | smooth **horizontal** wheel |
| 5 | selector | sensor | layer-select (see constraint 1) |

Cycling lands on layers 0–4 (five RGB colours); layer 5 is the held selector, not a landing layer. Internal numbering may change, but the selector must be the layer the middle-button hold activates.

## Must-preserve constraints (non-negotiable)
1. **Layer switching identical to current firmware.** Hold the **middle button**, then **turn the knob** to select the active base layer; release to land. The trigger is the middle-button *hold* (a keyboard behaviour), independent of knob mode. On the selector layer the knob is in **sensor mode**, so turning selects layers exactly as today. This is reimplemented on upstream ZMK (the original `&def_lshft` / `&lt` selector was custom code in the `refil/zmk` fork) but must **behave and feel identical**.
2. **Per-layer RGB colour indicator.** Each landing layer (0–4) shows a distinct underglow colour, as the stock ROTR did. Reimplement via a layer-state-changed listener. Keep the RGB on/off toggle on the selector layer's right button (`&rgb_ug RGB_TOG`).
3. **Fallback safety.** Never break bootloader entry (double-tap PCB reset) or reflashing. USB HID output must work with no Bluetooth pairing present (default to USB output).
4. Copy / Select-All / Paste preserved on all base-layer buttons.

## Workflow — GitHub Actions only (no local toolchain)
The user will **not** run local builds. Drive the full loop with the GitHub CLI (`gh`):
1. Make coherent multi-file edits in the fork.
2. Commit and push.
3. Trigger the build workflow and watch it (`gh workflow run …`, `gh run watch …`).
4. On failure, read the Actions logs, fix the C/DTS/Kconfig, repeat.
5. On success, download the `.uf2` artifact and place it in an obvious path for the user.
6. **Stop and hand off to the user to flash and verify on hardware.** You cannot flash the device or feel the result. Wait for the user's hardware report before continuing past a stage gate.

## Staged plan (flash + verify at each gate)
- **Stage 1 — prove the base.** Forked MOTR building on Actions with the 3 real buttons (Copy / Select-All / Paste) + a single smooth **vertical**-scroll layer. No multi-layer/selector/RGB yet. *Gate:* builds, flashes, buttons and smooth scroll confirmed on hardware.
- **Stage 2 — dual-mode + full layer map.** Add layer-aware mode switching and the full table (brightness, volume, scroll-v, scroll-h). *Gate:* each layer's knob does the right thing; no phantom ticks when crossing layers.
- **Stage 3 — selector + RGB + edge cases.** Reimplement the hold-middle-turn selector (identical feel) and per-layer RGB colours; finalise accumulator reset on mode/layer change. *Gate:* switching feels identical to stock; clean transitions both directions.

## Output expectations
- Keep diffs reviewable. Explain each custom-C addition in plain language in commit messages and `DESIGN.md`.
- Do **not** silently change the architecture (no reverting to MOTR's mouse-only model, no inventing a different layer scheme or selector gesture).
- Where a behaviour depends on hardware you can't test, **state the assumption explicitly and flag it for the user's hardware test** rather than guessing.
