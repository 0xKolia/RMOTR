# ROTR "Path C" — design notes

Aftermarket ZMK firmware for the Polarity Works ROTR, forked from
[MOTR](https://github.com/martial-cc/MOTR) (MIT, © Carl Henriksson). MOTR's
MA730 driver, board definition, DTS bindings and the upstream-ZMK pin are
reused; the additions below are ours.

## Architecture summary
One physical MA730 encoder serves several ZMK subsystems. The driver
(`drivers/sensor/ma730/ma730_input.c`) is the world-selector: on every poll
(~8 ms) it reads `zmk_keymap_highest_layer_active()` and routes rotation into
the world named by that layer in the devicetree `layer-modes` table. MOTR's
absolute-angle maths (wrapped delta + reversal clearing) is preserved
untouched; the accumulator is reset on every layer change so a turn crossing a
mode boundary cannot leak a phantom tick.

## Stage 3 layer map (current)

| Layer | Name       | Mode (DT)            | Knob output                      | RGB colour      |
|------:|------------|----------------------|----------------------------------|-----------------|
| 0     | default    | `MA730_MODE_INACTIVE`| nothing (resting state)          | White           |
| 1     | brightness | `MA730_MODE_KEY`     | F13 (CW) / F14 (CCW)             | Amber/Yellow    |
| 2     | arrows     | `MA730_MODE_KEY`     | Right (CW) / Left (CCW)         | Green           |
| 3     | scroll-h   | `MA730_MODE_SCROLL_H`| smooth horizontal wheel (CW=right)| Cyan           |
| 4     | scroll-v   | `MA730_MODE_SCROLL_V`| smooth vertical wheel (CW=down)  | Magenta         |
| 5     | reserved   | `MA730_MODE_KEY`     | (pre-loaded Vol Up/Down, dormant)| Orange          |
| 6     | reserved   | `MA730_MODE_INACTIVE`| nothing                          | Blue            |
| 7     | reserved   | `MA730_MODE_INACTIVE`| nothing                          | Teal            |
| 8     | reserved   | `MA730_MODE_INACTIVE`| nothing                          | Purple          |
| 9     | selector   | `MA730_MODE_SELECT`  | cycles base layers (held)        | (never shown)   |

Buttons on every active layer (fall through via `&trans`): LEFT = Copy
`LC(C)`, MIDDLE = Select-All `LC(A)` / hold = selector, RIGHT = Paste `LC(V)`.

### Exact RGB colours (HSB: hue 0-360, sat 0-100, brightness 0-100)
Set in `src/rgb_indicator.c` `layer_colors[]`:

| Layer | Colour          | H   | S   | B  |
|------:|-----------------|----:|----:|---:|
| 0     | White           | 0   | 0   | 80 |
| 1     | Amber/Yellow    | 40  | 100 | 80 |
| 2     | Green           | 120 | 100 | 80 |
| 3     | Cyan            | 180 | 100 | 80 |
| 4     | Magenta         | 320 | 100 | 80 |
| 5     | Orange (resv)   | 25  | 100 | 80 |
| 6     | Blue (resv)     | 225 | 100 | 80 |
| 7     | Teal (resv)     | 165 | 100 | 70 |
| 8     | Purple (resv)   | 280 | 100 | 80 |

Pure red (H 0, S 100) is reserved for future alert use; blue (225) is kept
clear of cyan (180).

## The selector (Stage 3, replaces the Stage 2 combo harness)
- MIDDLE is a hold-tap (`&lt_sel`, `zmk,behavior-hold-tap`,
  `flavor = "hold-preferred"`, `tapping-term-ms = <200>`): **tap** = Select-All,
  **hold** = momentary selector layer (9).
- While the selector layer is held it is the highest active layer, so the
  driver is in `MA730_MODE_SELECT`. Turning advances the candidate **base**
  layer (`zmk_keymap_layer_activate`/`deactivate` on layers 1..count-1, leaving
  the default layer 0 always active). The selector layer itself stays active
  throughout, so the highest layer never changes mid-selection.
- **Releasing MIDDLE** drops the momentary selector layer, leaving exactly the
  candidate base layer active — that is the landed layer.
- Selector resolution is **separate** from scroll sensitivity and never uses
  `zip_scroll_scaler`. `select-counts-per-step = <8192>` ≈ 8 steps/rev: one
  deliberate layer per nudge. CW advances to the next layer (wraps).
- Selector-layer buttons: LEFT = `&bt BT_CLR`, MIDDLE = `&trans` (the hold),
  RIGHT = `&rgb_ug RGB_TOG`.

## RGB behaviour (`src/rgb_indicator.c`) — ported from the original ROTR
This reproduces the stock Polarity Works ROTR underglow behaviour. The
desired model:

- **Toggle ON** (HOLD MIDDLE + TAP RIGHT = `RGB_TOG`, persisted): the active
  layer's colour is lit **perpetually** — at rest, while selecting, and at
  idle.
- **Toggle OFF:** **dark at rest**; lit **only while MIDDLE is held**
  (selection mode), showing the candidate colour SOLID and updating as the
  knob turns, then **dark the instant MIDDLE is released** and the layer is
  landed.

### What the original did (refil/zmk @ `rotrlayer`, `app/boards/arm/rotr2`)
The per-layer logic lived in that fork's **modified `app/src/rgb_underglow.c`**,
not in the board dir:

- A custom effect `UNDERGLOW_EFFECT_DEFAULTLAYER` (selected via
  `EFF_START=4`) keyed the strip colour to **`zmk_keymap_layer_default()`** —
  the persistent base layer chosen by the shift selector, so the landed layer
  stays shown after release.
- Crucially, the fork **commented out the `k_timer_start`/`k_timer_stop` in
  `on()`/`off()`** and started the tick unconditionally at init, so the **50 ms
  tick runs forever** and is the sole writer of the strip every frame. On/off
  is just a `state.on` flag the tick reads (`if (state.on)` fills colour, else
  leaves it dark). This always-running, self-owned render is why it never
  misses a transition.
- An optional `CONFIG_ZMK_RGB_UNDERGLOW_LAYER_ON` block force-turned the
  underglow on while the `"shift"` layer was the highest active and off
  otherwise — i.e. the "lit only while selecting" half of the model.

### How the port maps onto current (pinned) ZMK
Current upstream ZMK differs: `on()`/`off()` **do** start/stop the tick, the
tick early-returns `if (!state.on)`, `set_hsb` is cheap (no NVS write), and
idle auto-off is fully `#if`-compiled by `AUTO_OFF_IDLE`. We can't edit ZMK
core (out-of-tree, pinned), so we recreate the original's *always-running tick*
in `src/rgb_indicator.c` with our **own 50 ms `k_timer`** (`ROTR_TICK_MS`),
plus the `zmk_layer_state_changed` listener for instant response. Each frame
`render()`:

- **"Current layer"** = highest active layer *excluding* the selector layer —
  our analogue of the original's `layer_default` (the candidate while held,
  the landed layer after release).
