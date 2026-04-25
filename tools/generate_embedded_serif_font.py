#!/usr/bin/env python3
"""Generate the embedded font header used by the firmware renderer.

Uses Pillow (which bundles freetype) so the script works the same on Windows,
macOS, and Linux. A Noto Sans TrueType file is required; if it is not found
locally, the script downloads it once into ``tools/fonts/``.
"""

from __future__ import annotations

import argparse
import math
import os
import pathlib
import sys
import urllib.request

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:  # pragma: no cover
    sys.exit(
        "Pillow is required. Install it with:\n"
        "    python -m pip install Pillow\n"
        f"({exc})"
    )


DEFAULT_FONT_NAME = "NotoSans"
DEFAULT_FONT_FILENAME = "NotoSans-Regular.ttf"
DEFAULT_FONT_DOWNLOAD_URL = (
    "https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSans/"
    "hinted/ttf/NotoSans-Regular.ttf"
)
DEFAULT_FONT_CACHE_DIR = pathlib.Path(__file__).resolve().parent / "fonts"
DEFAULT_FONT_SEARCH_PATHS = [
    DEFAULT_FONT_CACHE_DIR,
    pathlib.Path("/usr/share/fonts/truetype/noto"),
    pathlib.Path("/usr/share/fonts/noto"),
    pathlib.Path("/Library/Fonts"),
    pathlib.Path("/System/Library/Fonts"),
    pathlib.Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts",
    pathlib.Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/Windows/Fonts"
    if os.environ.get("LOCALAPPDATA")
    else pathlib.Path("."),
]
DEFAULT_POINT_SIZE = 52
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
DEFAULT_OUTPUT_PATH = pathlib.Path("src/display/EmbeddedSerifFont.h")
DEFAULT_SYMBOL_PREFIX = "EmbeddedSerif"

