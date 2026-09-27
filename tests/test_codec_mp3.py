"""First-party MP3 decode tests; all compressed/audio data stays in temp storage.

FFmpeg is an independent encoder/decoder oracle, never a runtime dependency.
Run: python3 tests/test_codec_mp3.py
"""
import array
import math
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
CODEC = ROOT / "target/psvr2/tools/open_vrhmd/codec"


def frame_info(data):
    """Independent header/side-information inventory of generated vectors."""
    pos = 0
    if data[:3] == b"ID3":
        pos = 10 + sum((data[6 + i] & 127) << (7 * (3 - i)) for i in range(4))
    frames, short, reservoir, tables = [], 0, 0, set()
    while pos + 4 <= len(data):
        h = int.from_bytes(data[pos:pos + 4], "big")
        if h >> 21 != 0x7ff:
            break
        rate = (44100, 48000, 32000)[(h >> 10) & 3]
        bitrate = (0,32,40,48,56,64,80,96,112,128,160,192,224,256,320)[(h >> 12) & 15]
        size = 144000 * bitrate // rate + ((h >> 9) & 1)
        channels = 1 if ((h >> 6) & 3) == 3 else 2
        side_start = pos + 4 + (0 if h & (1 << 16) else 2)
        side = data[side_start:side_start + (17 if channels == 1 else 32)]
        bits = "".join(f"{v:08b}" for v in side)
        cursor = 0
        def take(n):
            nonlocal cursor
            v = int(bits[cursor:cursor+n], 2)
            cursor += n
            return v
        back = take(9)
        reservoir += back != 0
        take(5 if channels == 1 else 3)
        for _ in range(channels): take(4)
        for _ in range(2 * channels):
            take(12); take(9); take(8); take(4)
            if take(1):
                block = take(2); take(1)
                short += block == 2
                tables.update((take(5), take(5)))
                for _ in range(3): take(3)
            else:
                tables.update((take(5), take(5), take(5)))
                take(4); take(3)
            take(1); take(1); take(1)
        frames.append((pos, size, channels, (h >> 6) & 3))
        pos += size
    return frames, short, reservoir, tables


