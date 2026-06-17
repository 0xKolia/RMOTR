# CLAUDE.md

Project context for AI coding sessions. Keep this in sync when the
architecture changes; `DESIGN.md` is the long-form design record.

## What this is

**ROTR "Path C"** — aftermarket ZMK firmware for the Polarity Works **ROTR**
(a single-knob, three-button nRF52840 macropad with WS2812 underglow). It is a
fork of [MOTR](https://github.com/martial-cc/MOTR) (MIT, © Carl Henriksson);
MOTR's MA730 magnetic-encoder driver, board definition, DTS bindings and the
pinned upstream ZMK are reused. Everything under "Path C" below is ours.

## Hardware

- Polarity Works ROTR, nRF52840 (Adafruit nRF52 UF2 bootloader).
- One **MA730** magnetic rotary encoder (absolute angle over SPI).
- 1×3 buttons: LEFT, MIDDLE, RIGHT.
- WS2812 underglow strip, powered through the **gpio1.11 ext-power rail**.
- USB + BLE.

## Path C architecture

One physical MA730 encoder feeds several ZMK subsystems. The driver
(`drivers/sensor/ma730/ma730_input.c`) is the **world-selector**: every poll
(~8 ms) it reads `zmk_keymap_highest_layer_active()` and routes rotation into
the "world" named by that layer in the devicetree `layer-modes` table. MOTR's
absolute-angle maths is preserved; the accumulator resets on every layer
change so a turn crossing a mode boundary can't leak a phantom tick.

Knob modes (`boards/polarityworks/rotr/rotr-ma730.h`): `INACTIVE`, `KEY`
(taps encoded keycodes CW/CCW), `SCROLL_V`, `SCROLL_H`, `SELECT`.

**Activating a reserved layer is a devicetree edit, never a C change** — set
its `layer-modes` entry and (for KEY layers) its keycodes, then raise
`select-layer-count`. See DESIGN.md → "Activating a reserved layer".

## Layer map

| Layer | Name       | Knob mode  | RGB colour |
|------:|------------|------------|------------|
| 0 | default    | inactive   | White |
| 1 | brightness | key (F13/F14) | Amber |
| 2 | arrows     | key (Right/Left) | Green |
| 3 | scroll-h   | scroll-h   | Cyan |
| 4 | scroll-v   | scroll-v   | Magenta (H320 S100 B80) |
| 5–8 | reserved (5 = pre-loaded Volume) | varies | Orange/Blue/Teal/Purple |
| 9 | selector   | select     | (never shown) |

Buttons on every active layer (1–8 fall through via `&trans`): LEFT = Copy
`LC(C)`, MIDDLE = Select-All `LC(A)` / **hold** = selector, RIGHT = Paste
`LC(V)`. On the selector layer: LEFT = `&bt BT_CLR`, RIGHT = `&rgb_ug
RGB_TOG`.

**Selector gesture:** MIDDLE is a hold-tap (`&lt_sel`, hold-preferred,
200 ms). Tap = Select-All; hold = momentary selector layer 9, turning cycles
the candidate base layer, release lands it.

## RGB toggle model (`src/rgb_indicator.c`)

A `zmk_layer_state_changed` listener shows the current layer colour, where
"current layer" excludes the held selector (so a held selector previews the
candidate). It's a clean toggle (HOLD MIDDLE + TAP RIGHT, persisted by ZMK):

- **ON:** current colour lit **continuously**, at rest and at idle
  (`CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE=n`). ZMK's tick renders it.
- **OFF:** **dark at rest.** Lights only while MIDDLE is **held** — candidate
  colour **solid** for the whole hold, dark on release. No flashing.
- Off-state illumination is written straight to the strip
  (`led_strip_update_rgb`) and never flips ZMK's on/off state.
- The ext-power rail is **decoupled** from RGB on/off (defconfig deliberately
  sets `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER=n`); this module brings the rail up
  once at boot and keeps it powered. This was the dead-underglow fix — see
  DESIGN.md.

## Key files

| Path | What |
|------|------|
| `drivers/sensor/ma730/ma730_input.c` | encoder driver + world-selector |
| `src/rgb_indicator.c` | per-layer RGB + ext-power management |
| `boards/polarityworks/rotr/rotr-ma730.h` | shared mode/layer constants (DT + C) |
| `boards/polarityworks/rotr/rotr_nrf52840_zmk.dts` | `ma730` node: `layer-modes`, keycodes, tunables |
| `boards/polarityworks/rotr/rotr.keymap` | keymap + `&lt_sel` hold-tap |
| `boards/polarityworks/rotr/rotr_nrf52840_zmk_defconfig` | RGB/ext-power/USB/BLE config |
| `config/west.yml` | **pinned** ZMK revision (internal APIs, not stable ABI) |
| `build.yaml` | board + artifact name (`ROTR`) |
| `DESIGN.md` | full design notes, tunables, rationale |

## ZMK pin caveat

This firmware calls ZMK **application** APIs that are not a stable public ABI
(`zmk_keymap_*`, `zmk_rgb_underglow_*`, `raise_zmk_keycode_state_changed_from_encoded`).
ZMK is therefore pinned in `config/west.yml`; bumping the pin may require
adjusting these calls. Assumes `CONFIG_ZMK_KEYMAP_LAYER_REORDERING=off` so
layer index == layer id.

## Building (no local toolchain — build via GitHub Actions)

`.github/workflows/build.yml` reuses ZMK's `build-user-config.yml`. To build:

1. Push to a branch (or trigger **Actions → Run workflow**).
2. Wait for the run; download the `ROTR.uf2` artifact, **or** for a tagged
   release the `.uf2` is attached to the GitHub release.

`gh` CLI works for triggering runs and downloading artifacts:
`gh run watch`, `gh run download <id> -n ROTR`.

## Flashing

Double-press reset to enter the bootloader → ROTR mounts as USB mass storage
→ copy `ROTR.uf2` → it reboots automatically.
