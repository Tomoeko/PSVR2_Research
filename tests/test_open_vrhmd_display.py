"""Check framebuffer cleanup and hardware command payload preservation on the host."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# SHA-256 of the complete 136-byte table entries, including zero padding.
# Captured from the pre-refactor command sequence and compared byte for byte
# before replacing the imperative table construction with const tables.
PANEL_BATCHES = [
    (0, 5, "9086f2f118b52290b574e171daf4a6dc95a5bc409eb9521be660f7360b9b4abc"),
    (0, 4, "1c35294eaffc06434bdbbf9a8f3027d5dd1a03c2f22834eaa652b41dcb35a357"),
    (0, 2, "69f559634acc2224951deb33ede4276433776d72dc21bd12635e1adfc134f912"),
    (0, 1, "6820444302e10bbcb74c9e656824474b4156b084b7a675922d237f46aa2cd19e"),
    (0, 1, "85127f5cb60656dc63230fb021b8a2131c1566d79dd03a2a1e313de2707784c0"),
    (0, 3, "334272ad0d6c226fdab56b22d07b560eb6e786c1c584012fd1fd6137c37b40a4"),
    (0, 1, "b707241545a346265aab1ffb32ff64b55bf8f8dc1b56a46ef33ce3d15db11d33"),
    (1, 5, "7139edfb640de36326751b566f565bc96e79747b50a85aeffd6cde8c873f2478"),
    (1, 5, "6585a774e53e78c3420e8b632116511905f9194929e78df424980f506068213f"),
    (1, 3, "c66eafc2f5dea496accaf282cd1b8df529a0e605713caaf95d8cd3845267dd0d"),
    (1, 4, "f15acf63f78a467e9fd891373fd791abb32ec40f01e695ee3052f525bbc0d5dc"),
    (1, 4, "e22d7bc5d44bba86a1ff23d9088c843131d196cbb8fd7e889e8abc33619cd59f"),
    (1, 1, "b707241545a346265aab1ffb32ff64b55bf8f8dc1b56a46ef33ce3d15db11d33"),
]


class OpenVrhmdDisplayTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "a host C compiler is required")
    def test_framebuffer_abi_and_preserved_panel_batches(self):
        with tempfile.TemporaryDirectory(prefix="open-vrhmd-display-") as directory:
            executable = Path(directory) / "display-harness"
            compile_result = subprocess.run(
                [shutil.which("cc"), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-DPSVR2_SOURCE_FAMILY_0600",
                 str(ROOT / "tests/fixtures/open_vrhmd_display_harness.c"),
                 "-o", str(executable)], capture_output=True, text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([str(executable)], input="", capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            batches = []
            for line in result.stdout.splitlines():
                mode, count, payload = line.split()
                batches.append((int(mode), int(count),
                                hashlib.sha256(bytes.fromhex(payload)).hexdigest()))
            self.assertEqual(batches, PANEL_BATCHES)


if __name__ == "__main__":
    unittest.main()
