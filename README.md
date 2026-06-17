# ROTR — Path C

Aftermarket ZMK firmware for the Polarity Works **ROTR** macropad.

This is the **"Path C"** fork: one MA730 magnetic encoder driving several
layer-aware "worlds" (key taps, smooth scroll, a hold-to-select layer
selector) with a per-layer RGB underglow indicator. It is built on top of
[**MOTR**](https://github.com/martial-cc/MOTR) by Carl Henriksson (MIT) — MOTR's
MA730 driver, board definition, DTS bindings and the pinned upstream ZMK are
reused; the layer-selector architecture, per-layer RGB toggle model and
ext-power handling are added here.

This is an independent project and is not affiliated with Polarity Works.

## Features

- One knob, many modes — its behaviour follows the active layer:
  - **brightness** (F13/F14), **arrows** (Right/Left), **horizontal scroll**,
    **vertical scroll**, plus reserved layers (layer 5 pre-loaded as Volume).
- **Hold-MIDDLE layer selector:** hold the middle button and turn to pick a
  layer; release to land on it. A quick tap is Select-All.
- Buttons are Copy / Select-All / Paste on every active layer.
- **Per-layer RGB underglow** with a clean on/off toggle (hold MIDDLE + tap
  RIGHT): ON = the layer colour stays lit continuously; OFF = dark, lighting
  the candidate colour only while the selector is held.
- USB + Bluetooth, battery reporting.

See [`DESIGN.md`](DESIGN.md) for the full architecture and
[`CLAUDE.md`](CLAUDE.md) for a quick project map.

## Hardware

- Polarity Works — ROTR (nRF52840, MA730 encoder, WS2812 underglow).

## Download

The latest firmware is attached to the
[releases](https://github.com/0xKolia/RMOTR/releases) page as `ROTR.uf2`.

## Building

No local toolchain needed — firmware is built by GitHub Actions:

1. Fork this repository.
2. Edit the board files (e.g.
   `boards/polarityworks/rotr/rotr.keymap` or the `ma730` node in
   `rotr_nrf52840_zmk.dts`) to change behaviour.
3. Open the **Actions** tab → **Run workflow** (or push a commit).
4. When the run completes, download the `ROTR.uf2` artifact from it.

## Flashing

1. Double-press the reset button on the PCB to enter the bootloader.
2. The device appears as a USB mass storage device.
3. Copy `ROTR.uf2` onto it.
4. The device restarts automatically.

## Credits & License

Based on [MOTR](https://github.com/martial-cc/MOTR) by **Carl Henriksson**.

MIT. Copyright (c) 2026 Carl Henriksson.
