"""Run actual Stage1 USB helpers with fake native requests, never USB hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Stage1USBTests(unittest.TestCase):
    def test_publication_restoration_and_bounded_preparation(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("a host C compiler is required")
        with tempfile.TemporaryDirectory(prefix="stage1-usb-") as directory:
            binary = Path(directory) / "harness"
            result = subprocess.run(
                [compiler, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-fsanitize=address,undefined",
                 "-fno-omit-frame-pointer",
                 str(ROOT / "tests/fixtures/stage1_usb_harness.c"),
                 "-o", str(binary)], capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(),
                             "Stage1 USB lifecycle/preparation checks passed")


if __name__ == "__main__":
    unittest.main()
