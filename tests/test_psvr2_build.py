from __future__ import annotations

import argparse
import importlib.util
import io
import json
import shlex
import stat
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_TOOL_PATH = REPO_ROOT / "tools" / "psvr2_build.py"
SPEC = importlib.util.spec_from_file_location("psvr2_build", BUILD_TOOL_PATH)
assert SPEC and SPEC.loader
psvr2_build = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = psvr2_build
SPEC.loader.exec_module(psvr2_build)


class ManifestTests(unittest.TestCase):
    def test_maintained_profiles_modules_and_sources(self):
        manifest = psvr2_build.manifest()
        self.assertEqual(list(manifest["firmwares"]), ["01.10", "06.00"])
        self.assertEqual(set(manifest["modules"]), {"stage1", "stage3_serial", "rmmod_helper"})
        self.assertEqual(set(manifest["tools"]), {"busybox", "open_vrhmd", "input_verify", "kill"})
        self.assertEqual(set(psvr2_build.source_catalog()["firmwares"]), set(manifest["firmwares"]))
        self.assertEqual(psvr2_build.resolve_firmware_profile("1.10"), "01.10")
        for profile in manifest["sources"]["kernel"]["firmwares"].values():
            self.assertTrue(profile["url"].startswith("https://www.playstation.com/"))
            self.assertEqual(len(profile["archive_sha256"][0]), 64)
        for module in manifest["modules"].values():
            self.assertTrue((psvr2_build.TARGET_ROOT / module["path"] / "Makefile").is_file())
        for recipe in manifest["tools"].values():
            for source in recipe.get("sources", []):
                self.assertTrue((psvr2_build.TARGET_ROOT / source).is_file(), source)
        busybox = manifest["tools"]["busybox"]
        self.assertEqual(busybox["version"], "1.29.0")
        self.assertTrue(busybox["source_url"].startswith("https://www.playstation.com/"))
        self.assertEqual(len(busybox["source_sha256"]), 64)
        for patch in busybox["patches"]:
            self.assertTrue((psvr2_build.TARGET_ROOT / patch).is_file())
        self.assertNotIn("assets", manifest["tools"]["open_vrhmd"]["package"])

    def test_tool_receipt_identity_changes_with_authored_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "codec").mkdir()
            (root / "codec/main.c").write_text("int main(void){return 0;}\n")
            header = root / "codec/api.h"
            header.write_text("#define LIMIT 16\n")
            recipe = {"sources": ["codec/main.c"], "headers": ["codec/**/*.h"]}
            with mock.patch.object(psvr2_build, "TARGET_ROOT", root):
                original = psvr2_build.tool_source_hash(recipe)
                header.write_text("#define LIMIT 32\n")
                self.assertNotEqual(original, psvr2_build.tool_source_hash(recipe))
                header.unlink()
                with self.assertRaises(psvr2_build.BuildError):
                    psvr2_build.tool_source_hash(recipe)


class PublicSourceTests(unittest.TestCase):
    def test_nested_busybox_archive_uses_inner_gpl_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "busybox.tar.bz2"
            internal = io.BytesIO()
            with tarfile.open(fileobj=internal, mode="w:gz"):
                pass
            with tarfile.open(archive, "w:bz2") as bundle:
                files = {"busybox/src/Makefile": b"VERSION = 1\nPATCHLEVEL = 29\nSUBLEVEL = 0\n",
                         "busybox/src/Config.in": b"", "busybox/src/include/libbb.h": b"",
                         "busybox/src/applets/busybox.mkll": b"",
                         "busybox/src/testsuite/fixture.tar.gz": internal.getvalue()}
                for name, data in files.items():
                    member = tarfile.TarInfo(name)
                    member.size = len(data)
                    bundle.addfile(member, io.BytesIO(data))
            psvr2_build.unpack_nested_source_archive(root)
            self.assertFalse(archive.exists())
            self.assertTrue((root / "busybox/src/testsuite/fixture.tar.gz").is_file())
            self.assertEqual(psvr2_build.find_busybox_source(root, "1.29.0"), root / "busybox/src")

    def test_glibc_header_unwrap_ignores_unrelated_escaping_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inner = io.BytesIO()
            header = b"/* GNU C Library license */\nElf64_Ehdr;\n"
            with tarfile.open(fileobj=inner, mode="w:gz") as bundle:
                member = tarfile.TarInfo("glibc-2.28/elf/elf.h")
                member.size = len(header)
                bundle.addfile(member, io.BytesIO(header))
                link = tarfile.TarInfo("glibc-2.28/patches/sdk-link")
                link.type = tarfile.SYMTYPE
                link.linkname = "../../../outside-sdk-file"
                bundle.addfile(link)
            outer = io.BytesIO()
            with tarfile.open(fileobj=outer, mode="w:bz2") as bundle:
                data = inner.getvalue()
                member = tarfile.TarInfo("glibc/glibc-2.28.tgz")
                member.size = len(data)
                bundle.addfile(member, io.BytesIO(data))
            archive = root / "glibc.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("glibc.tar.bz2", outer.getvalue())
            self.assertEqual(psvr2_build.public_glibc_elf_header(archive), header.decode())
            self.assertEqual(list(root.iterdir()), [archive])

    def test_elf_host_adaptation_preserves_license_and_declarations(self):
        source = "/* GNU C Library license */\n#include <features.h>\n__BEGIN_DECLS\nElf64_Ehdr;\n__END_DECLS\n"
        result = psvr2_build.darwin_elf_header(source)
        self.assertIn("GNU C Library license", result)
        self.assertIn("Elf64_Ehdr", result)
        self.assertNotIn("features.h", result)
        self.assertNotIn("__BEGIN_DECLS", result)
        with self.assertRaises(psvr2_build.BuildError):
            psvr2_build.darwin_elf_header("foreign header")

    def test_source_fetch_rejects_non_sony_url_before_network(self):
        arguments = psvr2_build.build_parser().parse_args(["sources", "glibc"])
        data = {"sources": {"glibc": {"source_url": "https://example.com/source.zip", "source_sha256": "0" * 64}}}
        with (mock.patch.object(psvr2_build, "manifest", return_value=data),
              mock.patch.object(psvr2_build, "download_verified_archive") as download):
            with self.assertRaisesRegex(psvr2_build.BuildError, "Sony"):
                psvr2_build.cmd_sources(arguments)
            download.assert_not_called()