- **Toggle ON:** ZMK's own tick is running, so we just keep its colour current
  via `set_hsb` and let ZMK render it solid — a single writer. Perpetual at
  idle because `CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE=n` compiles out ZMK's
  idle auto-off.
- **Toggle OFF:** ZMK's tick is stopped, so we are the sole writer. We fill
  the candidate colour SOLID while the selector is held and black otherwise —
  written **straight to the strip** (`led_strip_update_rgb`), re-asserted every
  frame, never touching ZMK's on/off state.
- `BRT_MIN/MAX` are pinned to `0/100` (identity scaling) so the ON render
  (ZMK) and the OFF-held fill (ours) are the **same colour**.

This fixes the two regressions of the previous event-only build: OFF no longer
goes blind during selection (the tick re-asserts the candidate continuously),
and ON no longer goes dark on release (`set_hsb` is refreshed every frame and
ZMK renders it perpetually).

- Boot: a one-shot work 1.5 s after start brings up the ext-power rail, does
  one `render()`, then starts the periodic tick. Boot colour is white via
  `CONFIG_ZMK_RGB_UNDERGLOW_SAT_START=0`.

### Root cause of the dead-underglow bug (and the fix)
The LED strip is powered through the **gpio1.11 ext-power rail**. With
`CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER`, ZMK only enables that rail inside
`zmk_rgb_underglow_on()` — but the `ON_START` boot path just starts the
animation tick and **never calls `on()`**, and the rail's default-on state is
overridden by a stale persisted "off" in NVS. So the tick rendered onto an
**unpowered strip** and stayed dark in every state, on every layer. A
direct-drive test that forced the rail on (`ext_power_enable`) and wrote the
strip directly lit it perfectly — proving the strip/SPI/pinctrl/power path is
fine and the fault was purely this power-gating. (The merged `zephyr.dts` and
`.config` for `led_strip`/`ws2812`/`spi`/pinctrl were byte-identical to the
known-good original ROTR firmware, which is why the config/DT layer was ruled
out.)