# Extra Unicode code points rendered into a sparse side table indexed by
# binary search at runtime. Each entry maps the Unicode code point to its
# Adobe Glyph List (AGL) name as recognised by Ghostscript's `glyphshow`.
# Keep this list sorted by code point so the generated table is also sorted.
DEFAULT_EXTRA_CODEPOINTS: list[tuple[int, str]] = [
    # Latin-1 supplement letters and common punctuation.
    (0x00A1, "exclamdown"),
    (0x00A9, "copyright"),
    (0x00AB, "guillemotleft"),
    (0x00AE, "registered"),
    (0x00BB, "guillemotright"),
    (0x00BF, "questiondown"),
    (0x00C0, "Agrave"), (0x00C1, "Aacute"), (0x00C2, "Acircumflex"),
    (0x00C3, "Atilde"), (0x00C4, "Adieresis"), (0x00C5, "Aring"),
    (0x00C6, "AE"), (0x00C7, "Ccedilla"),
    (0x00C8, "Egrave"), (0x00C9, "Eacute"), (0x00CA, "Ecircumflex"),
    (0x00CB, "Edieresis"),
    (0x00CC, "Igrave"), (0x00CD, "Iacute"), (0x00CE, "Icircumflex"),
    (0x00CF, "Idieresis"),
    (0x00D0, "Eth"), (0x00D1, "Ntilde"),
    (0x00D2, "Ograve"), (0x00D3, "Oacute"), (0x00D4, "Ocircumflex"),
    (0x00D5, "Otilde"), (0x00D6, "Odieresis"), (0x00D8, "Oslash"),
    (0x00D9, "Ugrave"), (0x00DA, "Uacute"), (0x00DB, "Ucircumflex"),
    (0x00DC, "Udieresis"), (0x00DD, "Yacute"), (0x00DE, "Thorn"),
    (0x00DF, "germandbls"),
    (0x00E0, "agrave"), (0x00E1, "aacute"), (0x00E2, "acircumflex"),
    (0x00E3, "atilde"), (0x00E4, "adieresis"), (0x00E5, "aring"),
    (0x00E6, "ae"), (0x00E7, "ccedilla"),
    (0x00E8, "egrave"), (0x00E9, "eacute"), (0x00EA, "ecircumflex"),
    (0x00EB, "edieresis"),
    (0x00EC, "igrave"), (0x00ED, "iacute"), (0x00EE, "icircumflex"),
    (0x00EF, "idieresis"),
    (0x00F0, "eth"), (0x00F1, "ntilde"),
    (0x00F2, "ograve"), (0x00F3, "oacute"), (0x00F4, "ocircumflex"),
    (0x00F5, "otilde"), (0x00F6, "odieresis"), (0x00F8, "oslash"),
    (0x00F9, "ugrave"), (0x00FA, "uacute"), (0x00FB, "ucircumflex"),
    (0x00FC, "udieresis"), (0x00FD, "yacute"), (0x00FE, "thorn"),
    (0x00FF, "ydieresis"),
    # Polish letters.
    (0x0104, "Aogonek"), (0x0105, "aogonek"),
    (0x0106, "Cacute"), (0x0107, "cacute"),
    (0x0118, "Eogonek"), (0x0119, "eogonek"),
    (0x0141, "Lslash"), (0x0142, "lslash"),
    (0x0143, "Nacute"), (0x0144, "nacute"),
    (0x015A, "Sacute"), (0x015B, "sacute"),
    (0x0179, "Zacute"), (0x017A, "zacute"),
    (0x017B, "Zdotaccent"), (0x017C, "zdotaccent"),
    # General punctuation / typographic.
    (0x2013, "endash"), (0x2014, "emdash"),
    (0x2018, "quoteleft"), (0x2019, "quoteright"),
    (0x201C, "quotedblleft"), (0x201D, "quotedblright"),
    (0x2026, "ellipsis"),
    (0x20AC, "Euro"),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate an embedded font header for the display renderer."
    )
    parser.add_argument(
        "--point-size",
        type=int,
        default=DEFAULT_POINT_SIZE,
        help=f"Source font point size. Default: {DEFAULT_POINT_SIZE}",
    )
    parser.add_argument(
        "--font-name",
        default=DEFAULT_FONT_NAME,
        help=f"Logical font name written into the header banner. Default: {DEFAULT_FONT_NAME}",
    )
    parser.add_argument(
        "--font-file",
        type=pathlib.Path,
        default=None,
        help=(
            "Path to a TrueType font file. When omitted, the script searches\n"
            "local cache and OS font directories, falling back to a one-time\n"
            "download of Noto Sans Regular into tools/fonts/."
        ),
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=DEFAULT_OUTPUT_PATH,
        help=f"Output header path. Default: {DEFAULT_OUTPUT_PATH}",
    )
    parser.add_argument(
        "--symbol-prefix",
        default=DEFAULT_SYMBOL_PREFIX,
        help=f"Prefix for generated struct/constants. Default: {DEFAULT_SYMBOL_PREFIX}",
    )
    parser.add_argument(
        "--force-height",
        type=int,
        default=0,
        help=(
            "If set, pad the cropped glyph band (symmetrically, top first) with\n"
            "transparent rows until it reaches exactly this height. Used to make\n"
            "alternate font families share identical metrics with the Sans/Serif\n"
            "reference families so the renderer can hot-swap bitmaps."
        ),
    )
    return parser.parse_args()


