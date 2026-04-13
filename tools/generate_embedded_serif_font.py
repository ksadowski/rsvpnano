#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import subprocess
import tempfile


FONT_NAME = "DejaVuSerif"
FONT_SEARCH_PATHS = ["/usr/share/fonts/truetype/dejavu"]
POINT_SIZE = 52
CANVAS_WIDTH = 112
CANVAS_HEIGHT = 128
ORIGIN_X = 10
BASELINE_Y = 76
ALPHA_THRESHOLD = 16
FONT_TOP_PADDING = 4
FONT_BOTTOM_PADDING = 2
FIRST_CHAR = 32
LAST_CHAR = 126
SPACE_ADVANCE = 6
OUTPUT_PATH = pathlib.Path("src/display/EmbeddedSerifFont.h")


def escape_postscript_char(ch: str) -> str:
    if ch in ("\\", "(", ")"):
        return "\\" + ch
    code = ord(ch)
    if code < 32 or code > 126:
        return f"\\{code:03o}"
    return ch


def render_glyph(tmp_dir: pathlib.Path, ch: str) -> pathlib.Path:
    output = tmp_dir / f"{ord(ch):03d}.pgm"
    escaped = escape_postscript_char(ch)
    program = (
        "1 setgray clippath fill "
        "0 setgray "
        f"/{FONT_NAME} findfont {POINT_SIZE} scalefont setfont "
        f"{ORIGIN_X} {BASELINE_Y} moveto "
        f"({escaped}) show showpage"
    )
    command = [
        "gs",
        "-q",
        "-dNOPAUSE",
        "-dBATCH",
        "-dTextAlphaBits=4",
        "-dGraphicsAlphaBits=4",
        "-sDEVICE=pgmraw",
        "-r72",
        f"-g{CANVAS_WIDTH}x{CANVAS_HEIGHT}",
        f"-sOutputFile={output}",
    ]

    for font_path in FONT_SEARCH_PATHS:
        if pathlib.Path(font_path).is_dir():
            command.append(f"-sFONTPATH={font_path}")

    command += [
        "-c",
        program,
    ]

    subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )
    return output


def parse_pgm(path: pathlib.Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"P5\n"):
        raise ValueError(f"Unexpected PGM header in {path}")

    parts = data.split(b"\n")
    index = 1
    while parts[index].startswith(b"#"):
        index += 1

    width, height = map(int, parts[index].split())
    max_value = int(parts[index + 1])
    if max_value != 255:
        raise ValueError(f"Unexpected max value {max_value} in {path}")

    raster = b"\n".join(parts[index + 2 :])
    expected_length = width * height
    if len(raster) != expected_length:
        raise ValueError(f"Unexpected raster length in {path}: {len(raster)} != {expected_length}")

    return width, height, raster


def alpha_at(raster: bytes, width: int, x: int, y: int) -> int:
    return 255 - raster[y * width + x]


def main() -> None:
    glyph_images: dict[str, tuple[int, int, bytes]] = {}
    global_top = CANVAS_HEIGHT
    global_bottom = -1

    with tempfile.TemporaryDirectory(prefix="serif_font_") as tmp:
        tmp_dir = pathlib.Path(tmp)

        for code in range(FIRST_CHAR, LAST_CHAR + 1):
            ch = chr(code)
            pgm_path = render_glyph(tmp_dir, ch)
            width, height, raster = parse_pgm(pgm_path)
            glyph_images[ch] = (width, height, raster)

            for y in range(height):
                found = False
                for x in range(width):
                    if alpha_at(raster, width, x, y) > ALPHA_THRESHOLD:
                        global_top = min(global_top, y)
                        global_bottom = max(global_bottom, y)
                        found = True
                if found:
                    continue

    if global_bottom < global_top:
        raise RuntimeError("Failed to detect any font pixels")

    crop_top = max(0, global_top - FONT_TOP_PADDING)
    crop_bottom = min(CANVAS_HEIGHT - 1, global_bottom + FONT_BOTTOM_PADDING)
    font_height = crop_bottom - crop_top + 1
    bitmap_bytes: list[int] = []
    glyph_entries: list[str] = []

    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        ch = chr(code)
        width, height, raster = glyph_images[ch]

        min_x = width
        max_x = -1
        for y in range(crop_top, crop_bottom + 1):
            for x in range(width):
                if alpha_at(raster, width, x, y) > ALPHA_THRESHOLD:
                    min_x = min(min_x, x)
                    max_x = max(max_x, x)

        bitmap_offset = len(bitmap_bytes)

        if max_x >= min_x:
            glyph_width = max_x - min_x + 1
            for y in range(crop_top, crop_bottom + 1):
                for x in range(min_x, max_x + 1):
                    alpha = alpha_at(raster, width, x, y)
                    if alpha <= ALPHA_THRESHOLD:
                        alpha = 0
                    bitmap_bytes.append(alpha)
            x_advance = glyph_width + 1
        else:
            glyph_width = 0
            x_advance = SPACE_ADVANCE

        glyph_entries.append(
            "    "
            + "{"
            + f"{bitmap_offset}, {glyph_width}, {x_advance}"
            + "}, "
            + f"// {repr(ch)}"
        )

    lines: list[str] = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "// Generated from a real serif font at build time and embedded as glyph data.",
        f"// Source font: {FONT_NAME} at {POINT_SIZE} pt",
        "",
        "struct EmbeddedSerifGlyph {",
        "  uint32_t bitmapOffset;",
        "  uint8_t width;",
        "  uint8_t xAdvance;",
        "};",
        "",
        f"constexpr uint8_t kEmbeddedSerifFirstChar = {FIRST_CHAR};",
        f"constexpr uint8_t kEmbeddedSerifLastChar = {LAST_CHAR};",
        f"constexpr uint8_t kEmbeddedSerifHeight = {font_height};",
        "",
        "static const uint8_t kEmbeddedSerifBitmaps[] PROGMEM = {",
    ]

    for offset in range(0, len(bitmap_bytes), 16):
        chunk = bitmap_bytes[offset : offset + 16]
        lines.append("    " + ", ".join(f"{value:3d}" for value in chunk) + ",")

    lines += [
        "};",
        "",
        "static const EmbeddedSerifGlyph kEmbeddedSerifGlyphs[] PROGMEM = {",
        *glyph_entries,
        "};",
        "",
    ]

    OUTPUT_PATH.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