class InteractionTests(unittest.TestCase):
    def test_build_default_is_always_firmware_0600(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config.json"
            config.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "default_firmware": "01.10",
                        "firmwares": {},
                    }
                )
            )
            settings = psvr2_build.resolve_settings(
                argparse.Namespace(
                    config=str(config),
                    firmware=None,
                    verbose=False,
                ),
                require_kernel=False,
            )
        self.assertEqual(settings.firmware, "06.00")

    def test_matrix_requires_explicit_all_for_every_firmware(self) -> None:
        self.assertEqual(
            psvr2_build.selected_matrix_firmwares([]),
            ["06.00"],
        )
        self.assertEqual(
            psvr2_build.selected_matrix_firmwares(["1.10"]),
            ["01.10"],
        )
        self.assertEqual(
            psvr2_build.selected_matrix_firmwares(["all"]),
            psvr2_build.firmware_profiles(),
        )
        with self.assertRaisesRegex(
            psvr2_build.BuildError,
            "cannot be combined",
        ):
            psvr2_build.selected_matrix_firmwares(["all", "06.00"])

    def test_dragged_path_accepts_shell_escaped_spaces(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "arbitrary source bundle"
            archive.touch()
            self.assertEqual(
                psvr2_build.dragged_path(shlex.quote(str(archive))),
                archive.resolve(),
            )

    def test_confirmation_requires_yes_or_no(self) -> None:
        answers = iter(("maybe", "Y"))
        output = io.StringIO()
        with redirect_stdout(output):
            accepted = psvr2_build.confirm(
                "Install GNU make?",
                input_fn=lambda _prompt: next(answers),
            )
        self.assertTrue(accepted)
        self.assertIn("Please answer yes (y) or no (n).", output.getvalue())
        self.assertFalse(
            psvr2_build.confirm(
                "Install Zig?",
                input_fn=lambda _prompt: "n",
            )
        )

    def test_setup_command_requires_explicit_firmware_and_archive(self) -> None:
        parser = psvr2_build.build_parser()
        with (
            self.assertRaises(SystemExit),
            redirect_stderr(io.StringIO()),
        ):
            parser.parse_args(["setup", "--firmware", "06.00"])
        scripted = parser.parse_args(
            [
                "setup",
                "--firmware",
                "06.00",
                "/path/to/supplied-source-bundle",
                "--yes",
            ]
        )
        self.assertEqual(scripted.handler, psvr2_build.cmd_setup)
        self.assertEqual(
            scripted.archive,
            "/path/to/supplied-source-bundle",
        )
        self.assertTrue(scripted.yes)

    def test_busybox_source_selection_is_explicit(self) -> None:
        parser = psvr2_build.build_parser()
        downloaded = parser.parse_args(
            ["tool", "busybox", "--download", "--firmware", "06.00"]
        )
        self.assertTrue(downloaded.download)
        self.assertIsNone(downloaded.source)
        supplied = parser.parse_args(
            [
                "tool",
                "busybox",
                "--source",
                "/path/to/busybox.tar.bz2",
            ]
        )
        self.assertFalse(supplied.download)
        self.assertEqual(supplied.source, "/path/to/busybox.tar.bz2")
        with (
            self.assertRaises(SystemExit),
            redirect_stderr(io.StringIO()),
        ):
            parser.parse_args(
                [
                    "tool",
                    "busybox",
                    "--download",
                    "--source",
                    "/path/to/busybox.tar.bz2",
                ]
            )


class BusyBoxBuildTests(unittest.TestCase):
    def test_configuration_fragment_replaces_and_adds_symbols(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / ".config"
            config.write_text(
                "CONFIG_STATIC=n\n"
                "CONFIG_PAM=y\n"
                "# CONFIG_ASH is not set\n"
            )
            fragment = root / "fragment"
            fragment.write_text(
                "CONFIG_STATIC=y\n"
                "# CONFIG_PAM is not set\n"
                "CONFIG_ASH=y\n"
                "CONFIG_HUSH=y\n"
            )
            overrides = psvr2_build.apply_kconfig_fragment(config, fragment)
            resolved = psvr2_build.read_kconfig_values(config)
        self.assertEqual(resolved, overrides)
        self.assertEqual(resolved["CONFIG_STATIC"], "y")
        self.assertEqual(resolved["CONFIG_PAM"], "n")
        self.assertEqual(resolved["CONFIG_HUSH"], "y")

    def test_supplied_external_archive_is_hash_named_and_verified(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "arbitrary-name"
            with zipfile.ZipFile(source, "w") as bundle:
                bundle.writestr("busybox-source.txt", "source")
            digest = psvr2_build.sha256_file(source)
            recipe = {
                "source_sha256": digest,
                "source_url": "https://busybox.net/example",
            }
            with mock.patch.object(
                psvr2_build,
                "DEFAULT_EXTERNAL_ROOT",
                root / "cache",
            ):
                cached = psvr2_build.resolve_external_archive(
                    recipe,
                    supplied_source=str(source),
                    download=False,
                )
        self.assertEqual(cached.name, digest)

    def test_supplied_external_archive_rejects_wrong_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source.zip"
            with zipfile.ZipFile(source, "w") as bundle:
                bundle.writestr("payload", "source")
            with self.assertRaisesRegex(
                psvr2_build.BuildError,
                "SHA-256 mismatch",
            ):
                psvr2_build.resolve_external_archive(
                    {
                        "source_sha256": "0" * 64,
                        "source_url": "https://busybox.net/example",
                    },
                    supplied_source=str(source),
                    download=False,
                )

    def test_generated_applet_table_is_exported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            header = Path(temporary) / "applet_tables.h"
            header.write_text(
                "const char applet_names[] ALIGN1 = \"\"\n"
                "\"[\" \"\\0\"\n"
                "\"ash\" \"\\0\"\n"
                "\"hush\" \"\\0\"\n"
                ";\n"
            )
            self.assertEqual(
                psvr2_build.busybox_applets(header),
                ["[", "ash", "hush"],
            )


class SourceIdentityTests(unittest.TestCase):
    def test_known_archive_cannot_be_assigned_to_wrong_family(self) -> None:
        known_0600 = psvr2_build.source_catalog()["firmwares"]["06.00"]
        with self.assertRaisesRegex(
            psvr2_build.BuildError,
            "belongs to source family 06.00",
        ):
            psvr2_build.verify_source_identity(
                firmware="01.10",
                archive_sha256=known_0600["archive_sha256"][0],
                source_tree_git_sha1="f" * 40,
                source_tree_sha256="f" * 64,
                allow_unverified_source=True,
            )

    def test_unknown_source_requires_explicit_opt_in(self) -> None:
        arguments = {
            "firmware": "06.00",
            "archive_sha256": "a" * 64,
            "source_tree_git_sha1": "b" * 40,
            "source_tree_sha256": "c" * 64,
        }
        with self.assertRaisesRegex(
            psvr2_build.BuildError,
            "--allow-unverified-source",
        ):
            psvr2_build.verify_source_identity(
                **arguments,
                allow_unverified_source=False,
            )
        self.assertEqual(
            psvr2_build.verify_source_identity(
                **arguments,
                allow_unverified_source=True,
            ),
            "unverified",
        )

    def test_repacked_official_tree_is_identified_by_content(self) -> None:
        tree = psvr2_build.source_catalog()["firmwares"]["06.00"][
            "source_tree_git_sha1"
        ]
        tree_sha256 = psvr2_build.source_catalog()["firmwares"]["06.00"][
            "source_tree_sha256"
        ]
        self.assertEqual(
            psvr2_build.verify_source_identity(
                firmware="06.00",
                archive_sha256="a" * 64,
                source_tree_git_sha1=tree,
                source_tree_sha256=tree_sha256,
                allow_unverified_source=False,
            ),
            "official-tree",
        )


class ConfigurationTests(unittest.TestCase):
    def test_manual_source_override_clears_snapshot_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "linux"
            (source / "arch" / "arm64").mkdir(parents=True)
            (source / "Makefile").write_text("VERSION = 4\n")
            (source / "Kconfig").write_text('mainmenu "test"\n')
            defconfig = (
                source
                / "arch"
                / "arm64"
                / "configs"
                / "sie_release_mt3612_asic_a0_defconfig"
            )
            defconfig.parent.mkdir(parents=True)
            defconfig.write_text("CONFIG_ARM64=y\n")
            (source / "drivers" / "misc" / "mediatek" / "dprx").mkdir(
                parents=True
            )
            header = (
                source
                / "include"
                / "soc"
                / "mediatek"
                / "mtk_dprx_info.h"
            )
            header.parent.mkdir(parents=True)
            header.write_text("#pragma once\n")
            config = root / "config.json"
            config.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "firmwares": {
                            "06.00": {
                                "source_snapshot": "a" * 40,
                                "source_archive_sha256": "b" * 64,
                                "source_tree_git_sha1": "c" * 40,
                                "source_tree_sha256": "d" * 64,
                                "source_verification": "official-archive",
                            }
                        },
                    }
                )
            )
            args = argparse.Namespace(
                config=str(config),
                firmware="06.00",
                kernel_source=str(source),
                kernel_build=None,
                cross_prefix=None,
                tool_cc=None,
                glibc_version=None,
                make=None,
                jobs=None,
                sysroot=None,
                dynamic_linker=None,
                allow_unverified_source=True,
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", root / "sdk"),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", root / "output"),
            ):
                psvr2_build.cmd_configure(args)
                verify_result = psvr2_build.cmd_sdk_verify(
                    argparse.Namespace(
                        source=[],
                        require_official=False,
                        config=str(config),
                    )
                )
            saved = json.loads(config.read_text())["firmwares"]["06.00"]
        self.assertEqual(verify_result, 0)
        self.assertEqual(Path(saved["kernel_source"]), source.resolve())
        for key in (
            "source_snapshot",
            "source_archive_sha256",
            "source_tree_git_sha1",
        ):
            self.assertNotIn(key, saved)
        self.assertEqual(saved["source_verification"], "unverified")
        self.assertEqual(len(saved["source_tree_sha256"]), 64)


