"""Guards for the Wii Menu's native PSVR2 build entry point."""

from __future__ import annotations

import dataclasses
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("wii_menu_psvr2_build", ROOT / "tools/psvr2_build.py")
assert SPEC and SPEC.loader
builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = builder
SPEC.loader.exec_module(builder)


class WiiMenuBuildTests(unittest.TestCase):
    def settings(self, root: Path):
        return builder.Settings(
            firmware="06.00", kernel_source=None, kernel_build=root / "kernel",
            cross_prefix="aarch64-linux-gnu-", tool_cc=("/sdk/zig", "cc"),
            glibc_version="2.28", make="make", jobs=2, sysroot=None,
            dynamic_linker="/lib/ld-2.28.so", host_paths=(),
            source_archive_sha256=None, verbose=False,
        )

    def test_zig_uses_device_glibc_floor(self):
        with mock.patch.object(builder, "executable", return_value="/sdk/zig"):
            command = builder.wii_menu_compiler(self.settings(ROOT))
        self.assertEqual(command, ["/sdk/zig", "cc", "-target", "aarch64-linux-gnu.2.28"])

    def test_cross_gcc_keeps_sysroot(self):
        settings = dataclasses.replace(self.settings(ROOT), tool_cc=("/sdk/gcc",),
                                       sysroot=Path("/sdk/device"))
        with mock.patch.object(builder, "executable", return_value="/sdk/gcc"):
            command = builder.wii_menu_compiler(settings)
        self.assertEqual(command, ["/sdk/gcc", "--sysroot=/sdk/device"])

    def test_graphics_inputs_reject_host_libraries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "project"
            for relative in ("CMakeLists.txt", "src/CMakeLists.txt", "cmake/WiiMenuBackend.cmake",
                             "src/platform/psvr2/gl_api.h", "src/platform/psvr2/vr_layout.c"):
                path = project / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("synthetic source\n")
            libraries = root / "runtime/lib"
            libraries.mkdir(parents=True)
            header = bytearray(20)
            header[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", header, 18, 183)
            for name in ("libEGL.so.1", "libGLESv2.so.2"):
                (libraries / name).write_bytes(header)
            self.assertEqual(builder.wii_menu_inputs(project, root / "runtime"),
                             (libraries / "libEGL.so.1", libraries / "libGLESv2.so.2"))
            struct.pack_into("<H", header, 18, 62)
            (libraries / "libGLESv2.so.2").write_bytes(header)
            with self.assertRaisesRegex(builder.BuildError, "not AArch64 ELF"):
                builder.wii_menu_inputs(project, root / "runtime")

    def test_unverified_firmware_rejected_before_cmake(self):
        arguments = builder.build_parser().parse_args([
            "wii-menu", "--project", "/unused", "--firmware", "01.10",
        ])
        settings = dataclasses.replace(self.settings(ROOT), firmware="01.10")
        with mock.patch.object(builder, "resolve_settings", return_value=settings):
            with self.assertRaisesRegex(builder.BuildError, "06.00 only"):
                builder.cmd_wii_menu.__wrapped__(arguments)


if __name__ == "__main__":
    unittest.main()
