"""Generate PNG fixtures and verify the first-party decoder pixel for pixel."""

import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]
CODEC = ROOT / "target/psvr2/tools/open_vrhmd/codec"


class Image(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
                ("pixels", ctypes.POINTER(ctypes.c_ubyte))]


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def compressed_fixture(data):
    header = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", data) + chunk(b"IEND", b""))


def bad_deflate_fixtures():
    checksum = struct.pack(">I", zlib.adler32(b"\0\1\2\3\4"))
    def wrapped(bits):
        return compressed_fixture(b"\x78\x01" + bits + checksum)
    # Reserved block type, truncated stored data, and a bad LEN/NLEN pair.
    result = [wrapped(b"\x07"), wrapped(b"\x01\x05\x00\xfa\xff\0"),
              wrapped(b"\x01\x05\x00\0\0\0\1\2\3\4")]
    # Fixed codes 257 then distance 0 ask for a match before any literal exists.
    result.append(wrapped(b"\x03\x02"))
    # Dynamic HLIT=31 is reserved (it would declare 288 literal codes).
    result.append(wrapped(b"\xfd\0\0\0"))
    valid = zlib.compress(b"\0\1\2\3\4")
    result += [compressed_fixture(valid[:-1]), compressed_fixture(valid + b"\0"),
               compressed_fixture(valid[:-4] + b"\0\0\0\0")]
    return result


def paeth(a, b, c):
    prediction = a + b - c
    distances = [abs(prediction - value) for value in (a, b, c)]
    return (a, b, c)[distances.index(min(distances))]