def locate_font_file(explicit: pathlib.Path | None) -> pathlib.Path:
    if explicit is not None:
        if not explicit.is_file():
            sys.exit(f"Font file not found: {explicit}")
        return explicit

    candidate_names = [
        DEFAULT_FONT_FILENAME,
        "NotoSans-Regular.ttf",
        "DejaVuSans.ttf",
        "LiberationSans-Regular.ttf",
    ]
    for directory in DEFAULT_FONT_SEARCH_PATHS:
        if not directory or not directory.is_dir():
            continue
        for name in candidate_names:
            candidate = directory / name
            if candidate.is_file():
                return candidate

    DEFAULT_FONT_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    target = DEFAULT_FONT_CACHE_DIR / DEFAULT_FONT_FILENAME
    print(f"Downloading {DEFAULT_FONT_FILENAME} to {target}...", file=sys.stderr)
    try:
        urllib.request.urlretrieve(DEFAULT_FONT_DOWNLOAD_URL, target)
    except Exception as exc:  # pragma: no cover
        sys.exit(
            f"Could not download Noto Sans automatically ({exc}).\n"
            f"Please download it manually from:\n  {DEFAULT_FONT_DOWNLOAD_URL}\n"
            f"and save it to:\n  {target}\n"
            f"or pass --font-file <path-to-ttf>."
        )
    return target


def render_glyph_raster(
    font: ImageFont.FreeTypeFont, codepoint: int
) -> tuple[int, int, bytes]:
    """Rasterise a single code point into a CANVAS-sized grayscale image.

    Returns (width, height, raster) where pixel value matches Ghostscript's
    pgmraw convention: 255 = background, 0 = full glyph coverage. The caller
    converts to alpha via ``alpha_at`` (255 - pixel).
    """
    image = Image.new("L", (CANVAS_WIDTH, CANVAS_HEIGHT), 255)
    draw = ImageDraw.Draw(image)
    text = chr(codepoint)
    # Anchor "ls" places (x, y) at the left edge of the glyph on the baseline,
    # mirroring PostScript's `moveto ... show` semantics.
    draw.text((ORIGIN_X, BASELINE_Y), text, font=font, fill=0, anchor="ls")
    return CANVAS_WIDTH, CANVAS_HEIGHT, image.tobytes()


def advance_width_for(font: ImageFont.FreeTypeFont, codepoint: int) -> int:
    text = chr(codepoint)
    advance = font.getlength(text)
    return max(1, int(math.floor(float(advance) + 0.5)))


def alpha_at(raster: bytes, width: int, x: int, y: int) -> int:
    return 255 - raster[y * width + x]


