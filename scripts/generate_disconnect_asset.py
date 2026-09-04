#!/usr/bin/env python3
"""Generate the S7.1 embedded RGB565 asset from the approved disconnect artwork.

`main/ui/generated/doze_frame.bin` is the checked 360x360 little-endian RGB565
conversion of `doze_frame_preview.png`. LVGL is built with LV_COLOR_16_SWAP=1,
so the generated C array stores each 16-bit pixel in the opposite byte order.
Keeping this conversion reproducible prevents a future manual byte-order change
from turning the on-device image into swapped colors.
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/ui/generated/doze_frame.bin"
OUTPUT_C = ROOT / "main/ui/generated/avatar_layers/avatar_disconnect.c"
OUTPUT_H = ROOT / "main/ui/generated/avatar_layers/avatar_disconnect.h"
EXPECTED_BYTES = 360 * 360 * 2


def main() -> None:
    raw = SOURCE.read_bytes()
    if len(raw) != EXPECTED_BYTES:
        raise SystemExit(f"unexpected source size: {len(raw)} != {EXPECTED_BYTES}")
    swapped = bytearray(EXPECTED_BYTES)
    swapped[0::2] = raw[1::2]
    swapped[1::2] = raw[0::2]

    lines = [
        "/* Generated from ui/generated/doze_frame_preview.png; do not edit by hand. */",
        '#include "avatar_disconnect.h"',
        "#if LV_COLOR_16_SWAP != 1",
        '#error "Avatar asset byte order disagrees with LV_COLOR_16_SWAP"',
        "#endif",
        "",
        "static const uint8_t julia_s7_1_disconnected_map[] = {",
    ]
    for offset in range(0, len(swapped), 16):
        chunk = swapped[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    lines.extend(
        [
            "};",
            "",
            "const lv_img_dsc_t avatar_asset_julia_s7_1_disconnected = {",
            "    .header.always_zero = 0,",
            "    .header.w = 360,",
            "    .header.h = 360,",
            "    .header.cf = LV_IMG_CF_TRUE_COLOR,",
            "    .data_size = sizeof(julia_s7_1_disconnected_map),",
            "    .data = julia_s7_1_disconnected_map,",
            "};",
            "",
        ]
    )
    OUTPUT_C.write_text("\n".join(lines), encoding="utf-8")
    OUTPUT_H.write_text(
        "/* Generated from ui/generated/doze_frame_preview.png; do not edit by hand. */\n"
        "#pragma once\n"
        '#include "lvgl.h"\n\n'
        "extern const lv_img_dsc_t avatar_asset_julia_s7_1_disconnected;\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