class KernelPatchTests(unittest.TestCase):
    @staticmethod
    def write_stock_u_serial(root: Path) -> Path:
        source = root / "drivers" / "usb" / "gadget" / "function" / "u_serial.c"
        source.parent.mkdir(parents=True)
        lines = ["\n"] * 657

        def place(line_number: int, text: str) -> None:
            replacement = text.splitlines(keepends=True)
            lines[line_number - 1 : line_number - 1 + len(replacement)] = replacement

        place(
            76,
            "/* RX and TX queues can buffer QUEUE_SIZE packets before they hit the\n"
            " * next layer of buffering.  For TX that's a circular buffer; for RX\n"
            " * consider it a NOP.  A third layer is provided by the TTY code.\n"
            " */\n"
            "#define QUEUE_SIZE\t\t16\n"
            "#define WRITE_BUF_SIZE\t\t8192\t\t/* TX only */\n"
            "\n"
            "/* circular buffer */\n"
            "struct gs_buf {\n",
        )
        place(
            373,
            "\t\t\tbreak;\n"
            "\n"
            "\t\treq = list_entry(pool->next, struct usb_request, list);\n"
            "\t\tlen = gs_send_packet(port, req->buf, in->maxpacket);\n"
            "\t\tif (len == 0) {\n"
            "\t\t\twake_up_interruptible(&port->drain_wait);\n"
            "\t\t\tbreak;\n",
        )
        place(
            647,
            "\t * be as speedy as we might otherwise be.\n"
            "\t */\n"
            "\tfor (i = 0; i < n; i++) {\n"
            "\t\treq = gs_alloc_req(ep, ep->maxpacket, GFP_ATOMIC);\n"
            "\t\tif (!req)\n"
            "\t\t\treturn list_empty(head) ? -ENOMEM : 0;\n"
            "\t\treq->complete = fn;\n",
        )
        source.write_text("".join(lines))
        return source

    def test_tracked_u_serial_patch_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.write_stock_u_serial(root)
            patch = psvr2_build.KERNEL_PATCH_ROOT / "u_serial-throughput.patch"
            with mock.patch.object(psvr2_build, "KERNEL_SOURCE_PATCHES", (patch,)):
                psvr2_build.apply_kernel_source_patches(root)
                psvr2_build.apply_kernel_source_patches(root)
            contents = source.read_text()
        self.assertIn("#define QUEUE_SIZE\t\t32", contents)
        self.assertIn("#define WRITE_BUF_SIZE\t\t65536", contents)
        self.assertIn("#define REQ_BUF_MULT\t\t32", contents)
        self.assertIn("gs_send_packet(port, req->buf, req->length)", contents)
        self.assertIn(
            "gs_alloc_req(ep, ep->maxpacket * REQ_BUF_MULT, GFP_ATOMIC)",
            contents,
        )

    def test_tracked_u_serial_patch_rejects_unknown_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.write_stock_u_serial(root)
            source.write_text(source.read_text().replace("QUEUE_SIZE\t\t16", "QUEUE_SIZE 17"))
            patch = psvr2_build.KERNEL_PATCH_ROOT / "u_serial-throughput.patch"
            with (
                mock.patch.object(psvr2_build, "KERNEL_SOURCE_PATCHES", (patch,)),
                self.assertRaisesRegex(
                    psvr2_build.BuildError,
                    "does not match either side",
                ),
            ):
                psvr2_build.apply_kernel_source_patches(root)


