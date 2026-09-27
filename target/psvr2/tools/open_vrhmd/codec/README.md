# Authored media codecs

These C11 decoders have no external codec or compression dependency. They use
the C runtime and `libm`. Images produce owned, top-first RGBA8 pixels; video
frames expose decoder-owned YUV420 planes in display order. Errors leave image
output empty and distinguish a malformed stream from clean playback completion.

| Decoder | Supported input |
| --- | --- |
| PNG | Grayscale, RGB, palette, grayscale-alpha and RGBA; 1/2/4/8/16-bit depths where defined; all filters, transparency, Adam7; stored/fixed/dynamic DEFLATE. |
| JPEG | 8-bit Huffman sequential and progressive DCT; grayscale, RGB/YCbCr, CMYK/YCCK; subsampling, restart intervals, spectral selection and successive approximation. |
| MPEG | MPEG-1 elementary/program streams; I/P/B frames, motion compensation and custom quantization matrices. Program-stream audio is skipped. |
| MP3 | MPEG-1 Layer III at 32, 44.1 and 48 kHz; mono/stereo/joint stereo, CBR/VBR and bit reservoir. |

PNG/JPEG dimensions are limited to 8192; their combined input, workspace and
output budget is 192 MiB. MPEG dimensions are limited to 2048×2048; dimension
changes and MPEG-2 video extensions fail. JPEG arithmetic, lossless and 12-bit
coding are unsupported. Metadata does not apply EXIF orientation or color
profiles. MPEG and MP3 read from files with bounded decoder state.

Tests create synthetic images, tones and video locally. PNG checks exact pixels;
JPEG uses Pillow, and MPEG/MP3 use FFmpeg as optional independent test oracles.
These programs are test dependencies and are never used by the target decoder.
Bounds and malformed inputs also run under AddressSanitizer/UndefinedBehaviorSanitizer.

Specifications: [PNG](https://www.w3.org/TR/png-3/),
[zlib](https://www.rfc-editor.org/rfc/rfc1950),
[DEFLATE](https://www.rfc-editor.org/rfc/rfc1951),
[JPEG T.81](https://www.w3.org/Graphics/JPEG/itu-t81.pdf), and ISO/IEC 11172
parts 1–3 for MPEG system, video and audio syntax. Numeric coding tables describe
the formats; no third-party decoder implementation is included.
