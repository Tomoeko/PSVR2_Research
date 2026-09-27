"""Validate the authored decoder with locally generated MPEG1 clips.

FFmpeg is an independent test encoder/reference decoder, never a runtime
component. Every clip comes from deterministic synthetic filters; no media
fixture is included in the repository.
"""
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("clang"),
                     "requires host clang and FFmpeg for independent validation")
class MpegCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="mpeg-validation-")
        cls.work = Path(cls.directory.name)
        cls.decoder = cls.work / "decode"
        subprocess.run([
            "clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(ROOT / "tests/fixtures/codec_mpeg_harness.c"),
            str(ROOT / "target/psvr2/tools/open_vrhmd/codec/mpeg.c"),
            "-lm", "-o", str(cls.decoder)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def make_clip(self, name, source, *, frames=30, bf=2, gop=12, quant=3,
                  program=False, audio=False, rate="25", matrices=False):
        clip = self.work / (name + (".mpg" if program else ".m1v"))
        cmd = ["ffmpeg", "-v", "error", "-f", "lavfi", "-i", source]
        if audio:
            cmd += ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"]
        cmd += ["-frames:v", str(frames), "-r", rate, "-c:v", "mpeg1video",
                "-g", str(gop), "-bf", str(bf), "-q:v", str(quant)]
        if matrices:
            cmd += ["-intra_matrix", ",".join(str(8 + (i % 8) * 3 + (i // 8) * 2) for i in range(64)),
                    "-inter_matrix", ",".join(str(16 + (i % 5)) for i in range(64))]
        if audio:
            cmd += ["-c:a", "mp2", "-b:a", "128k", "-shortest"]
        cmd += ["-f", "mpeg" if program else "mpeg1video", "-y", str(clip)]
        subprocess.run(cmd, check=True)
        return clip

    def compare(self, clip, *, frames, width, height, rate):
        ours = self.work / (clip.stem + ".ours")
        reference = self.work / (clip.stem + ".reference")
        result = subprocess.run([str(self.decoder), str(clip), str(ours), "0.31"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = result.stdout.split()
        self.assertEqual([int(x) for x in fields[:3]], [frames, width, height])
        self.assertAlmostEqual(float(fields[3]), rate, places=6)
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(clip), "-an",
                        "-f", "rawvideo", "-pix_fmt", "yuv420p", "-vsync", "0",
                        "-y", str(reference)], check=True)
        a, b = ours.read_bytes(), reference.read_bytes()
        self.assertEqual(len(a), len(b))
        differences = [abs(x - y) for x, y in zip(a, b)]
        # ISO MPEG IDCT conformance permits small integer-rounding differences.
        # Comparisons cover all visible luma/chroma pixels, not just checksums.
        self.assertLessEqual(max(differences), 4)
        self.assertLess(sum(differences) / len(differences), 0.35)

    def test_normative_codes_and_negative_half_pixel_motion(self):
        binary = self.work / "syntax-checks"
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        str(ROOT / "tests/fixtures/codec_mpeg_syntax_harness.c"),
                        "-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_i_only_gradient(self):
        clip = self.make_clip("intra", "testsrc2=size=64x48:rate=25", bf=0, gop=1,
                              frames=8, quant=2)
        self.compare(clip, frames=8, width=64, height=48, rate=25)

    def test_p_frames_and_repeated_headers(self):
        clip = self.make_clip("predicted", "testsrc2=size=128x96:rate=25",
                              bf=0, gop=7, frames=35, quant=2)
        self.compare(clip, frames=35, width=128, height=96, rate=25)

    def test_b_frames_crop_and_fractional_rate(self):
        clip = self.make_clip("bidirectional", "testsrc2=size=98x70:rate=30000/1001",
                              frames=37, gop=10, quant=3, rate="30000/1001")
        self.compare(clip, frames=37, width=98, height=70, rate=30000 / 1001)

    def test_high_frequency_escape_coefficients(self):
        clip = self.make_clip("noise", "nullsrc=size=80x64:rate=25,noise=alls=100:allf=t+u:all_seed=19",
                              frames=10, gop=5, quant=1)
        self.compare(clip, frames=10, width=80, height=64, rate=25)

    def test_program_stream_interleaved_audio(self):
        clip = self.make_clip("program", "testsrc2=size=96x64:rate=25", program=True,
                              audio=True, frames=28)
        self.compare(clip, frames=28, width=96, height=64, rate=25)

    def test_custom_quantizers_and_reverse_motion(self):
        clip = self.make_clip("quantizers", "testsrc2=size=128x80:rate=25,hflip",
                              frames=41, gop=15, quant=4, matrices=True)
        self.compare(clip, frames=41, width=128, height=80, rate=25)

    def test_truncation_and_invalid_header_fail(self):
        clip = self.make_clip("malformed-source", "testsrc2=size=64x48:rate=25",
                              frames=12)
        contents = clip.read_bytes()
        huge = bytearray(contents)
        huge[4:7] = b"\xff\xff\xff"  # 4095 by 4095 exceeds the RAM budget.
        for name, data in [("truncated", contents[:-25]),
                           ("oversized-dimensions", bytes(huge)),
                           ("bad-sequence", b"\x00\x00\x01\xb3" + b"\x00" * 16),
                           ("junk", b"not a movie"),
                           ("unsupported", contents[:12] + b"\x00\x00\x01\xb5\x10" + contents[12:])]:
            with self.subTest(name=name):
                broken = self.work / name
                broken.write_bytes(data)
                result = subprocess.run([str(self.decoder), str(broken), str(self.work / "discard")],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(result.stderr.strip())


if __name__ == "__main__":
    unittest.main()