class BootstrapTests(unittest.TestCase):
    @staticmethod
    def make_executable(path: Path, contents: str = "#!/bin/sh\nexit 0\n") -> None:
        path.write_text(contents)
        path.chmod(0o755)

    def test_saved_manual_tools_do_not_require_homebrew(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            gmake = root / "gmake"
            self.make_executable(
                gmake,
                "#!/bin/sh\necho 'GNU Make 4.4.1'\n",
            )
            prefix = str(root / "aarch64-none-elf-")
            for suffix in ("gcc", "strip", "objcopy"):
                self.make_executable(Path(prefix + suffix))
            zig = root / "zig"
            self.make_executable(zig)
            config = root / "config.json"
            config.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "firmwares": {},
                        "make": str(gmake),
                        "cross_prefix": prefix,
                        "tool_cc": f"{zig} cc",
                    }
                )
            )
            args = argparse.Namespace(config=str(config), check=False, yes=False)
            output = io.StringIO()
            with (
                redirect_stdout(output),
                mock.patch.object(psvr2_build.platform, "system", return_value="Darwin"),
                mock.patch.object(psvr2_build.platform, "machine", return_value="x86_64"),
                mock.patch.object(
                    psvr2_build,
                    "command_line_tools_ready",
                    return_value=True,
                ),
                mock.patch.object(psvr2_build, "find_homebrew", return_value=None),
                mock.patch.object(
                    psvr2_build,
                    "discover_kernel_host_helpers",
                    return_value={
                        "bc": "/usr/bin/bc",
                        "perl": "/usr/bin/perl",
                    },
                ),
            ):
                result = psvr2_build.cmd_bootstrap(args)
            saved = json.loads(config.read_text())
        self.assertEqual(result, 0)
        self.assertEqual(saved["make"], str(gmake))
        self.assertEqual(saved["cross_prefix"], prefix)
        self.assertEqual(saved["tool_cc"], f"{zig} cc")
        self.assertIn(str(root), saved["host_paths"])
        self.assertNotIn("[missing] Homebrew", output.getvalue())

    def test_check_mode_does_not_install_or_modify_config(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config.json"
            original = '{"schema": 1, "firmwares": {}}\n'
            config.write_text(original)
            args = argparse.Namespace(config=str(config), check=True, yes=False)
            with (
                mock.patch.object(psvr2_build.platform, "system", return_value="Darwin"),
                mock.patch.object(
                    psvr2_build,
                    "command_line_tools_ready",
                    return_value=True,
                ),
                mock.patch.object(psvr2_build, "find_homebrew", return_value=None),
                mock.patch.object(psvr2_build, "discover_gnu_make", return_value=None),
                mock.patch.object(psvr2_build, "discover_cross_prefix", return_value=None),
                mock.patch.object(
                    psvr2_build,
                    "discover_target_compiler",
                    return_value=None,
                ),
                mock.patch.object(
                    psvr2_build,
                    "discover_kernel_host_helpers",
                    return_value={
                        "bc": "/usr/bin/bc",
                        "perl": "/usr/bin/perl",
                    },
                ),
                mock.patch.object(psvr2_build, "run") as run_mock,
            ):
                result = psvr2_build.cmd_bootstrap(args)
            current = config.read_text()
        self.assertEqual(result, 1)
        self.assertEqual(current, original)
        run_mock.assert_not_called()

    def test_each_missing_tool_gets_a_separate_confirmation(self) -> None:
        installed: set[str] = set()

        def fake_run(command: list[str], **_kwargs: object) -> None:
            if command[-1] == "make":
                installed.add("make")
            elif command[-1] == "gcc-aarch64-embedded":
                installed.add("cross")
            elif command[-1] == "zig":
                installed.add("target")

        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "config.json"
            args = argparse.Namespace(config=str(config), check=False, yes=False)
            with (
                mock.patch.object(psvr2_build.platform, "system", return_value="Darwin"),
                mock.patch.object(
                    psvr2_build,
                    "command_line_tools_ready",
                    return_value=True,
                ),
                mock.patch.object(
                    psvr2_build,
                    "find_homebrew",
                    return_value="/mock/bin/brew",
                ),
                mock.patch.object(
                    psvr2_build,
                    "discover_gnu_make",
                    side_effect=lambda *_args: (
                        "/mock/bin/gmake" if "make" in installed else None
                    ),
                ),
                mock.patch.object(
                    psvr2_build,
                    "discover_cross_prefix",
                    side_effect=lambda *_args: (
                        "/mock/bin/aarch64-none-elf-"
                        if "cross" in installed
                        else None
                    ),
                ),
                mock.patch.object(
                    psvr2_build,
                    "discover_target_compiler",
                    side_effect=lambda *_args: (
                        ("/mock/bin/zig", "cc") if "target" in installed else None
                    ),
                ),
                mock.patch.object(
                    psvr2_build,
                    "discover_kernel_host_helpers",
                    return_value={
                        "bc": "/usr/bin/bc",
                        "perl": "/usr/bin/perl",
                    },
                ),
                mock.patch.object(
                    psvr2_build,
                    "confirm",
                    side_effect=(True, False, True),
                ) as confirm_mock,
                mock.patch.object(psvr2_build, "run", side_effect=fake_run),
            ):
                result = psvr2_build.cmd_bootstrap(args)
        self.assertEqual(result, 1)
        self.assertEqual(
            [call.args[0] for call in confirm_mock.call_args_list],
            [
                "Install GNU make?",
                "Install Arm GNU AArch64 toolchain?",
                "Install AArch64 Linux compiler?",
            ],
        )
        self.assertEqual(installed, {"make", "target"})


class ImportTests(unittest.TestCase):
    @staticmethod
    def make_kernel_zip(path: Path, marker: str) -> None:
        with zipfile.ZipFile(path, "w") as bundle:
            bundle.writestr("linux/Makefile", "VERSION = 4\n")
            bundle.writestr("linux/Kconfig", 'mainmenu "test"\n')
            bundle.writestr("linux/arch/arm64/Kconfig", 'menu "arm64"\nendmenu\n')
            bundle.writestr(
                "linux/arch/arm64/configs/sie_release_mt3612_asic_a0_defconfig",
                "CONFIG_ARM64=y\n",
            )
            bundle.writestr(
                "linux/drivers/misc/mediatek/dprx/.keep",
                "",
            )
            bundle.writestr(
                "linux/include/soc/mediatek/mtk_dprx_info.h",
                "#pragma once\n",
            )
            bundle.writestr("linux/release-marker.txt", marker)

    def test_guided_setup_configures_a_dragged_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            kernel = root / "kernel"
            (kernel / "arch" / "arm64").mkdir(parents=True)
            (kernel / "Makefile").write_text("VERSION = 4\n")
            (kernel / "Kconfig").write_text("mainmenu \"test\"\n")
            archive = root / "arbitrary-source-bundle.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                for source in (
                    kernel / "Makefile",
                    kernel / "Kconfig",
                ):
                    bundle.write(source, f"linux/{source.name}")
                bundle.writestr("linux/arch/arm64/.keep", "")
                bundle.writestr(
                    "linux/arch/arm64/configs/"
                    "sie_release_mt3612_asic_a0_defconfig",
                    "CONFIG_ARM64=y\n",
                )
                bundle.writestr(
                    "linux/drivers/misc/mediatek/dprx/.keep",
                    "",
                )
                bundle.writestr(
                    "linux/include/soc/mediatek/mtk_dprx_info.h",
                    "#pragma once\n",
                )
            expected_archive_sha256 = psvr2_build.sha256_file(archive)
            config = root / "config.json"
            args = argparse.Namespace(
                archive=shlex.quote(str(archive)),
                firmware="01.10",
                yes=False,
                force=False,
                allow_unverified_source=True,
                no_prepare=True,
                verbose=False,
                defconfig="test_defconfig",
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", root / "sdk"),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", root / "output"),
                mock.patch.object(psvr2_build, "cmd_bootstrap", return_value=0),
            ):
                result = psvr2_build.cmd_setup(args)
            saved = json.loads(config.read_text())
        self.assertEqual(result, 0)
        self.assertEqual(saved["default_firmware"], "01.10")
        self.assertEqual(
            saved["firmwares"]["01.10"]["source_archive_sha256"],
            expected_archive_sha256,
        )
        self.assertTrue(saved["firmwares"]["01.10"]["source_snapshot"])

    def test_failed_force_import_preserves_previous_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            valid = root / "valid-source"
            self.make_kernel_zip(valid, "previous source")
            archive = root / "invalid.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("not-a-kernel.txt", "invalid")
            config = root / "config.json"
            valid_args = argparse.Namespace(
                archive=str(valid),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            invalid_args = argparse.Namespace(
                archive=str(archive),
                firmware="06.00",
                cross_prefix=None,
                force=True,
                allow_unverified_source=True,
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", root / "output"),
            ):
                psvr2_build.cmd_sdk_import(valid_args)
                before = json.loads(config.read_text())
                with self.assertRaises(psvr2_build.BuildError):
                    psvr2_build.cmd_sdk_import(invalid_args)
                after = json.loads(config.read_text())
                marker = sdk_root / "active" / "release-marker.txt"
                marker_contents = marker.read_text()
            self.assertEqual(
                before["firmwares"]["06.00"]["source_snapshot"],
                after["firmwares"]["06.00"]["source_snapshot"],
            )
            self.assertEqual(marker_contents, "previous source")

    def test_nested_kernel_archive_is_imported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_tree = root / "payload" / "linux"
            (source_tree / "arch" / "arm64").mkdir(parents=True)
            (source_tree / "Makefile").write_text("VERSION = 4\n")
            (source_tree / "Kconfig").write_text("mainmenu \"test\"\n")
            (source_tree / "arch" / "arm64" / "Kconfig").write_text(
                "menu \"arm64\"\nendmenu\n"
            )
            defconfig = (
                source_tree
                / "arch"
                / "arm64"
                / "configs"
                / "sie_release_mt3612_asic_a0_defconfig"
            )
            defconfig.parent.mkdir(parents=True)
            defconfig.write_text("CONFIG_ARM64=y\n")
            dprx = source_tree / "drivers" / "misc" / "mediatek" / "dprx"
            dprx.mkdir(parents=True)
            (dprx / ".keep").write_text("")
            header = (
                source_tree
                / "include"
                / "soc"
                / "mediatek"
                / "mtk_dprx_info.h"
            )
            header.parent.mkdir(parents=True)
            header.write_text("#pragma once\n")
            nested = root / "payload.tar"
            with tarfile.open(nested, "w") as bundle:
                bundle.add(source_tree.parent, arcname="linux")
            outer = root / "outer-bundle.zip"
            with zipfile.ZipFile(outer, "w") as bundle:
                bundle.write(nested, nested.name)
            sdk_root = root / "sdk"
            config = root / "config.json"
            args = argparse.Namespace(
                archive=str(outer),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", root / "output"),
            ):
                result = psvr2_build.cmd_sdk_import(args)
            saved = json.loads(config.read_text())
            imported = Path(saved["firmwares"]["06.00"]["kernel_source"])
            imported_is_valid = (imported / "arch" / "arm64").is_dir()
        self.assertEqual(result, 0)
        self.assertTrue(imported_is_valid)

    def test_build_receipt_carries_source_archive_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "stage1.ko"
            artifact.write_bytes(b"module")
            receipt = root / "stage1.json"
            settings = psvr2_build.Settings(
                firmware="06.00",
                kernel_source=None,
                kernel_build=root / "kernel",
                cross_prefix="/toolchain/aarch64-none-elf-",
                tool_cc=("/toolchain/zig", "cc"),
                glibc_version="2.28",
                make="/usr/bin/make",
                jobs=1,
                sysroot=None,
                dynamic_linker="/lib/ld-2.28.so",
                host_paths=(),
                source_archive_sha256="a" * 64,
                verbose=False,
                source_tree_git_sha1="c" * 40,
                source_tree_sha256="d" * 64,
                source_verification="official-archive",
            )
            psvr2_build.write_receipt(
                receipt,
                kind="module",
                name="stage1",
                settings=settings,
                source_hash="b" * 64,
                artifact=artifact,
            )
            data = json.loads(receipt.read_text())
            external_receipt = root / "busybox.json"
            psvr2_build.write_receipt(
                external_receipt,
                kind="external-target-tool",
                name="busybox",
                settings=settings,
                source_hash="e" * 64,
                artifact=artifact,
                include_kernel_source=False,
            )
            external_data = json.loads(external_receipt.read_text())
        self.assertEqual(data["kernel_source_archive_sha256"], "a" * 64)
        self.assertEqual(data["kernel_source_tree_git_sha1"], "c" * 40)
        self.assertEqual(data["kernel_source_tree_sha256"], "d" * 64)
        self.assertEqual(data["kernel_source_verification"], "official-archive")
        self.assertNotIn("kernel_source_archive_sha256", external_data)
        self.assertNotIn("kernel_source_snapshot", external_data)
        self.assertNotIn("kernel_source_tree_sha256", external_data)

    def test_adjacent_snapshot_diff_reports_changed_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            output = root / "output"
            config = root / "config.json"
            first = root / "first"
            second = root / "second"
            self.make_kernel_zip(first, "first")
            self.make_kernel_zip(second, "second")
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", output),
            ):
                for firmware, archive in (("01.10", first), ("06.00", second)):
                    args = argparse.Namespace(
                        archive=str(archive),
                        firmware=firmware,
                        cross_prefix=None,
                        force=False,
                        allow_unverified_source=True,
                        config=str(config),
                    )
                    psvr2_build.cmd_sdk_import(args)
                data = json.loads(config.read_text())
                report = psvr2_build.generate_firmware_diff(
                    data, "01.10", "06.00"
                )
            self.assertEqual(report["changed_files"], 1)
            self.assertEqual(report["changes"][0]["path"], "release-marker.txt")

    def test_incremental_import_set_uses_already_configured_family(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            output = root / "output"
            config = root / "config.json"
            archives: dict[str, Path] = {}
            for firmware in psvr2_build.firmware_profiles():
                archive = root / f"{firmware}.zip"
                self.make_kernel_zip(archive, firmware)
                archives[firmware] = archive
            first_args = argparse.Namespace(
                archive=str(archives["06.00"]),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", output),
            ):
                psvr2_build.cmd_sdk_import(first_args)
                remaining = [
                    [firmware, str(archive)]
                    for firmware, archive in archives.items()
                    if firmware != "06.00"
                ]
                set_args = argparse.Namespace(
                    source=remaining,
                    force=False,
                    allow_unverified_source=True,
                    cross_prefix=None,
                    config=str(config),
                )
                psvr2_build.import_source_set(set_args, require_all=True)
                saved = json.loads(config.read_text())
                refs = {
                    firmware: psvr2_build.read_snapshot_ref(firmware)
                    for firmware in psvr2_build.firmware_profiles()
                }
        self.assertEqual(
            set(saved["firmwares"]),
            set(psvr2_build.firmware_profiles()),
        )
        self.assertTrue(all(refs.values()))

    def test_set_preflight_failure_leaves_config_and_refs_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            output = root / "output"
            config = root / "config.json"
            existing = root / "06.00.zip"
            valid_new = root / "01.10.zip"
            invalid_new = root / "01.10.zip"
            self.make_kernel_zip(existing, "existing")
            self.make_kernel_zip(valid_new, "valid new")
            with zipfile.ZipFile(invalid_new, "w") as bundle:
                bundle.writestr("not-a-kernel.txt", "invalid")
            first_args = argparse.Namespace(
                archive=str(existing),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", output),
            ):
                psvr2_build.cmd_sdk_import(first_args)
                before = config.read_text()
                before_ref = psvr2_build.read_snapshot_ref("06.00")
                set_args = argparse.Namespace(
                    source=[
                        ["01.10", str(valid_new)],
                        ["01.10", str(invalid_new)],
                    ],
                    force=False,
                    allow_unverified_source=True,
                    cross_prefix=None,
                    config=str(config),
                )
                with self.assertRaises(psvr2_build.BuildError):
                    psvr2_build.import_source_set(set_args, require_all=False)
                after = config.read_text()
                new_ref = psvr2_build.read_snapshot_ref("01.10")
                existing_ref = psvr2_build.read_snapshot_ref("06.00")
        self.assertEqual(before, after)
        self.assertIsNone(new_ref)
        self.assertEqual(existing_ref, before_ref)

    def test_activation_failure_rolls_back_refs_config_and_active_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            output = root / "output"
            config = root / "config.json"
            existing = root / "06.00.zip"
            replacement = root / "01.10.zip"
            self.make_kernel_zip(existing, "existing")
            self.make_kernel_zip(replacement, "replacement")
            first_args = argparse.Namespace(
                archive=str(existing),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", output),
            ):
                psvr2_build.cmd_sdk_import(first_args)
                before = config.read_text()
                before_ref = psvr2_build.read_snapshot_ref("06.00")
                candidate = psvr2_build.preflight_source_archive(
                    firmware="01.10",
                    archive=replacement,
                    allow_unverified_source=True,
                )
                real_activate = psvr2_build.activate_source_snapshot

                def fail_new_activation(firmware: str, snapshot: str) -> Path:
                    if firmware == "01.10":
                        raise psvr2_build.BuildError("injected activation failure")
                    return real_activate(firmware, snapshot)

                apply_args = argparse.Namespace(
                    force=False,
                    cross_prefix=None,
                    config=str(config),
                )
                with (
                    mock.patch.object(
                        psvr2_build,
                        "activate_source_snapshot",
                        side_effect=fail_new_activation,
                    ),
                    self.assertRaisesRegex(
                        psvr2_build.BuildError,
                        "injected activation failure",
                    ),
                ):
                    psvr2_build.apply_source_candidates(
                        apply_args,
                        [candidate],
                        compact=False,
                    )
                after = config.read_text()
                added_ref = psvr2_build.read_snapshot_ref("01.10")
                active = json.loads(
                    (sdk_root / "active" / ".psvr2-source-snapshot.json").read_text()
                )
        self.assertEqual(before, after)
        self.assertIsNone(added_ref)
        self.assertEqual(active["firmware"], "06.00")
        self.assertEqual(active["snapshot"], before_ref)

    def test_guided_setup_does_not_prompt_for_identical_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            output = root / "output"
            config = root / "config.json"
            archive = root / "source.zip"
            self.make_kernel_zip(archive, "same")
            import_args = argparse.Namespace(
                archive=str(archive),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            setup_args = argparse.Namespace(
                archive=str(archive),
                firmware="06.00",
                yes=False,
                force=False,
                allow_unverified_source=True,
                no_prepare=True,
                verbose=False,
                defconfig="test_defconfig",
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", output),
                mock.patch.object(psvr2_build, "cmd_bootstrap", return_value=0),
                mock.patch.object(psvr2_build, "confirm") as confirm_mock,
            ):
                psvr2_build.cmd_sdk_import(import_args)
                result = psvr2_build.cmd_setup(setup_args)
            confirm_mock.assert_not_called()
        self.assertEqual(result, 0)

    def test_ambiguous_kernel_trees_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("first", "second"):
                candidate = root / name
                (candidate / "arch" / "arm64").mkdir(parents=True)
                (candidate / "Makefile").write_text("VERSION = 4\n")
                (candidate / "Kconfig").write_text('mainmenu "test"\n')
            with self.assertRaisesRegex(
                psvr2_build.BuildError,
                "multiple candidate kernel trees",
            ):
                psvr2_build.find_kernel_source(root)

    def test_zip_symlinks_and_expansion_over_limit_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            symlink_archive = root / "symlink.zip"
            with zipfile.ZipFile(symlink_archive, "w") as bundle:
                entry = zipfile.ZipInfo("link")
                entry.create_system = 3
                entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                bundle.writestr(entry, "../../outside")
            with self.assertRaisesRegex(
                psvr2_build.BuildError,
                "ZIP symlinks",
            ):
                psvr2_build.extract_archive(symlink_archive, root / "symlink-out")

            oversized = root / "oversized.zip"
            with zipfile.ZipFile(oversized, "w") as bundle:
                bundle.writestr("large", "12345")
            with (
                mock.patch.object(
                    psvr2_build,
                    "MAX_ARCHIVE_EXPANDED_BYTES",
                    4,
                ),
                self.assertRaisesRegex(
                    psvr2_build.BuildError,
                    "safety limit",
                ),
            ):
                psvr2_build.extract_archive(oversized, root / "oversized-out")
            with (
                mock.patch.object(psvr2_build, "MAX_ARCHIVE_MEMBERS", 0),
                self.assertRaisesRegex(
                    psvr2_build.BuildError,
                    "too many entries",
                ),
            ):
                psvr2_build.extract_archive(oversized, root / "members-out")

    def test_sdk_verify_detects_missing_firmware_ref(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            output = root / "output"
            config = root / "config.json"
            archive = root / "source.zip"
            self.make_kernel_zip(archive, "verify")
            import_args = argparse.Namespace(
                archive=str(archive),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            verify_args = argparse.Namespace(
                source=[],
                require_official=False,
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", output),
            ):
                psvr2_build.cmd_sdk_import(import_args)
                self.assertEqual(psvr2_build.cmd_sdk_verify(verify_args), 0)
                snapshot = json.loads(config.read_text())["firmwares"]["06.00"][
                    "source_snapshot"
                ]
                psvr2_build.update_snapshot_refs(
                    {"06.00": None},
                    expected={"06.00": snapshot},
                )
                failed = psvr2_build.cmd_sdk_verify(verify_args)
        self.assertEqual(failed, 1)

    def test_sdk_verify_detects_corrupted_git_objects(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk_root = root / "sdk"
            output = root / "output"
            config = root / "config.json"
            archive = root / "source.zip"
            self.make_kernel_zip(archive, "corruption")
            import_args = argparse.Namespace(
                archive=str(archive),
                firmware="06.00",
                cross_prefix=None,
                force=False,
                allow_unverified_source=True,
                config=str(config),
            )
            verify_args = argparse.Namespace(
                source=[],
                require_official=False,
                config=str(config),
            )
            with (
                mock.patch.object(psvr2_build, "DEFAULT_SDK_ROOT", sdk_root),
                mock.patch.object(psvr2_build, "DEFAULT_OUTPUT", output),
            ):
                psvr2_build.cmd_sdk_import(import_args)
                pack = next(
                    (sdk_root / "source-store.git" / "objects" / "pack").glob(
                        "*.pack"
                    )
                )
                contents = bytearray(pack.read_bytes())
                contents[len(contents) // 2] ^= 0xFF
                pack.chmod(pack.stat().st_mode | stat.S_IWUSR)
                pack.write_bytes(contents)
                failed = psvr2_build.cmd_sdk_verify(verify_args)
        self.assertEqual(failed, 1)


class ToolCompileTests(unittest.TestCase):
    def test_userspace_compiler_receives_selected_firmware_family(self) -> None:
        for firmware, macro in (("01.10", "0110"), ("06.00", "0600")):
            with self.subTest(firmware=firmware):
                settings = argparse.Namespace(
                    tool_cc=("/toolchain/zig", "cc"), firmware=firmware,
                    glibc_version="2.28", sysroot=None,
                    dynamic_linker="/lib/ld-2.28.so",
                )
                recipe = psvr2_build.manifest()["tools"]["open_vrhmd"]
                with mock.patch.object(psvr2_build, "executable", return_value="/toolchain/zig"):
                    command = psvr2_build.tool_compile_command(
                        "open_vrhmd", recipe, Path("/tmp/open_vrhmd"), settings
                    )
                self.assertIn(f"-DPSVR2_SOURCE_FAMILY_{macro}=1", command)
                other = "0110" if macro == "0600" else "0600"
                self.assertNotIn(f"-DPSVR2_SOURCE_FAMILY_{other}=1", command)


class CompatibilityTests(unittest.TestCase):
    def test_darwin_host_flags_do_not_override_kbuild_appends(self) -> None:
        settings = psvr2_build.Settings(
            firmware="06.00",
            kernel_source=Path("/kernel/source"),
            kernel_build=Path("/kernel/build"),
            cross_prefix="/toolchain/aarch64-none-elf-",
            tool_cc=("/toolchain/zig", "cc"),
            glibc_version="2.28",
            make="/usr/bin/make",
            jobs=1,
            sysroot=None,
            dynamic_linker="/lib/ld-2.28.so",
            host_paths=(),
            source_archive_sha256=None,
            verbose=False,
        )
        with (mock.patch.object(psvr2_build.platform, "system", return_value="Darwin"),
              mock.patch.object(Path, "is_file", return_value=True)):
            command = psvr2_build.kernel_make_base(settings)
            environment = psvr2_build.kernel_host_environment(settings)
        self.assertFalse(
            any(item.startswith("HOST_EXTRACFLAGS=") for item in command)
        )
        self.assertIn("inputs/host-include", environment["HOST_EXTRACFLAGS"])

    def test_darwin_modpost_patch_is_idempotent(self) -> None:
        old_call = (
            'char *__cat(pstart_,name) = getsectdata("__TEXT",\t\\\n'
            '\t\t\t#name, &__cat(name,_len));\t\t\t\\'
        )
        fixture = (
            "#include <mach-o/getsect.h>\n"
            "#define ADD(name) \\\n"
            f"{old_call}\n"
            'static int marker __attribute__((section("__TEXT, " #name)));\n'
        )
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            file2alias = source / "scripts" / "mod" / "file2alias.c"
            file2alias.parent.mkdir(parents=True)
            file2alias.write_text(fixture)
            with mock.patch.object(psvr2_build.platform, "system", return_value="Darwin"):
                psvr2_build.apply_darwin_kernel_compatibility(source)
                once = file2alias.read_text()
                psvr2_build.apply_darwin_kernel_compatibility(source)
                twice = file2alias.read_text()
        self.assertEqual(once, twice)
        self.assertIn("#include <mach-o/ldsyms.h>", once)
        self.assertIn("&_mh_execute_header", once)
        self.assertIn("getsectiondata(", once)
        self.assertIn('section("__DATA, " #name)', once)
        self.assertNotIn('getsectdata("__TEXT"', once)

    def test_elf_interpreter_patch(self) -> None:
        old_interpreter = b"/lib/ld-linux-aarch64.so.1\0"
        image = bytearray(256)
        image[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<Q", image, 32, 64)
        struct.pack_into("<H", image, 54, 56)
        struct.pack_into("<H", image, 56, 1)
        struct.pack_into("<I", image, 64, 3)
        struct.pack_into("<Q", image, 72, 160)
        struct.pack_into("<Q", image, 96, len(old_interpreter))
        image[160 : 160 + len(old_interpreter)] = old_interpreter

        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "tool"
            executable.write_bytes(image)
            psvr2_build.patch_elf_interpreter(executable, "/lib/ld-2.28.so")
            patched = executable.read_bytes()
        self.assertEqual(
            patched[160 : 160 + len(old_interpreter)].rstrip(b"\0"),
            b"/lib/ld-2.28.so",
        )

    def test_archive_traversal_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(psvr2_build.BuildError):
                psvr2_build.safe_member_path(root, "../escape")


if __name__ == "__main__":
    unittest.main()
