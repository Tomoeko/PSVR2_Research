"""Stock takeover regression using private proc fixtures and intercepted signals."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[1]


class StockTakeoverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("host C compiler unavailable")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.proc = cls.root / "proc"
        cls.ram = cls.root / "vrhmd_main.elf"
        cls.rom = cls.root / "rom-vrhmd_main.elf"
        cls.other = cls.root / "unrelated.elf"
        for path in (cls.ram, cls.rom, cls.other):
            path.write_bytes(b"fixture, never executed")
        cls.binary = cls.root / "stock_test"
        subprocess.run([
            compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            '-DOPEN_VRHMD_PROC_ROOT="' + str(cls.proc) + '"',
            '-DOPEN_VRHMD_STOCK_RAM_PATH="' + str(cls.ram) + '"',
            '-DOPEN_VRHMD_STOCK_ROOT_PATH="' + str(cls.rom) + '"',
            "-DOPEN_VRHMD_STOCK_WAIT_MS=50",
            str(REPO_ROOT / "tests/fixtures/open_vrhmd_stock_harness.c"),
            "-o", str(cls.binary)
        ], check=True, capture_output=True, text=True)

    def setUp(self):
        shutil.rmtree(self.proc, ignore_errors=True)
        self.proc.mkdir()

    def process(self, executable, comm="VrhmdMain"):
        directory = self.proc / "96"
        directory.mkdir()
        if executable is not None:
            (directory / "exe").symlink_to(executable)
        (directory / "stat").write_text(
            f"96 ({comm}) S" + " 0" * 18 + " 12345\n")

    def worker(self, comm="WarpaFeTriggerT", flags=0x00200000, state="S"):
        directory = self.proc / "97"
        directory.mkdir()
        (directory / "comm").write_text(comm + "\n")
        (directory / "stat").write_text(
            "97 (" + comm + ") " + state + "".join(
                " " + str(flags if field == 9 else 0) for field in range(4, 23)) + "\n")

    def run_case(self, **options):
        env = dict(os.environ)
        for name in ("TEST_SIGNAL_FAIL", "TEST_STOCK_HOLD", "TEST_PID_REUSED", "TEST_CALL_OPEN", "TEST_STOP_FAIL"):
            env.pop(name, None)
        env.update({name: str(value) for name, value in options.items()})
        return subprocess.run([str(self.binary)], env=env, capture_output=True,
                              text=True, check=True, timeout=3)

    def test_renamed_stock_comm_is_identified_by_executable(self):
        self.process(self.ram)
        result = self.run_case()
        self.assertIn("takeover=1 signals=1", result.stdout)
        self.assertIn("stock VrhmdMain executable at PID 96", result.stderr)

    def test_inactive_warpa_skips_non_idempotent_streamoff(self):
        self.process(self.ram)
        result = self.run_case(TEST_CALL_OPEN=1)
        self.assertIn("open=0 signals=1 teardown=1", result.stdout)
        self.assertIn("skipping non-idempotent STREAMOFF", result.stderr)

    def test_active_warpa_worker_is_left_unchanged_during_display_takeover(self):
        self.process(self.ram)
        self.worker()
        result = self.run_case(TEST_CALL_OPEN=1)
        self.assertIn("open=0 signals=1 teardown=1", result.stdout)
        self.assertIn("Leaving Sony WARPA tracking worker PID 97 unchanged", result.stderr)

    def test_orphaned_active_warpa_does_not_block_owned_display_devices(self):
        self.worker()
        result = self.run_case(TEST_CALL_OPEN=1)
        self.assertIn("open=0 signals=0 teardown=0", result.stdout)
        self.assertIn("Leaving Sony WARPA tracking worker PID 97 unchanged", result.stderr)

    def test_warpa_comm_alone_does_not_identify_a_kernel_worker(self):
        self.process(self.ram)
        self.worker(flags=0)
        self.assertIn("takeover=1", self.run_case().stdout)

    def test_exited_warpa_task_is_not_an_active_worker(self):
        self.process(self.ram)
        self.worker(state="Z")
        self.assertIn("takeover=1", self.run_case().stdout)

    def test_unreadable_warpa_state_is_a_nonfatal_diagnostic(self):
        self.process(self.ram)
        self.worker(flags="invalid")
        result = self.run_case()
        self.assertIn("takeover=1 signals=1", result.stdout)
        self.assertIn("WARPA tracking state unknown", result.stderr)

    def test_original_comm_and_root_executable_are_supported(self):
        self.process(self.rom, "vrhmd_main.elf")
        self.assertIn("takeover=1 signals=1", self.run_case().stdout)

    def test_task_name_alone_does_not_authorize_a_signal(self):
        self.process(self.other)
        self.assertIn("takeover=0 signals=0", self.run_case().stdout)

    def test_process_without_executable_is_already_released(self):
        self.process(None)
        self.assertIn("takeover=0 signals=0", self.run_case().stdout)

    def test_signal_failure_prevents_device_initialization(self):
        self.process(self.ram)
        self.assertIn("takeover=-1 signals=1", self.run_case(TEST_SIGNAL_FAIL=1).stdout)

    def test_teardown_failure_after_takeover_closes_devices_and_blocks_init(self):
        self.process(self.ram)
        result = self.run_case(TEST_CALL_OPEN=1, TEST_STOP_FAIL=1)
        self.assertIn("open=-1 signals=1 teardown=1 closed=1", result.stdout)

    def test_unreleased_stock_process_has_bounded_wait(self):
        self.process(self.ram)
        result = self.run_case(TEST_STOCK_HOLD=1)
        self.assertIn("takeover=-1 signals=1", result.stdout)
        self.assertIn("within 50ms", result.stderr)

    def test_pid_reuse_before_signal_is_rejected(self):
        self.process(self.ram)
        self.assertIn("takeover=-1 signals=0", self.run_case(TEST_PID_REUSED=1).stdout)

    def test_proc_scan_failure_is_not_treated_as_stock_absence(self):
        self.proc.rmdir()
        self.assertIn("takeover=-1 signals=0", self.run_case().stdout)


if __name__ == "__main__":
    unittest.main()
