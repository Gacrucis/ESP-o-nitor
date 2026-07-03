# 3d-models

Printable enclosure for the ESP-o-nitor, exported as STEP (ISO 10303) so it can be imported into
any CAD tool or slicer (FreeCAD, Fusion 360, PrusaSlicer, etc.).

## Parts

| File | Part |
| --- | --- |
| `Case - ESPCase.step` | Housing for the ESP32 board. |
| `Case - ESPLid.step` | Lid for the ESP32 housing. |
| `Case - BiggerBase.step` | Base that holds the two OLED SSD1306 (128x64) displays. |
| `Case - Lid.step` | Lid for the display base. |

Together they make up the enclosure for the ESP32 and the two screens.

## Notes

- STEP is a solid-model format: open the files in a slicer to generate the printing G-code, or in
  CAD to adjust them.
- Match the print with the hardware in [`../esp32/`](../esp32/): ESP32 dev board and two OLED
  SSD1306 128x64 displays.
