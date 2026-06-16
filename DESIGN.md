# ROTR "Path C" — design notes

Aftermarket ZMK firmware for the Polarity Works ROTR, forked from
[MOTR](https://github.com/martial-cc/MOTR) (MIT, © Carl Henriksson). MOTR's
MA730 driver, board definition, DTS bindings and the upstream-ZMK pin are
reused; the additions below are ours.

## Stage 1 — base
- Three buttons on a single layer: Copy `LC(C)`, Select-All `LC(A)`,
  Paste `LC(V)`.
- Knob = MOTR's smooth vertical mouse wheel (`INPUT_REL_WHEEL` via the input
  subsystem and an `zmk,input-listener`).

## Stage 2 — layer-aware dual-mode knob

### Where the layer awareness lives
The MA730 driver (`drivers/sensor/ma730/ma730_input.c`) is the world-selector.
On every poll (~8 ms) it reads `zmk_keymap_highest_layer_active()` and looks
the index up in an in-driver mode table:

| Layer | Name       | World    | Knob output                          |
|------:|------------|----------|--------------------------------------|
| 0     | default    | inactive | nothing                              |
| 1     | brightness | keyboard | tap **F13** (CW) / **F14** (CCW)     |
| 2     | volume     | keyboard | tap **Vol Up** (CW) / **Vol Dn** (CCW) |
| 3     | scroll-v   | pointing | smooth `INPUT_REL_WHEEL` (vertical)  |
| 4     | scroll-h   | pointing | smooth `INPUT_REL_HWHEEL` (horizontal) |

The vertical/horizontal choice is made **driver-side** (the brief allows this
or layer-gated listeners); doing it in the driver keeps a single input-listener
and one scaler for both axes.

### Keyboard world — how a turn becomes a key tap
ZMK's normal sensor path uses `sensor-bindings` (e.g. `&inc_dec_kp`). We instead
raise keycode events directly from the driver with
`raise_zmk_keycode_state_changed_from_encoded(<encoded keycode>, pressed, ts)`,
where the encoded keycode is the same value `&kp` uses (`F13`, `C_VOL_UP`, …).

**Why this deviates from the brief's `&inc_dec_kp` suggestion:** making the
MA730 a *dual* Zephyr-sensor + input device in one driver is significantly more
code and version-sensitive. Raising keycode events is self-contained, produces
HID output identical to `&kp`, and keeps the per-layer routing in one place.
The key identities are easy to change in the driver's `ma730_layer_modes`
table; this can be migrated to `sensor-bindings` later if desired.

Tap timing: a press and its release are queued in a small FIFO and drained
**one entry per poll**, so each transition lands on a separate HID report and
the host never coalesces a tap into nothing. All queue access happens on the
single poll execution context, so it needs no locking.

### Pointing world — smoothness
The MA730 is a 16-bit absolute encoder (65536 counts/rev). The driver meters
counts into fine wheel units at `scroll-counts-per-unit` (default 64 →
~1024 units/rev), tracking the remainder so no motion is lost. The board's
`zip_scroll_scaler` then divides by the listener divisor (default **16**,
in `rotr_nrf52840_zmk.dts`) with `track-remainders`, giving ~64 notches/rev.
**Tune feel by changing that 16** (higher = slower).

Velocity-based acceleration is applied in the driver: the per-poll speed is
multiplied by `scroll-accel-gain` (default 6) and added to a 256-based unity
factor, clamped at `scroll-accel-max` (default 1024 = 4×). Acceleration scales
the *magnitude*, not the event rate, so the HID report rate stays bounded by
the poll interval. Set `scroll-accel-gain = <0>` for linear scrolling.

Direction defaults: CW scrolls **down** and pans **right**. Flip with
`invert-scroll` / `invert-hscroll` on the `ma730` node.

### Critical edge case — accumulator reset
On any layer change (hence any mode change) the driver discards the straddling
delta, zeroes the accumulator and re-seeds the last angle, so a turn crossing a
keyboard↔scroll boundary cannot leak a phantom tick. This builds on MOTR's
existing reversal-clearing logic, which is preserved untouched.

### Temporary layer switching (Stage 2 testing only)
The real hold-middle-turn selector is Stage 3. For now, combos toggle layers
while single presses stay Copy/Select-All/Paste:

- LEFT + MIDDLE → toggle layer 1 (brightness)
- MIDDLE + RIGHT → toggle layer 2 (volume)
- LEFT + RIGHT → toggle layer 3 (scroll-v)
- LEFT + MIDDLE + RIGHT → toggle layer 4 (scroll-h)

Layer 0 is the state with nothing toggled. Toggle one layer on, test, toggle it
off before moving on.

### Tunables summary
| Setting | Location | Default | Effect |
|---------|----------|--------:|--------|
| scroll divisor | `ma730-listener` `zip_scroll_scaler 1 N` (DTS) | 16 | main scroll speed; higher = slower |
| `scroll-counts-per-unit` | `ma730` node (DTS) | 64 | fine resolution before scaler |
| `scroll-accel-gain` | `ma730` node (DTS) | 6 | acceleration strength; 0 = off |
| `scroll-accel-max` | `ma730` node (DTS) | 1024 | acceleration cap (256 = 1×) |
| `key-counts-per-detent` | `ma730` node (DTS) | 2731 | detents/rev for brightness/volume |
| `invert-scroll` / `invert-hscroll` | `ma730` node (DTS) | off | flip wheel direction |
