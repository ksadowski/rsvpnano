# RSVP Nano

RSVP Nano is an open-source ESP32-S3 reading device for showing text one word at a time with RSVP (Rapid Serial Visual Presentation). The firmware is built around stable anchor-letter rendering, readable typography, tunable pacing, SD card storage, and local EPUB conversion.

## Highlights

- One-word RSVP reader with stable anchor alignment.
- Adjustable typography, anchor guides, pacing, and phantom words.
- Chapter and paragraph-aware navigation.
- SD card library under `/books`.
- Local on-device EPUB conversion to cached `.rsvp` files.
- Streaming book engine with sidecar `.idx` files: large books (100k+ words) load and read with bounded DRAM use.
- USB mass-storage mode for copying books to the SD card.
- Browser-based firmware installation with no IDE required.

## Getting Started

### Flash From The Browser

The easiest way to install the firmware is the web flasher:

<https://ionutdecebal.github.io/rsvpnano/>

Use Chrome or Edge on desktop, connect the device over USB, and follow the installer prompts.

The browser flasher uses ESP Web Tools and Web Serial, so it must be opened over HTTPS or localhost.

### Add Books

Create a `books` folder at the root of the SD card:

```text
/books
  my-book.epub
  another-book.rsvp
```

The firmware prioritizes `.rsvp` files. If a matching `.rsvp` file does not exist yet, an EPUB appears in the library and is converted locally the first time it is opened. The converted `.rsvp` file is then reused on future launches.

If a conversion is interrupted, you may see sidecar files such as:

```text
.rsvp.tmp
.rsvp.converting
.rsvp.failed
```

The reader also creates a `<book>.rsvp.idx` sidecar next to each `.rsvp` (see
[Streaming Book Engine](#streaming-book-engine) below). It is rebuilt
automatically if missing or stale, so it is safe to delete.

## Streaming Book Engine

To support very large books (the original motivation was a 161k-word German
novel that exhausted DRAM and reset the device), the firmware no longer keeps
the full word list in RAM. Instead it uses a small sidecar index next to each
`.rsvp` file:

```text
/books/MyBook.rsvp        original word stream
/books/MyBook.rsvp.idx    binary word index (auto-generated)
```

How it works:

- On first open of a `.rsvp`, the firmware streams the file once and writes
  `MyBook.rsvp.idx`. This contains the cleaned word bytes plus offset and
  length tables, chapter and paragraph markers, and a footer with a magic
  number, version and the source file size.
- Subsequent opens skip the build: the index is opened, its offset table is
  loaded into PSRAM, and the `.rsvp` words are read from SD on demand with a
  small in-memory LRU cache.
- After an EPUB is converted on device, its `.idx` is built immediately so the
  first read is instant.
- The index is invalidated automatically when the source `.rsvp` size or the
  index format version changes; the next load rebuilds it.

Memory effect on a 161k-word book:

- Before: every word lived as an Arduino `String` in DRAM, totalling tens of
  MB and crashing the device.
- After: ~640 KB offset table + ~320 KB length table in **PSRAM**, word bytes
  stay on SD, only the current word plus a tiny cache live in DRAM. Total DRAM
  use stays around 10 % regardless of book size.

The index format lives in `src/storage/BookIndex.{h,cpp}`. Its converter tag
(`idx-v1`) is bumped whenever the parser logic changes so old caches are
invalidated.

## Build From Source

Install PlatformIO Core, then run:

```sh
pio run
pio run -t upload
pio device monitor
```

The default environment is `waveshare_esp32s3_usb_msc`, which includes the reader and USB transfer mode.

Serial monitor runs at `115200`.

To export the merged binary used by the browser flasher:

```sh
python3 tools/export_web_firmware.py
```

That command writes:

```text
web/firmware/rsvp-nano.bin
```

## Hardware

The current firmware configuration targets the [Waveshare ESP32-S3-Touch-LCD-3.49](https://www.waveshare.com/esp32-s3-touch-lcd-3.49.htm?&aff_id=153227). This is an affiliate link, so if you click it to find the hardware and buy the board, it helps support the project:

- ESP32-S3 with 16 MB flash and OPI PSRAM.
- AXS15231B-based 172 x 640 LCD panel used in landscape as 640 x 172.
- SD card connected through `SD_MMC`.
- Touch, battery, and board power control pins defined in `src/board/BoardConfig.h`.

If you are adapting the project to different hardware, start with `src/board/BoardConfig.h`, then review the display, touch, power, and SD wiring code.

## Desktop Book Conversion

If you prefer to pre-convert books on a computer, copy the helper files from `tools/sd_card_converter` to the SD card root and run the launcher for your platform:

- Windows: `Convert books.bat`
- macOS: `Convert books.command`
- Linux: `convert_books_linux.sh` or `python3 convert_books.py`

The desktop converter scans `/books` and creates `.rsvp` files beside supported sources.
The Linux path has been used during development. The macOS and Windows launchers are included, but they have not been tested yet.

Supported input formats:

- `.epub`
- `.txt`
- `.md` / `.markdown`
- `.html` / `.htm` / `.xhtml`

## Languages And Text Encoding

The reader is UTF-8 throughout. Books in English plus Polish (full diacritics:
`ą ć ę ł ń ó ś ź ż` and uppercase `Ą Ć Ę Ł Ń Ó Ś Ź Ż`) and other Western
European languages with Latin-1 accented letters render with the embedded
serif font.

If you regenerate the embedded fonts, you only need Python 3 and Pillow
(the TrueType file is downloaded automatically on first run):

```sh
python -m pip install Pillow
python tools/generate_embedded_serif_font.py \
    --output src/display/EmbeddedSerifFont.h
python tools/generate_embedded_serif_font.py --point-size 35 \
    --symbol-prefix EmbeddedSerif70 \
    --output src/display/EmbeddedSerifFont70.h
```

The script caches Noto Sans Regular into `tools/fonts/`. Pass
`--font-file path/to/font.ttf` to use a different TrueType file.

The list of extra Unicode code points (Polish, Latin-1, common typographic
punctuation) lives at the top of `tools/generate_embedded_serif_font.py`. Add
to it to support more scripts.

## RSVP File Format

`.rsvp` files are plain UTF-8 text. The reader understands a small set of
directives:

```text
@rsvp 1
@title The Book Title
@author Author Name
@source /books/source.epub
@chapter Chapter 1
@para
```

Normal text lines after the directives are split into words by the firmware.

## Contributing

Issues, experiments, forks, and pull requests are welcome. If you change hardware mappings, build environments, or the flashing flow, please update the relevant docs alongside the code.

## License

MIT. See [LICENSE](LICENSE).
