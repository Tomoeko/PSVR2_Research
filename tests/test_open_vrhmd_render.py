"""Host rendering/stream regressions, without opening headset devices or EGL.

The C harness compares matrix transforms to reference math, checks padded MPEG
plane uploads through fake GL callbacks, and sends full packets through real
nonblocking sockets with constrained send buffers. Address/undefined sanitizers
cover the source functions and harness memory accesses.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "target/psvr2/tools/open_vrhmd"


class OpenVrhmdRenderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("a host C compiler is required")
        cls.temporary = tempfile.TemporaryDirectory(prefix="open-vrhmd-render-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "render-harness"
        flags = ["-std=gnu11", "-DGPU_RENDER", "-DPSVR2_SOURCE_FAMILY_0600=1",
                 "-O1", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if sys.platform == "darwin":
            # Linux target semaphores are deprecated only in the macOS SDK.
            flags.append("-Wno-deprecated-declarations")
        result = subprocess.run(
            [compiler, *flags,
             str(ROOT / "tests/fixtures/open_vrhmd_render_harness.c"),
             str(SOURCE / "render/matrix.c"),
             str(SOURCE / "render/color.c"),
             str(SOURCE / "render/media.c"),
             str(SOURCE / "stream/video.c"),
             str(SOURCE / "stream/mirrorscope.c"),
             "-pthread", "-lm", "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode:
            raise AssertionError(result.stderr)

    def run_check(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "render/stream check passed")

    def test_matrix_aliasing_and_optimized_model_equivalence(self):
        self.run_check("matrices")

    def test_hue_wraparound(self):
        self.run_check("colors")

    def test_mpeg_uploads_padded_plane_dimensions(self):
        self.run_check("video")

    def test_png_row_orientation_preserves_rgba_and_middle_row(self):
        self.run_check("png")

    def test_stream_short_writes_stalls_skips_and_cancellation(self):
        self.run_check("stream")


if __name__ == "__main__":
    unittest.main()