def main() -> None:
    args = parse_args()
    extras = sorted(DEFAULT_EXTRA_CODEPOINTS)

    font_path = locate_font_file(args.font_file)
    print(f"Using font: {font_path}", file=sys.stderr)
    font = ImageFont.truetype(str(font_path), args.point_size)

    ascii_glyph_images: dict[int, tuple[int, int, bytes]] = {}
    extra_glyph_images: dict[int, tuple[int, int, bytes]] = {}
    global_top = CANVAS_HEIGHT
    global_bottom = -1

    def update_bounds(width: int, height: int, raster: bytes) -> None:
        nonlocal global_top, global_bottom
        for y in range(height):
            for x in range(width):
                if alpha_at(raster, width, x, y) > ALPHA_THRESHOLD:
                    global_top = min(global_top, y)
                    global_bottom = max(global_bottom, y)
                    break

    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        width, height, raster = render_glyph_raster(font, code)
        ascii_glyph_images[code] = (width, height, raster)
        update_bounds(width, height, raster)

    for codepoint, _glyph_name in extras:
        width, height, raster = render_glyph_raster(font, codepoint)
        extra_glyph_images[codepoint] = (width, height, raster)
        update_bounds(width, height, raster)

    if global_bottom < global_top:
        raise RuntimeError("Failed to detect any font pixels")

    crop_top = max(0, global_top - FONT_TOP_PADDING)
    crop_bottom = min(CANVAS_HEIGHT - 1, global_bottom + FONT_BOTTOM_PADDING)
    font_height = crop_bottom - crop_top + 1

    if args.force_height:
        target = args.force_height
        if font_height > target:
            sys.exit(
                f"Detected glyph height {font_height} already exceeds "
                f"--force-height={target}; pick a smaller --point-size."
            )
        # Pad transparently by widening the crop window, alternating top/bottom
        # (top first) so the resulting band remains roughly baseline-centred.
        extend_top = True
        while font_height < target:
            if extend_top and crop_top > 0:
                crop_top -= 1
            elif not extend_top and crop_bottom < CANVAS_HEIGHT - 1:
                crop_bottom += 1
            elif crop_top > 0:
                crop_top -= 1
            elif crop_bottom < CANVAS_HEIGHT - 1:
                crop_bottom += 1
            else:
                sys.exit(
                    f"Canvas exhausted before reaching --force-height={target}; "
                    "raise CANVAS_HEIGHT in the generator."
                )
            font_height = crop_bottom - crop_top + 1
            extend_top = not extend_top
    bitmap_bytes: list[int] = []
    ascii_entries: list[str] = []
    extra_entries: list[str] = []

    def append_glyph_bitmap(width: int, height: int, raster: bytes) -> tuple[int, int, int]:
        """Append the cropped bitmap for one glyph, returning (offset, x_offset, glyph_width)."""
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
            return bitmap_offset, min_x - ORIGIN_X, glyph_width

        return bitmap_offset, 0, 0

    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        width, height, raster = ascii_glyph_images[code]
        bitmap_offset, x_offset, glyph_width = append_glyph_bitmap(width, height, raster)
        x_advance = advance_width_for(font, code)
        ascii_entries.append(
            "    {"
            + f"{bitmap_offset}, {x_offset}, {glyph_width}, {x_advance}"
            + "}, "
            + f"// {repr(chr(code))}"
        )

    for codepoint, glyph_name in extras:
        width, height, raster = extra_glyph_images[codepoint]
        bitmap_offset, x_offset, glyph_width = append_glyph_bitmap(width, height, raster)
        x_advance = advance_width_for(font, codepoint)
        extra_entries.append(
            "    {"
            + f"0x{codepoint:04X}, {bitmap_offset}, {x_offset}, {glyph_width}, {x_advance}"
            + "}, "
            + f"// U+{codepoint:04X} {glyph_name}"
        )

    lines: list[str] = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "// Generated from a real serif font at build time and embedded as glyph data.",
        f"// Source font: {args.font_name} at {args.point_size} pt",
        "",
        f"struct {args.symbol_prefix}Glyph " + "{",
        "  uint32_t bitmapOffset;",
        "  int8_t xOffset;",
        "  uint8_t width;",
        "  uint8_t xAdvance;",
        "};",
        "",
        f"struct {args.symbol_prefix}ExtraGlyph " + "{",
        "  uint32_t codepoint;",
        "  uint32_t bitmapOffset;",
        "  int8_t xOffset;",
        "  uint8_t width;",
        "  uint8_t xAdvance;",
        "};",
        "",
        f"constexpr uint8_t k{args.symbol_prefix}FirstChar = {FIRST_CHAR};",
        f"constexpr uint8_t k{args.symbol_prefix}LastChar = {LAST_CHAR};",
        f"constexpr uint8_t k{args.symbol_prefix}Height = {font_height};",
        f"constexpr size_t k{args.symbol_prefix}ExtraGlyphCount = {len(extra_entries)};",
        "",
        f"static const uint8_t k{args.symbol_prefix}Bitmaps[] PROGMEM = " + "{",
    ]

    if not bitmap_bytes:
        bitmap_bytes.append(0)

    for offset in range(0, len(bitmap_bytes), 16):
        chunk = bitmap_bytes[offset : offset + 16]
        lines.append("    " + ", ".join(f"{value:3d}" for value in chunk) + ",")

    lines += [
        "};",
        "",
        f"static const {args.symbol_prefix}Glyph k{args.symbol_prefix}Glyphs[] PROGMEM = " + "{",
        *ascii_entries,
        "};",
        "",
        f"static const {args.symbol_prefix}ExtraGlyph k{args.symbol_prefix}ExtraGlyphs[] PROGMEM = " + "{",
    ]

    if extra_entries:
        lines.extend(extra_entries)
    else:
        lines.append("    {0, 0, 0, 0, 0},  // sentinel; count is 0")

    lines += [
        "};",
        "",
    ]

    args.output.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
