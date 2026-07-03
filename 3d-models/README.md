# 3d-models

Printable enclosure for the ESP-o-nitor. Each part is provided both as STEP (ISO 10303, editable
in CAD) and as STL (ready to slice).

- [`STEP/`](STEP/) - solid models (FreeCAD, Fusion 360, etc.).
- [`STL/`](STL/) - meshes ready for a slicer (PrusaSlicer, Cura, etc.).
- [`previews/`](previews/) - PNG render of each part.

## Parts

| Part | Qty | Purpose | Preview |
| --- | --- | --- | --- |
| `Case - ESPCase` | 1 | Housing for the ESP32 board. | ![ESPCase](previews/Case%20-%20ESPCase.png) |
| `Case - ESPLid` | 1 | Lid for the ESP32 housing. | ![ESPLid](previews/Case%20-%20ESPLid.png) |
| `Case - BiggerBase` | 1 | Base that holds the two OLED SSD1306 (128x64) displays. | ![BiggerBase](previews/Case%20-%20BiggerBase.png) |
| `Case - Lid` | 2 | Bezel/lid for the display base - one per display. | ![Lid](previews/Case%20-%20Lid.png) |

You need **2 units of `Case - Lid`** for the display base (one bezel per screen). Every other part
is printed once. Together they make up the enclosure for the ESP32 and the two screens.

## Notes

- STEP is a solid-model format: open the `STL/` files directly in a slicer to generate the
  printing G-code, or the `STEP/` files in CAD to adjust them.
- Match the print with the hardware in [`../esp32/`](../esp32/): ESP32 dev board and two OLED
  SSD1306 128x64 displays.
