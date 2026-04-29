# MOTR

Aftermarket ZMK firmware for the Polarity Works — ROTR

This firmware provides:

- left, middle, and right mouse buttons
- encoder mouse-wheel scrolling
- USB and Bluetooth support
- RGB underglow
- battery reporting

Built against upstream ZMK (pinned in `config/west.yml`) with an
out-of-tree board definition and encoder driver.

This is an independent project and is not affiliated with Polarity Works.

## Hardware

- Polarity Works — ROTR

## Download

https://github.com/martial-cc/MOTR/releases/latest/download/MOTR.uf2

## Building

To build your own firmware:

1. Fork this repository.
2. In your fork, edit `boards/polarityworks/rotr/rotr.keymap` to change the button behavior.
3. Click the **Actions** tab.
4. Click **Run workflow** (or open the latest run).
5. Wait until the workflow completes.
6. Open the run and download the `.uf2` file from the artifacts.

## Flashing

To flash:

1. Double-press the reset button on the device to enter the bootloader.
   (This is the small button on the PCB.)
2. The device appears as a USB mass storage device.
3. Copy the firmware file (`MOTR.uf2`) to it.
4. The device will restart automatically.

## Notes

An alternate keymap with RGB controls is available at:

[rotr.keymap-rgb](https://github.com/martial-cc/MOTR/blob/main/boards/polarityworks/rotr/rotr.keymap-rgb)

Copy it over `boards/polarityworks/rotr/rotr.keymap` before building to use it.

## License

MIT. Copyright (c) 2026 Carl Henriksson.
