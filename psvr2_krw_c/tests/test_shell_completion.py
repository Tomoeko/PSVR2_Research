"""Test context/quoting and actual read-only shell queries without a headset.

Pass the CMake-built harness as the first argument, or compile an isolated
temporary harness with the host compiler. Fixture changes are confined to a
temporary directory; the provider's remote queries only enumerate it.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="psvr2-completion-") as temporary:
        directory = Path(temporary)
        if len(sys.argv) > 1:
            binary = Path(sys.argv[1]).resolve()
        else:
            binary = directory / "completion-test"
            compiler = os.environ.get("CC") or shutil.which("clang") or "cc"
            usb = subprocess.check_output(
                ["pkg-config", "--cflags", "libusb-1.0"], text=True).split()
            subprocess.run([
                compiler, "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic",
                "-Wconversion", "-Wshadow", "-Werror", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / "src"), *usb,
                str(ROOT / "tests/shell_completion_harness.c"),
                str(ROOT / "src/shell_completion.c"), str(ROOT / "src/util.c"),
                "-o", str(binary),
            ], check=True)
        fixtures = directory / "fixtures"
        fixtures.mkdir()
        subprocess.run([str(binary), str(fixtures)], check=True, timeout=30)


if __name__ == "__main__":
    main()