class Mp3Codec(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="psvr2-mp3-")
        cls.dir = Path(cls.temp.name)
        cls.decoder = cls.dir / "decode"
        compiler = shutil.which(os.environ.get("CC", "cc"))
        if not compiler:
            raise unittest.SkipTest("C11 compiler unavailable")
        subprocess.run([
            compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic",
            "-Werror", "-Wconversion", "-Wshadow", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-I", str(CODEC),
            str(ROOT / "tests/fixtures/codec_mp3_harness.c"), str(CODEC / "mp3.c"),
            "-lm", "-o", str(cls.decoder),
        ], check=True, capture_output=True)
        cls.env = dict(os.environ, ASAN_OPTIONS=("detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1") + ":abort_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        cls.ffmpeg = shutil.which("ffmpeg")
        cls.vectors = {}
        if cls.ffmpeg:
            for rate in (32000, 44100, 48000):
                for channels in (1, 2):
                    wav = cls.dir / f"generated-{rate}-{channels}.wav"
                    rng = random.Random(1907 + rate + channels)
                    samples = array.array("h")
                    # Steady tones, strong onsets, silence, and seeded noise
                    # exercise long and switched short blocks and the reservoir.
                    for i in range(rate * 3 // 5):
                        time = i / rate
                        envelope = 0.0 if .22 < time < .25 else 1.0
                        onset = (i % (rate // 10)) < 300
                        noise = .24 * rng.uniform(-1, 1) if onset else 0.0
                        for ch in range(channels):
                            value = envelope * (.19 * math.sin(2 * math.pi * (997 + 127 * ch) * time)
                                                + .09 * math.sin(2 * math.pi * (5031 - 741 * ch) * time)
                                                + noise)
                            samples.append(round(32767 * value))
                    with wave.open(str(wav), "wb") as out:
                        out.setnchannels(channels); out.setsampwidth(2)
                        out.setframerate(rate); out.writeframes(samples.tobytes())
                    for vbr in (False, True):
                        mp3 = cls.dir / f"vector-{rate}-{channels}-{int(vbr)}.mp3"
                        args = [cls.ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(wav),
                                "-c:a", "libmp3lame", "-joint_stereo", "1" if vbr else "0"]
                        args += ["-q:a", "2"] if vbr else ["-b:a", "192k"]
                        subprocess.run(args + ["-write_xing", "1" if vbr else "0", "-y", str(mp3)],
                                       check=True, capture_output=True)
                        ref = subprocess.run([cls.ffmpeg, "-hide_banner", "-loglevel", "error",
                                              "-i", str(mp3), "-f", "s16le", "-"],
                                             check=True, capture_output=True).stdout
                        cls.vectors[(rate, channels, vbr)] = (mp3, ref)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def decode(self, path, chunk=173):
        result = subprocess.run([str(self.decoder), str(path), str(chunk)],
                                capture_output=True, env=self.env, timeout=20)
        self.assertNotIn(b"AddressSanitizer", result.stderr)
        self.assertNotIn(b"runtime error:", result.stderr)
        return result

    def require_vectors(self):
        if not self.vectors:
            self.skipTest("FFmpeg with libmp3lame unavailable")

    def test_waveforms_all_rates_mono_stereo_joint_cbr_vbr(self):
        self.require_vectors()
        seen_short = seen_reservoir = 0
        for (rate, channels, vbr), (path, reference) in self.vectors.items():
            with self.subTest(rate=rate, channels=channels, vbr=vbr):
                result = self.decode(path)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(len(result.stdout), len(reference))
                ours, expected = array.array("h", result.stdout), array.array("h", reference)
                delta = [int(a) - int(b) for a, b in zip(ours, expected)]
                rms = math.sqrt(sum(v*v for v in delta) / len(delta))
                # FFmpeg's stereo s16 conversion rounds differently on some hosts;
                # preserve a tight absolute PCM bound as well as waveform RMS.
                self.assertLessEqual(max(map(abs, delta)), 2)
                self.assertLess(rms, .8)
                frames, short, reservoir, _ = frame_info(path.read_bytes())
                self.assertGreater(len(frames), 10)
                self.assertIn(1 if channels == 2 and vbr else 3 if channels == 1 else 0,
                              {frame[3] for frame in frames})
                seen_short += short; seen_reservoir += reservoir
        self.assertGreater(seen_short, 0)
        self.assertGreater(seen_reservoir, 0)

    def test_intensity_and_mixed_short_blocks_against_reference(self):
        if not self.ffmpeg:
            self.skipTest("FFmpeg unavailable")
        def packed(text):
            text += "0" * ((-len(text)) % 8)
            return bytes(int(text[i:i+8], 2) for i in range(0, len(text), 8))
        for block in (0, 2, 3):  # 3 here denotes mixed short blocks
            for extension in (1, 3):  # intensity; intensity plus M/S
                for position in range(8):
                    mixed = int(block == 3)
                    scalefactors = ([4] * 11 + [3] * 10 if not block else
                                    [4] * 17 + [3] * 18 if mixed else
                                    [4] * 18 + [3] * 18)
                    right = "".join(f"{position:0{width}b}" for width in scalefactors)
                    # Huffman table 1, pair (1,1), two positive signs.
                    left = "00000"
                    def granule(ch):
                        fields = [(len(right) if ch else len(left), 12),
                                  (0 if ch else 1, 9), (210, 8), (15 if ch else 0, 4),
                                  (int(bool(block)), 1)]
                        if block:
                            fields += [(2, 2), (mixed, 1), (0 if ch else 1, 5), (0, 5),
                                       (0, 3), (0, 3), (0, 3)]
                        else:
                            fields += [(0 if ch else 1, 5), (0, 5), (0, 5), (0, 4), (0, 3)]
                        fields += [(0, 1), (0, 1), (0, 1)]
                        return "".join(f"{v:0{n}b}" for v, n in fields)
                    side = packed("0" * 20 + granule(0) + granule(1) + granule(0) + granule(1))
                    self.assertEqual(len(side), 32)
                    main = packed(left + right + left + right)
                    header = bytes((0xff, 0xfb, 0x94, 0x40 | extension << 4))
                    frame = header + side + main + bytes(384 - 36 - len(main))
                    path = self.dir / "intensity.mp3"; path.write_bytes(frame * 4)
                    reference = subprocess.run([self.ffmpeg, "-hide_banner", "-loglevel", "error",
                                                "-i", str(path), "-f", "s16le", "-"],
                                               capture_output=True, check=True).stdout
                    with self.subTest(block=block, extension=extension, position=position):
                        result = self.decode(path)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(len(result.stdout), len(reference))
                        ours = array.array("h", result.stdout); expected = array.array("h", reference)
                        self.assertLessEqual(max(abs(int(a)-int(b)) for a,b in zip(ours, expected)), 2)

    def test_extended_huffman_magnitudes_against_reference(self):
        if not self.ffmpeg:
            self.skipTest("FFmpeg unavailable")
        def packed(text):
            text += "0" * ((-len(text)) % 8)
            return bytes(int(text[i:i+8], 2) for i in range(0, len(text), 8))
        # Annex B escape codebooks: (15,15) followed by 13-bit extensions
        # reaches the maximum representable magnitude, 8206, with both signs.
        for table, code in ((23, "00000011"), (31, "0011")):
            main = code + "1" * 13 + "1" + "1" * 13 + "0"
            fields = [(len(main),12),(1,9),(120,8),(0,4),(0,1),
                      (table,5),(0,5),(0,5),(0,4),(0,3),(0,1),(0,1),(0,1)]
            granule = "".join(f"{v:0{n}b}" for v,n in fields)
            side = packed("0" * 18 + granule * 2)
            self.assertEqual(len(side), 17)
            payload = packed(main * 2)
            frame = bytes.fromhex("fffb94c0") + side + payload + bytes(384-21-len(payload))
            path = self.dir / "escape.mp3"; path.write_bytes(frame * 4)
            reference = subprocess.run([self.ffmpeg,"-hide_banner","-loglevel","error",
                                        "-i",str(path),"-f","s16le","-"],
                                       capture_output=True,check=True).stdout
            with self.subTest(table=table):
                result = self.decode(path)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(len(result.stdout),len(reference))
                ours=array.array("h",result.stdout); expected=array.array("h",reference)
                self.assertGreater(max(map(abs,ours)),0)
                self.assertLessEqual(max(abs(int(a)-int(b)) for a,b in zip(ours,expected)),2)

    def test_arbitrary_read_boundaries(self):
        self.require_vectors()
        path, _ = self.vectors[(44100, 2, True)]
        expected = self.decode(path, 8192)
        self.assertEqual(expected.returncode, 0, expected.stderr)
        for size in (1, 17, 1151, 1152, 1153):
            with self.subTest(size=size):
                got = self.decode(path, size)
                self.assertEqual(got.returncode, 0, got.stderr)
                self.assertEqual(got.stdout, expected.stdout)

    def test_structural_truncation_and_invalid_headers(self):
        self.require_vectors()
        path, _ = self.vectors[(48000, 2, True)]
        data = path.read_bytes()
        frames, _, _, _ = frame_info(data)
        for cut in (0, 1, 2, 3, 9, frames[0][0] + 3,
                    frames[0][0] + frames[0][1] - 1, len(data) - 1, len(data) - 32):
            with self.subTest(cut=cut):
                f = self.dir / "truncated.mp3"; f.write_bytes(data[:cut])
                self.assertIn(self.decode(f).returncode, (3, 4))
        offset = frames[1][0]
        for byte, mask in ((1, 0x18), (2, 0x0c), (2, 0xf0)):
            changed = bytearray(data); changed[offset+byte] ^= mask
            f = self.dir / "invalid-header.mp3"; f.write_bytes(changed)
            with self.subTest(byte=byte, mask=mask):
                self.assertIn(self.decode(f).returncode, (3, 4))
        changed = bytearray(data)
        changed[offset + 3] = (changed[offset + 3] & 0x3f) | 0xc0
        f = self.dir / "changed-channels.mp3"; f.write_bytes(changed)
        self.assertIn(self.decode(f).returncode, (3, 4))
        changed = bytearray(data)
        # main_data_begin=511 on the first audio frame cannot reference a
        # reservoir shorter than the preceding physical metadata frame.
        offset = frames[1][0] + 4
        changed[offset] = 255; changed[offset + 1] |= 128
        f = self.dir / "invalid-reservoir.mp3"; f.write_bytes(changed)
        self.assertIn(self.decode(f).returncode, (3, 4))

    def test_crc_and_unsupported_modes_fail_explicitly(self):
        # A complete silent frame with CRC, constructed from the bit syntax.
        header = bytes.fromhex("fffa9400")  # MPEG1 L3, 128k, 48k stereo, CRC
        side = bytes(32)
        def crc(data):
            value = 0xffff
            for byte in data:
                for shift in range(7, -1, -1):
                    carry = ((value >> 15) ^ (byte >> shift)) & 1
                    value = (value << 1) & 65535
                    if carry: value ^= 0x8005
            return value
        checksum = crc(header[2:] + side)
        data = header + checksum.to_bytes(2, "big") + side + bytes(384-38)
        f = self.dir / "crc.mp3"; f.write_bytes(data)
        good = self.decode(f)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertEqual(good.stdout, bytes(1152 * 2 * 2))
        wrong = bytearray(data); wrong[4] ^= 1; f.write_bytes(wrong)
        self.assertEqual(self.decode(f).returncode, 3)
        for index, value in ((1, 0xf2), (2, 0x04), (3, 0x01)):
            wrong = bytearray(data); wrong[index] = value; f.write_bytes(wrong)
            self.assertEqual(self.decode(f).returncode, 3)

    def test_sanitized_mutation_and_random_input(self):
        self.require_vectors()
        data = self.vectors[(32000, 2, False)][0].read_bytes()
        rng = random.Random(7147)
        for index in range(96):
            changed = bytearray(data)
            if index < 48:
                for _ in range(1 + index % 9):
                    where = rng.randrange(len(changed)); changed[where] ^= 1 << rng.randrange(8)
            else:
                changed = bytearray(rng.randbytes(rng.randrange(0, 4096)))
            f = self.dir / "mutation.mp3"; f.write_bytes(changed)
            result = self.decode(f)
            # Entropy-coded main-data bit changes can remain structurally valid;
            # those are decoded, never substituted with concealed/silent audio.
            self.assertIn(result.returncode, (0, 3, 4), result.stderr)
            self.assertLessEqual(len(result.stdout), len(data) * 48)


if __name__ == "__main__":
    unittest.main()