**Fix:** the rail is **decoupled** from RGB on/off — the board defconfig does
**not** set `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER`. `src/rgb_indicator.c` (the
owner of the RGB subsystem) enables the rail once at startup, after settings
load, and keeps it powered. RGB on/off now only gates colour/animation.

### RGB-off rail choice (per the build brief's question)
On "off" the strip **just goes dark; the ext-power rail stays powered.** I
chose this over cutting the rail because cutting it would (a) re-introduce the
mid-toggle edge case and (b) add the rail's enable latency to every off-state
selector illumination. The cost is a few mA for the (unlit) strip rail while
RGB is off — acceptable on a USB/BLE macropad, and it makes the off-state
selector illumination instant and glitch-free. The earlier toggle edge case
is gone: the direct writes never change ZMK's on/off state, so `RGB_TOG`
always does the expected thing.

## Activating a reserved layer (5–8) — devicetree only, no C change
In `boards/polarityworks/rotr/rotr_nrf52840_zmk.dts`, `ma730` node:
1. Set that layer's entry in `layer-modes` to the desired `MA730_MODE_*`.
2. If it's a `MA730_MODE_KEY` layer, set its `layer-keycodes-cw` and
   `layer-keycodes-ccw` entries (encoded keycodes, e.g. `C_VOL_UP`).
3. Raise `select-layer-count` so the selector cycles up to it (e.g. `<6>` to
   reach layer 5). This is **the single value** that gates reserved layers.
4. (Optional) its RGB colour is already pre-assigned in `layer_colors[]`.

Layer 5 is pre-loaded as Volume (Vol Up CW / Vol Down CCW), so enabling it is
just step 3: `select-layer-count = <6>`.

## Tunables summary
| Setting | Location | Default | Effect |
|---------|----------|--------:|--------|
| scroll divisor | `ma730-listener` `zip_scroll_scaler 1 N` (DTS) | 32 | scroll speed; higher = slower (both axes) |
| `select-counts-per-step` | `ma730` node (DTS) | 8192 | selector coarseness (~8/rev) |
| `select-layer-count` | `ma730` node (DTS) | 5 | layers the selector cycles |
| `tapping-term-ms` | `lt_sel` behaviour (keymap) | 200 | hold-vs-tap threshold for the selector |
| `scroll-counts-per-unit` | `ma730` node (DTS) | 64 | fine resolution before scaler |
| `scroll-accel-gain` / `-max` | `ma730` node (DTS) | 6 / 1024 | velocity accel; gain 0 = off |
| `key-counts-per-detent` | `ma730` node (DTS) | 2731 | detents/rev for KEY layers |
| `invert-scroll` / `invert-hscroll` | `ma730` node (DTS) | off | flip wheel direction |
| `CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE` | board defconfig | n | must stay `n` for perpetual-on at idle |

## ZMK-internal dependency (pinned in `config/west.yml`)
This firmware calls ZMK application APIs that are not a stable public ABI:
- `zmk_keymap_highest_layer_active()`, `zmk_keymap_layer_active()`,
  `zmk_keymap_layer_activate()`, `zmk_keymap_layer_deactivate()`
  (`zmk/keymap.h`)
- `raise_zmk_keycode_state_changed_from_encoded()`
  (`zmk/events/keycode_state_changed.h`) — used for the keyboard-world taps
  (a deliberate, documented deviation from `&inc_dec_kp` sensor-bindings;
  far less code than a dual sensor+input device, identical HID output).
- `zmk_rgb_underglow_*()` (`zmk/rgb_underglow.h`)
- ZMK assumption: `CONFIG_ZMK_KEYMAP_LAYER_REORDERING` is **off**, so layer
  index == layer id (the driver uses the index from
  `zmk_keymap_highest_layer_active()` directly as a layer id).

Because these are internal, **ZMK is pinned to a specific revision** in
`config/west.yml`. Bumping that pin may require adjusting these calls. The
build also adds ZMK's private `app/include` and the board directory to the
out-of-tree library include paths (see the two `CMakeLists.txt`).