def filtered_row(row, previous, pixel_bytes, kind):
    result = bytearray([kind])
    for index, value in enumerate(row):
        a = row[index - pixel_bytes] if index >= pixel_bytes else 0
        b = previous[index] if previous else 0
        c = previous[index - pixel_bytes] if previous and index >= pixel_bytes else 0
        predictor = (0, a, b, (a + b) // 2, paeth(a, b, c))[kind]
        result.append((value - predictor) & 255)
    return result


def fixture(width, height, color, depth, interlaced=False, filter_kind=0,
            compression="dynamic", transparent=True):
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    maximum = (1 << depth) - 1
    palette_count = min(16, 1 << depth) if color == 3 else 0
    palette = [(index * 17, (index * 37) & 255, (index * 59) & 255,
                (index * 31) & 255 if transparent else 255)
               for index in range(palette_count)]
    samples = []
    for y in range(height):
        row = []
        for x in range(width):
            values = tuple((x * 173 + y * 241 + channel * 337) & maximum
                           for channel in range(channels))
            if color == 3:
                values = ((x + y * 3) % palette_count,)
            row.append(values)
        samples.append(row)
    key = samples[0][0]
    expected = bytearray()
    for row in samples:
        for values in row:
            scaled = tuple(value >> 8 if depth == 16 else value * 255 // maximum
                           for value in values)
            if color == 3:
                expected.extend(palette[values[0]])
            elif color in (0, 4):
                alpha = scaled[1] if color == 4 else (0 if transparent and values == key else 255)
                expected.extend((scaled[0], scaled[0], scaled[0], alpha))
            else:
                alpha = scaled[3] if color == 6 else (0 if transparent and values == key else 255)
                expected.extend((*scaled[:3], alpha))
    passes = [(0, 0, 1, 1)] if not interlaced else [
        (0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
        (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    filtered = bytearray()
    for left, top, step_x, step_y in passes:
        if left >= width or top >= height:
            continue
        previous = None
        for y in range(top, height, step_y):
            values = [sample for x in range(left, width, step_x) for sample in samples[y][x]]
            if depth == 16:
                row = b"".join(struct.pack(">H", value) for value in values)
            elif depth == 8:
                row = bytes(values)
            else:
                row = bytearray((len(values) * depth + 7) // 8)
                for index, value in enumerate(values):
                    bit = index * depth
                    row[bit // 8] |= value << (8 - depth - bit % 8)
            filtered.extend(filtered_row(row, previous, max(1, (channels * depth + 7) // 8), filter_kind))
            previous = row
    if compression == "stored":
        compressed = zlib.compress(filtered, level=0)
    elif compression == "fixed":
        compressor = zlib.compressobj(level=6, strategy=zlib.Z_FIXED)
        compressed = compressor.compress(filtered) + compressor.flush()
    else:
        compressed = zlib.compress(filtered, level=9)
    header = struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, int(interlaced))
    png = bytearray(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header))
    if color == 3:
        png.extend(chunk(b"PLTE", bytes(value for entry in palette for value in entry[:3])))
        if transparent:
            png.extend(chunk(b"tRNS", bytes(entry[3] for entry in palette)))
    elif transparent and color in (0, 2):
        png.extend(chunk(b"tRNS", b"".join(struct.pack(">H", value) for value in key)))
    midpoint = len(compressed) // 2
    png.extend(chunk(b"IDAT", compressed[:midpoint]))
    png.extend(chunk(b"IDAT", compressed[midpoint:]))
    png.extend(chunk(b"IEND", b""))
    return bytes(png), bytes(expected)


class PngCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="psvr2-png-tests-")
        library = Path(cls.temporary.name) / "codec.so"
        subprocess.run(["cc", "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra",
                        "-Wconversion", "-Werror", str(CODEC / "png.c"), "-o", str(library)], check=True)
        cls.codec = ctypes.CDLL(str(library))
        cls.codec.psvr2_png_decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Image)]
        cls.codec.psvr2_png_decode.restype = ctypes.c_bool
        cls.codec.psvr2_png_load.argtypes = [ctypes.c_char_p, ctypes.POINTER(Image)]
        cls.codec.psvr2_png_load.restype = ctypes.c_bool
        cls.codec.psvr2_png_free.argtypes = [ctypes.POINTER(Image)]

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def decode(self, png):
        data = ctypes.create_string_buffer(bytes(png))
        image = Image()
        success = self.codec.psvr2_png_decode(data, len(png), ctypes.byref(image))
        result = ctypes.string_at(image.pixels, image.width * image.height * 4) if success else b""
        dimensions = (image.width, image.height)
        if not success:
            self.assertFalse(image.pixels)
            self.assertEqual(dimensions, (0, 0))
        self.codec.psvr2_png_free(ctypes.byref(image))
        return success, dimensions, result

    def test_all_color_types_depths_filters_and_interlace(self):
        formats = [(0, depth) for depth in (1, 2, 4, 8, 16)]
        formats += [(3, depth) for depth in (1, 2, 4, 8)]
        formats += [(color, depth) for color in (2, 4, 6) for depth in (8, 16)]
        for color, depth in formats:
            for interlaced in (False, True):
                for kind in range(5):
                    with self.subTest(color=color, depth=depth, interlaced=interlaced, filter=kind):
                        png, pixels = fixture(19, 13, color, depth, interlaced, kind)
                        self.assertEqual(self.decode(png), (True, (19, 13), pixels))

    def test_stored_fixed_and_dynamic_deflate_with_large_repeated_history(self):
        for compression in ("stored", "fixed", "dynamic"):
            png, pixels = fixture(301, 113, 6, 8, filter_kind=4, compression=compression)
            self.assertEqual(self.decode(png), (True, (301, 113), pixels))

    def test_small_adam7_passes_and_opaque_palette(self):
        for width, height in ((1, 1), (1, 9), (9, 1), (2, 3), (7, 5)):
            png, pixels = fixture(width, height, 3, 4, interlaced=True, transparent=False)
            self.assertEqual(self.decode(png), (True, (width, height), pixels))

    def test_file_ownership_and_missing_file(self):
        png, pixels = fixture(3, 2, 6, 16)
        path = Path(self.temporary.name) / "generated.png"
        path.write_bytes(png)
        image = Image()
        self.assertTrue(self.codec.psvr2_png_load(bytes(path), ctypes.byref(image)))
        self.assertEqual(ctypes.string_at(image.pixels, 24), pixels)
        self.codec.psvr2_png_free(ctypes.byref(image))
        self.assertFalse(image.pixels)
        self.assertFalse(self.codec.psvr2_png_load(bytes(path.with_suffix(".missing")), ctypes.byref(image)))

    def test_truncation_crc_and_trailing_bytes(self):
        png, _ = fixture(11, 7, 6, 8)
        for length in range(len(png)):
            self.assertFalse(self.decode(png[:length])[0])
        corrupted = bytearray(png)
        corrupted[29] ^= 1
        self.assertFalse(self.decode(corrupted)[0])
        self.assertFalse(self.decode(png + b"extra")[0])

    def test_unknown_critical_chunks_and_nonconsecutive_data(self):
        png, _ = fixture(4, 4, 2, 8)
        ihdr_end = 33
        self.assertFalse(self.decode(png[:ihdr_end] + chunk(b"ABCD", b"") + png[ihdr_end:])[0])
        self.assertTrue(self.decode(png[:ihdr_end] + chunk(b"abCD", b"") + png[ihdr_end:])[0])
        first_data = png.index(b"IDAT") - 4
        first_end = first_data + struct.unpack_from(">I", png, first_data)[0] + 12
        self.assertFalse(self.decode(png[:first_end] + chunk(b"tEXt", b"tag\0value") + png[first_end:])[0])

    def test_invalid_format_and_resource_limits(self):
        for width, height, depth, color, interlace in ((0, 1, 8, 6, 0),
                (8193, 1, 8, 6, 0), (8192, 8192, 8, 6, 0),
                (2, 2, 4, 2, 0), (2, 2, 8, 7, 0), (2, 2, 8, 6, 2)):
            png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, interlace))
            png += chunk(b"IDAT", zlib.compress(b"\0")) + chunk(b"IEND", b"")
            self.assertFalse(self.decode(png)[0])

    def test_deterministic_mutations_never_leave_partial_output(self):
        png, _ = fixture(8, 8, 6, 8)
        generator = random.Random(1)
        for _ in range(200):
            changed = bytearray(png)
            changed[generator.randrange(len(changed))] ^= 1 << generator.randrange(8)
            self.decode(changed)

    def test_malformed_deflate_after_valid_chunk_checksums(self):
        for png in bad_deflate_fixtures():
            self.assertFalse(self.decode(png)[0])
        # Keep PNG CRCs valid while changing the compressed bitstream so this
        # also exercises the inflater rather than stopping at the chunk layer.
        generator = random.Random(2)
        compressed = zlib.compress(b"\0\1\2\3\4")
        for _ in range(300):
            changed = bytearray(compressed)
            changed[generator.randrange(len(changed))] ^= 1 << generator.randrange(8)
            self.decode(compressed_fixture(bytes(changed)))

    def test_palette_cannot_follow_transparency(self):
        png, _ = fixture(2, 2, 2, 8)
        offset = png.index(b"IDAT") - 4
        self.assertFalse(self.decode(png[:offset] + chunk(b"PLTE", b"\0\0\0") + png[offset:])[0])

    def test_sanitized_valid_and_invalid_inputs(self):
        harness = Path(self.temporary.name) / "sanitized-png"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I", str(CODEC), str(ROOT / "tests/codec_png_harness.c"),
                        str(CODEC / "png.c"), "-o", str(harness)], check=True)
        cases = [(True, fixture(19, 13, color, depth, True, kind)[0])
                 for color, depth in ((0, 1), (3, 4), (2, 8), (4, 16), (6, 16))
                 for kind in range(5)]
        cases += [(False, data) for data in bad_deflate_fixtures()]
        valid, _ = fixture(7, 5, 6, 8)
        cases += [(False, valid[:length]) for length in range(len(valid))]
        command = [str(harness)]
        for index, (expected, png) in enumerate(cases):
            path = Path(self.temporary.name) / f"sanitizer-{index}.png"
            path.write_bytes(png)
            command += [str(int(expected)), str(path)]
        subprocess.run(command, check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
