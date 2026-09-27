"""Exercise the source release gate without device data."""

import importlib.util
from pathlib import Path
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "tools" / "check_release.py"
SPEC = importlib.util.spec_from_file_location("check_release", SOURCE)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class ReleaseFileTests(unittest.TestCase):
    def test_regular_source_is_accepted(self):
        self.assertEqual(CHECK.file_errors("psvr2_krw_c/src/example.c",
                                         b"int example(void) { return 0; }\n"), [])
        self.assertEqual(CHECK.file_errors("target/psvr2/modules/README.md", b"Module guide\n"), [])

    def test_binary_file_and_content_are_rejected(self):
        self.assertTrue(CHECK.file_errors("tools/program.bin", b"\x7fELF\x00"))
        self.assertIn("binary content", CHECK.file_errors("tools/program.c", b"\x00"))

    def test_private_home_path_is_rejected(self):
        path = "/" + "Users" + "/" + "example" + "/" + "input"
        self.assertIn("host home path", CHECK.file_errors("docs/example.md", path.encode()))

    def test_unexpected_root_module_and_git_modes_are_rejected(self):
        self.assertTrue(CHECK.file_errors("captures/example.txt", b"capture"))
        self.assertTrue(CHECK.file_errors("target/psvr2/modules/unused/example.c", b"code"))
        self.assertTrue(CHECK.file_errors("tools/example.py", b"target", "120000"))

    def test_public_hashes_are_not_treated_as_credentials(self):
        self.assertEqual(CHECK.file_errors("docs/example.md", ("a" * 64).encode()), [])


if __name__ == "__main__":
    unittest.main()
