"""Native supervision tests using a fake BusyBox and local Stage1 transport.

The fake implements ash with /bin/sh, setsid with POSIX setsid(), and the target's
kill grammar. It checks mailbox lengths and process/FIFO behavior without USB.
"""
from pathlib import Path
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PREBUILT = (Path(sys.argv.pop(1)).resolve()
            if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None)


class BusyBoxJobTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="pj-", dir="/tmp")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.binary = PREBUILT
        if cls.binary is None:
            compiler = shutil.which("cc")
            if not compiler:
                raise unittest.SkipTest("host C compiler unavailable")
            (cls.directory / "libusb.h").write_text(
                "typedef struct libusb_context libusb_context;\n"
                "typedef struct libusb_device_handle libusb_device_handle;\n"
                "struct libusb_config_descriptor;\n")
            cls.binary = cls.directory / "jobs"
            subprocess.run([
                compiler, "-std=gnu11", "-DPSVR2_SHELL_JOB_WAIT_SECONDS=0.2",
                "-I" + str(cls.directory), "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / "src"), "-Wall", "-Wextra", "-Werror",
                str(ROOT / "tests/busybox_jobs_harness.c"),
                str(ROOT / "src/shell_busybox.c"), str(ROOT / "src/util.c"),
                "-o", str(cls.binary)], check=True, capture_output=True, text=True)
        # busybox_path is 64 bytes; keep the executable and quoted cwd short.
        if len(os.fsencode(cls.binary)) >= 64:
            short_binary = cls.directory / "jobs"
            os.symlink(cls.binary, short_binary)
            cls.binary = short_binary
        cls.cwd = cls.directory / "user's files"
        cls.cwd.mkdir()

    def run_case(self, case):
        process = subprocess.Popen([str(self.binary), case, str(self.cwd)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True)
        stdout = stderr = ""
        try:
            stdout, stderr = process.communicate(timeout=8)
            self.assertEqual(process.returncode, 0, stdout + stderr)
        finally:
            if process.poll() is None:
                process.kill()
                remaining_stdout, remaining_stderr = process.communicate()
                stdout += remaining_stdout
                stderr += remaining_stderr
            # Failed assertions must not leave detached test jobs behind.
            directories = set(Path("/tmp").glob(f".psvr2-job-{process.pid}-*"))
            # A reentry case deliberately exits its first host process. Its
            # surviving remote-job receipt must also be cleaned after failure.
            directories.update(Path(path) for path in re.findall(
                r"/tmp/\.psvr2-job-\d+-\d+-\d+", stdout + "\n" + stderr))
            for directory in directories:
                if not directory.exists():
                    continue
                for filename in ("pid", "pid.saved", "launch"):
                    path = directory / filename
                    if not path.exists():
                        continue
                    text = path.read_text().strip()
                    if not text.isascii() or not text.isdecimal():
                        continue
                    pid = int(text)
                    if pid <= 1 or pid > 2147483647:
                        continue
                    try:
                        os.killpg(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                shutil.rmtree(directory)

    def test_nonzero_exit(self):
        self.run_case("exit")

    def test_quoted_cwd_and_status(self):
        self.run_case("cwd")

    def test_commands_larger_than_mailbox_with_many_quotes(self):
        self.run_case("chunks")

    def test_long_job_releases_mailbox_for_second_command(self):
        self.run_case("concurrent")

    def test_detached_job_survives_host_exit_without_blocking_new_session(self):
        self.run_case("reentry")

    def test_fifo_keeps_reader_alive_and_accepts_input(self):
        self.run_case("fifo")

    def test_stop_terminates_process_group(self):
        self.run_case("stop")

    def test_stop_during_session_startup(self):
        self.run_case("startup")

    def test_supervisor_waits_for_signal_handler_cleanup(self):
        self.run_case("cleanup")

    def test_cancel_after_pid_publish_uses_target_busybox_group_grammar(self):
        self.run_case("early-cancel")

    def test_ambiguous_launch_failure_retains_cancellable_job(self):
        self.run_case("failure")

    def test_lost_status_response_retains_live_cancellable_job(self):
        self.run_case("read-failure")

    def test_missing_status_never_fabricates_exit_or_deletes_job(self):
        self.run_case("missing-status")

    def test_invalid_group_and_launch_pids_are_rejected(self):
        self.run_case("invalid-pid")

    def test_malformed_status_protocol_is_rejected(self):
        self.run_case("malformed")


if __name__ == "__main__":
    unittest.main()
