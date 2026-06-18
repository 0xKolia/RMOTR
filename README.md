# RMOTR

Custom ZMK firmware for the Polarity Works ROTR macropad.

This project combines the upstream [ZMK](https://github.com/zmkfirmware/zmk)
keyboard firmware ecosystem with the excellent
[MOTR](https://github.com/martial-cc/MOTR) ROTR work by Carl Henriksson. MOTR
provides the working ROTR board support, MA730 magnetic encoder driver,
devicetree bindings, and the pinned upstream ZMK base. RMOTR builds on that
foundation with a layer-aware knob, stock-style ROTR RGB behavior, ghost-input
filtering, and connection-aware power management.

This is an independent community firmware. It is not affiliated with Polarity
Works.

## What Is New

- One physical MA730 knob controls different actions depending on the active
  layer.
- Hold-middle layer selector: hold the middle button, turn the knob to choose a
  layer, release to land on it.
- Per-layer RGB underglow that behaves like the original ROTR:
  - RGB on: the active layer color stays lit continuously.
  - RGB off: the LED stays dark, except while holding middle to select a layer.
- Stationary knob jitter filtering to prevent occasional ghost inputs.
- USB-over-Bluetooth priority:
  - USB HID is preferred whenever plugged into a host.
  - BLE is used when USB HID is unavailable.
- Power management:
  - USB idle power-off after 15 minutes.
  - BLE or disconnected idle power-off after 5 minutes.
  - Immediate power-off when USB/BLE disconnects and no usable host remains.
  - Button press can wake the device from power-off.

## Current Controls

The ROTR has three buttons and one knob.

| Control | Normal behavior |
|---------|-----------------|
| Left button | Copy, `Ctrl+C` |
| Middle button tap | Select all, `Ctrl+A` |
| Middle button hold | Enter layer selection mode |
| Right button | Paste, `Ctrl+V` |
| Hold middle + tap right | Toggle RGB underglow |
| Hold middle + tap left | Clear Bluetooth bonds |
| Hold middle + turn knob | Select active layer |

## Current Layers

| Layer | Name | Knob behavior | RGB color |
|------:|------|---------------|-----------|
| 0 | Default | No knob output | White |
| 1 | Brightness | Clockwise `F13`, counter-clockwise `F14` | Amber/yellow |
| 2 | Arrows | Clockwise Right, counter-clockwise Left | Green |
| 3 | Horizontal scroll | Smooth horizontal scroll | Cyan |
| 4 | Vertical scroll | Smooth vertical scroll | Magenta |
| 5 | Reserved | Pre-loaded as volume up/down, disabled by default | Orange |
| 6 | Reserved | No knob output | Blue |
| 7 | Reserved | No knob output | Teal |
| 8 | Reserved | No knob output | Purple |
| 9 | Selector | Temporary held layer used for layer selection | Not shown |

Layers 5-8 are intentionally reserved. Layer 5 already has volume keycodes in
the devicetree, but it is excluded from the selector until
`select-layer-count` is raised.

## RGB Behavior

RGB is toggled with hold middle + tap right.

When RGB is on, the LED shows the active layer color continuously, including
while idle and after a layer is selected.

When RGB is off, the LED is dark at rest. While middle is held for layer
selection, the LED lights in the candidate layer color and updates as the knob
turns. Releasing middle lands the layer and turns the LED dark again.

## Power Behavior

USB is preferred over Bluetooth. If the ROTR is connected over Bluetooth and is
then plugged into a USB host, USB HID becomes the active output.

Current timeout behavior:

| State | Behavior |
|-------|----------|
| USB HID connected | Power off after 15 minutes of inactivity |
| BLE connected | Power off after 5 minutes of inactivity |
| Not connected | Power off after 5 minutes of inactivity |
| USB power lost with no BLE connection | Power off immediately |
| BLE disconnects with no USB HID connection | Power off immediately |

A button press can wake the device from power-off. Knob movement alone cannot
wake it, because the MA730 encoder is read by SPI polling and is not a GPIO wake
source.

## Hardware

Target hardware:

- Polarity Works ROTR
- Nordic nRF52840
- MA730 magnetic rotary encoder
- Three-button matrix
- WS2812 underglow strip
- USB and Bluetooth
- Battery reporting

## Building

Firmware is built with GitHub Actions. No local ZMK toolchain is required.

1. Fork this repository.
2. Edit the board files if needed:
   - Keymap: `boards/polarityworks/rotr/rotr.keymap`
   - Knob layers/tunables: `boards/polarityworks/rotr/rotr_nrf52840_zmk.dts`
   - Firmware config: `boards/polarityworks/rotr/rotr_nrf52840_zmk_defconfig`
3. Push a commit or run the workflow manually from the Actions tab.
4. Download the generated `ROTR.uf2` artifact.

## Flashing

1. Double-press the reset button on the ROTR PCB to enter the UF2 bootloader.
2. The device should mount as USB mass storage.
3. Copy `ROTR.uf2` onto it.
4. The device reboots into the new firmware.

## Project Map

| Path | Purpose |
|------|---------|
| `boards/polarityworks/rotr/rotr.keymap` | Button bindings and layer definitions |
| `boards/polarityworks/rotr/rotr_nrf52840_zmk.dts` | Hardware, MA730 modes, layer tunables |
| `drivers/sensor/ma730/ma730_input.c` | Layer-aware MA730 knob driver |
| `src/rgb_indicator.c` | Per-layer RGB indicator and LED power handling |
| `src/power_policy.c` | USB/BLE priority and idle power-off policy |
| `config/west.yml` | Pinned ZMK revision |
| `DESIGN.md` | Detailed design notes and rationale |
| `CLAUDE.md` | Short project map for future AI/code sessions |

## Credits

- [ZMK](https://github.com/zmkfirmware/zmk), the keyboard firmware project this
  firmware builds on.
- [MOTR](https://github.com/martial-cc/MOTR) by Carl Henriksson, which provides
  the ROTR board support, MA730 driver foundation, DTS bindings, and pinned ZMK
  setup used here.
- Original Polarity Works ROTR behavior, especially the layer-color underglow
  model, inspired the RGB behavior implemented in this fork.

## License

MIT. See [LICENSE](LICENSE).
