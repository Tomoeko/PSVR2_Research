"""Generate JPEG inputs; compare pixels against an independent test-only decoder."""

import ctypes
import io
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

try:
    from PIL import Image as PillowImage
except ImportError:
    PillowImage = None

ROOT = Path(__file__).resolve().parents[1]
CODEC = ROOT / "target/psvr2/tools/open_vrhmd/codec"


class Image(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
                ("pixels", ctypes.POINTER(ctypes.c_ubyte))]


def segment(kind, body):
    return bytes((255, kind)) + struct.pack(">H", len(body) + 2) + body


def flat_fixture(width=1, height=1, progressive=False, restart=False):
    # Author a tiny JPEG directly: DC difference zero, AC end-of-block.
    quant = segment(0xdb, b"\0" + bytes([1]) * 64)
    frame = segment(0xc2 if progressive else 0xc0,
                    bytes([8]) + struct.pack(">HH", height, width) + b"\1\1\x11\0")
    counts = bytes([1]) + bytes(15)
    tables = segment(0xc4, b"\0" + counts + b"\0\x10" + counts + b"\0")
    prefix = b"\xff\xd8" + quant + frame + tables
    blocks = ((width + 7) // 8) * ((height + 7) // 8)
    if restart:
        prefix += segment(0xdd, b"\0\1")
    def entropy(bits_per_block):
        if restart:
            return b"".join(bytes([(1 << (8 - bits_per_block)) - 1]) +
                            (bytes([255, 0xd0 + (index % 8)]) if index + 1 < blocks else b"")
                            for index in range(blocks))
        bits = "0" * (blocks * bits_per_block)
        bits += "1" * ((-len(bits)) % 8)
        return bytes(int(bits[index:index + 8], 2) for index in range(0, len(bits), 8))
    if progressive:
        scans = ((0, 0, 0, 1), (1, 63, 0, 1), (0, 0, 1, 0), (1, 63, 1, 0))
        data = b"".join(segment(0xda, bytes((1, 1, 0, first, last, high * 16 + low))) +
                        entropy(1) for first, last, high, low in scans)
    else:
        data = segment(0xda, b"\1\1\0\0\x3f\0") + entropy(2)
    return prefix + data + b"\xff\xd9"


def generated_fixture(mode, width, height, progressive=False, subsampling=0, restart=0):
    image = PillowImage.new(mode, (width, height))
    channels = len(mode)
    values = []
    for y in range(height):
        for x in range(width):
            value = tuple((x * 13 + y * 3 + index * 53) % 256 for index in range(channels))
            values.append(value[0] if channels == 1 else value)
    image.putdata(values)
    stream = io.BytesIO()
    image.save(stream, format="JPEG", quality=88, progressive=progressive,
               subsampling=subsampling, restart_marker_blocks=restart)
    return stream.getvalue()


class JpegCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="psvr2-jpeg-tests-")
        library = Path(cls.temporary.name) / "codec.so"
        subprocess.run(["cc", "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra",
                        "-Wconversion", "-Werror", str(CODEC / "jpeg.c"),
                        str(CODEC / "png.c"), "-lm", "-o", str(library)], check=True)
        cls.codec = ctypes.CDLL(str(library))
        cls.codec.psvr2_jpeg_decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Image)]
        cls.codec.psvr2_jpeg_decode.restype = ctypes.c_bool
        cls.codec.psvr2_jpeg_load.argtypes = [ctypes.c_char_p, ctypes.POINTER(Image)]
        cls.codec.psvr2_jpeg_load.restype = ctypes.c_bool
        cls.codec.psvr2_png_free.argtypes = [ctypes.POINTER(Image)]

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def decode(self, jpeg):
        data = ctypes.create_string_buffer(bytes(jpeg))
        image = Image()
        success = self.codec.psvr2_jpeg_decode(data, len(jpeg), ctypes.byref(image))
        pixels = ctypes.string_at(image.pixels, image.width * image.height * 4) if success else b""
        dimensions = (image.width, image.height)
        if not success:
            self.assertFalse(image.pixels)
            self.assertEqual(dimensions, (0, 0))
        self.codec.psvr2_png_free(ctypes.byref(image))
        return success, dimensions, pixels

    def test_authored_flat_images_sequential_progressive_and_restart(self):
        for width, height in ((1, 1), (8, 8), (17, 9)):
            for progressive in (False, True):
                for restart in (False, True):
                    data = flat_fixture(width, height, progressive, restart)
                    self.assertEqual(self.decode(data), (True, (width, height),
                                     bytes((128, 128, 128, 255)) * width * height))

    @unittest.skipUnless(PillowImage, "Pillow is an optional independent JPEG test oracle")
    def test_grayscale_rgb_cmyk_baseline_progressive_and_subsampling(self):
        for mode in ("L", "RGB", "CMYK"):
            for progressive in (False, True):
                for subsampling in (0, 1, 2):
                    with self.subTest(mode=mode, progressive=progressive, subsampling=subsampling):
                        jpeg = generated_fixture(mode, 37, 29, progressive, subsampling)
                        success, dimensions, actual = self.decode(jpeg)
                        self.assertTrue(success)
                        self.assertEqual(dimensions, (37, 29))
                        expected = PillowImage.open(io.BytesIO(jpeg)).convert("RGBA").tobytes()
                        errors = [abs(a - b) for a, b in zip(actual, expected)]
                        # IDCT rounding and chroma interpolation differ between implementations.
                        self.assertLessEqual(max(errors), 3)
                        self.assertLess(sum(errors) / len(errors), 0.5)

    @unittest.skipUnless(PillowImage, "Pillow is an optional independent JPEG test oracle")
    def test_generated_restart_intervals(self):
        for progressive in (False, True):
            for restart in (1, 3, 7):
                jpeg = generated_fixture("RGB", 81, 47, progressive, 2, restart)
                success, dimensions, actual = self.decode(jpeg)
                self.assertTrue(success)
                self.assertEqual(dimensions, (81, 47))
                expected = PillowImage.open(io.BytesIO(jpeg)).convert("RGBA").tobytes()
                self.assertLessEqual(max(abs(a - b) for a, b in zip(actual, expected)), 3)

    def test_truncation_invalid_restart_and_unsupported_precision(self):
        jpeg = flat_fixture(17, 9, restart=True)
        for length in range(len(jpeg)):
            self.assertFalse(self.decode(jpeg[:length])[0])
        self.assertFalse(self.decode(jpeg.replace(b"\xff\xd0", b"\xff\xd3", 1))[0])
        modified = bytearray(jpeg)
        modified[modified.index(b"\xff\xc0") + 4] = 12
        self.assertFalse(self.decode(modified)[0])
        self.assertFalse(self.decode(jpeg + b"extra")[0])

    def test_bounds_and_huffman_overflow(self):
        jpeg = flat_fixture()
        invalid_frame = segment(0xc0, b"\x08\xff\xff\xff\xff\1\1\x11\0")
        start = jpeg.index(b"\xff\xc0")
        end = start + 2 + struct.unpack_from(">H", jpeg, start + 2)[0]
        self.assertFalse(self.decode(jpeg[:start] + invalid_frame + jpeg[end:])[0])
        table = jpeg.index(b"\xff\xc4")
        modified = bytearray(jpeg)
        modified[table + 5] = 3
        self.assertFalse(self.decode(modified)[0])
        generator = random.Random(7)
        for _ in range(300):
            modified = bytearray(jpeg)
            modified[generator.randrange(len(modified))] ^= 1 << generator.randrange(8)
            self.decode(modified)

    def test_quantization_is_captured_per_component(self):
        prefix = b"\xff\xd8" + segment(0xdb, b"\0" + bytes([1]) * 64)
        prefix += segment(0xc0, b"\x08\0\1\0\1\3R\x11\0G\x11\0B\x11\0")
        counts = bytes([1]) + bytes(15)
        prefix += segment(0xc4, b"\0" + counts + b"\4\x10" + counts + b"\0")
        # Each component has DC=8. A later scan redefines the same table id,
        # which must not alter components already decoded with the earlier one.
        data = prefix
        for index, quant in enumerate((1, 8, 16)):
            if index:
                data += segment(0xdb, b"\0" + bytes([quant]) * 64)
            data += segment(0xda, bytes((1, ord("RGB"[index]), 0, 0, 63, 0))) + b"\x43"
        data += b"\xff\xd9"
        self.assertEqual(self.decode(data), (True, (1, 1), bytes((129, 136, 144, 255))))

    def test_progressive_quantization_change_is_rejected(self):
        data = flat_fixture(progressive=True)
        first_scan = data.index(b"\xff\xda")
        second_scan = data.index(b"\xff\xda", first_scan + 2)
        changed = data[:second_scan] + segment(0xdb, b"\0" + bytes([2]) * 64) + data[second_scan:]
        self.assertFalse(self.decode(changed)[0])

    def test_adobe_ycck_and_invalid_color_transform(self):
        counts = bytes([1]) + bytes(15)
        prefix = b"\xff\xd8" + segment(0xee, b"Adobe\0\x64\0\0\0\0\2")
        prefix += segment(0xdb, b"\0" + bytes([1]) * 64)
        prefix += segment(0xc0, b"\x08\0\1\0\1\4\1\x11\0\2\x11\0\3\x11\0\4\x11\0")
        prefix += segment(0xc4, b"\0" + counts + b"\0\x10" + counts + b"\0")
        data = prefix + segment(0xda, b"\4\1\0\2\0\3\0\4\0\0\x3f\0") + b"\0\xff\xd9"
        self.assertEqual(self.decode(data), (True, (1, 1), bytes((64, 64, 64, 255))))
        if PillowImage:
            expected = PillowImage.open(io.BytesIO(data)).convert("RGBA").tobytes()
            self.assertEqual(expected, bytes((64, 64, 64, 255)))
        modified = bytearray(data)
        modified[17] = 1
        self.assertFalse(self.decode(modified)[0])

    def test_file_loader_and_sanitized_malformed_inputs(self):
        jpeg = flat_fixture(17, 9, progressive=True, restart=True)
        path = Path(self.temporary.name) / "generated.jpg"
        path.write_bytes(jpeg)
        image = Image()
        self.assertTrue(self.codec.psvr2_jpeg_load(bytes(path), ctypes.byref(image)))
        self.codec.psvr2_png_free(ctypes.byref(image))
        self.assertFalse(self.codec.psvr2_jpeg_load(bytes(path.with_suffix(".missing")), ctypes.byref(image)))
        harness = Path(self.temporary.name) / "sanitized-jpeg"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I", str(CODEC), str(ROOT / "tests/codec_jpeg_harness.c"),
                        str(CODEC / "jpeg.c"), str(CODEC / "png.c"), "-lm",
                        "-o", str(harness)], check=True)
        cases = [(True, flat_fixture(17, 9, progressive, restart))
                 for progressive in (False, True) for restart in (False, True)]
        cases += [(False, jpeg[:length]) for length in range(len(jpeg))]
        command = [str(harness)]
        for index, (expected, data) in enumerate(cases):
            path = Path(self.temporary.name) / f"sanitizer-{index}.jpg"
            path.write_bytes(data)
            command += [str(int(expected)), str(path)]
        subprocess.run(command, check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
