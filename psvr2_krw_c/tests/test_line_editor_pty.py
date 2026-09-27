"""Exercise the actual interactive editor through a narrow pseudo-terminal.

The driver reports accepted command bytes on stderr, keeping terminal rendering
separate. The small terminal model implements the editor's cursor/erase controls
and catches automatic wrapping that would duplicate prompts on older versions.
Pass the CMake-built driver path as the first argument, or run this file directly
to compile a temporary standalone driver with the host C compiler.
"""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unicodedata
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None


class Screen:
    def __init__(self, columns):
        self.columns = columns
        self.rows = [[" "] * columns]
        self.row = self.column = self.wraps = 0
        self.pending_wrap = False

    def _row(self):
        while len(self.rows) <= self.row:
            self.rows.append([" "] * self.columns)
        return self.rows[self.row]

    def feed(self, payload):
        text = payload.decode("utf-8", "replace")
        offset = 0
        while offset < len(text):
            if text[offset:offset + 2] == "\x1b[":
                match = re.match(r"\x1b\[([0-9;?]*)([A-Za-z~])", text[offset:])
                if not match:
                    raise AssertionError("Unexpected terminal escape: " + repr(text[offset:]))
                parameters, command = match.groups()
                count = int(parameters or "1") if "?" not in parameters else 0
                if command == "K":
                    self._row()[self.column:] = [" "] * (self.columns - self.column)
                elif command == "C":
                    self.column = min(self.column + count, self.columns - 1)
                elif command == "D":
                    self.column = max(0, self.column - count)
                elif command == "J" and count == 2:
                    self.rows = [[" "] * self.columns]
                    self.row = 0
                elif command == "H":
                    self.row = self.column = 0
                elif command not in ("h", "l"):
                    raise AssertionError("Unexpected cursor control: " + match.group())
                self.pending_wrap = False
                offset += len(match.group())
                continue
            character = text[offset]
            offset += 1
            if character == "\a":
                continue
            if character == "\r":
                self.column = 0
                self.pending_wrap = False
                continue
            if character == "\n":
                self.row += 1
                self.pending_wrap = False
                self._row()
                continue
            if unicodedata.combining(character):
                continue
            width = 2 if unicodedata.east_asian_width(character) in ("W", "F") else 1
            if self.pending_wrap or self.column + width > self.columns:
                self.row += 1
                self.column = 0
                self.wraps += 1
            row = self._row()
            row[self.column] = character
            if width == 2:
                row[self.column + 1] = ""
            self.column += width
            self.pending_wrap = self.column == self.columns
            if self.pending_wrap:
                self.column -= 1


class Terminal:
    def __init__(self, reads=1, columns=24, prompt="PSVR2> ", completion=False):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, columns, 0, 0))
        self.original = termios.tcgetattr(self.slave)
        environment = dict(os.environ)
        environment["LC_ALL"] = "en_US.UTF-8" if sys.platform == "darwin" else "C.UTF-8"
        arguments = [str(BINARY), str(reads), prompt]
        if completion:
            arguments.append("complete")
        self.process = subprocess.Popen(
            arguments, stdin=self.slave, stdout=self.slave,
            stderr=subprocess.PIPE, env=environment,
        )
        self.output = bytearray()
        self.columns = columns
        self.collect_until(b"\x1b[?2004h")
        self.collect(0.025)

    def collect(self, seconds=0.05):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [],
                                         max(0, deadline - time.monotonic()))
            if not ready:
                break
            try:
                chunk = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            self.output.extend(chunk)

    def collect_until(self, token, timeout=2):
        deadline = time.monotonic() + timeout
        while token not in self.output:
            if self.process.poll() is not None:
                raise AssertionError("Editor exited before terminal mode setup")
            if time.monotonic() >= deadline:
                raise AssertionError("Terminal output timeout")
            self.collect(0.02)

    def send(self, payload):
        while payload:
            count = os.write(self.master, payload)
            payload = payload[count:]

    def finish(self):
        _, records = self.process.communicate(timeout=5)
        self.records = records
        self.collect(0.05)
        if self.process.returncode:
            raise AssertionError(records.decode("utf-8", "replace"))
        restored = termios.tcgetattr(self.slave)
        original = list(self.original)
        # Darwin sets PENDIN when canonical mode returns. It is kernel input
        # state rather than an editor preference, and clears on the next read.
        pending_input = getattr(termios, "PENDIN", 0)
        restored[3] &= ~pending_input
        original[3] &= ~pending_input
        if restored != original:
            raise AssertionError("Terminal attributes were not restored")
        lines = [bytes.fromhex(line[5:].decode()) for line in records.splitlines()
                 if line.startswith(b"LINE:")]
        screen = Screen(self.columns)
        screen.feed(bytes(self.output))
        return lines, screen

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        if self.process.stderr:
            self.process.stderr.close()
        os.close(self.master)
        os.close(self.slave)


class LineEditorPtyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global BINARY
        if BINARY is not None:
            return
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("a host C compiler is required")
        cls.temporary = tempfile.TemporaryDirectory(prefix="psvr2-line-editor-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        # The standalone editor needs only strdup from the common utility API.
        shim = directory / "util.c"
        shim.write_text("#include <stdlib.h>\n#include <string.h>\n"
                        "char *psvr2_strdup(const char *s) { size_t n=strlen(s)+1; "
                        "char *p=malloc(n); if(p) memcpy(p,s,n); return p; }\n")
        BINARY = directory / "line-editor-driver"
        subprocess.run(
            [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic",
             "-Wconversion", "-Wshadow", "-Werror", "-I" + str(ROOT / "include"),
             "-I" + str(ROOT / "src"), str(ROOT / "src/line_editor.c"),
             str(ROOT / "tests/line_editor_pty_driver.c"), str(shim),
             "-o", str(BINARY)], check=True, capture_output=True, text=True,
        )

    def terminal(self, **options):
        terminal = Terminal(**options)
        self.addCleanup(terminal.close)
        return terminal

    def assert_no_wrap(self, terminal, screen, reads=1):
        self.assertEqual(screen.wraps, 0, bytes(terminal.output))
        self.assertEqual(screen.row, reads)
        self.assertEqual(terminal.output.count(b"\x1b[?2004h"), reads)
        self.assertEqual(terminal.output.count(b"\x1b[?2004l"), reads)

    def test_long_paste_is_buffered_without_prompt_rows_or_marker_suffixes(self):
        terminal = self.terminal(columns=24)
        command = b"upload /tmp/" + b"abcdef0123456789" * 350
        terminal.send(b"\x1b[200~" + command + b"\x1b[201~")
        terminal.collect()
        self.assertIsNone(terminal.process.poll())
        self.assertFalse(select.select([terminal.process.stderr], [], [], 0)[0])
        terminal.send(b"\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [command])
        self.assertLess(len(terminal.output), 400)
        self.assert_no_wrap(terminal, screen)

    def test_paste_line_breaks_and_tabs_wait_for_explicit_enter(self):
        terminal = self.terminal(columns=24)
        terminal.send(b"\x1b[200~first\r\nsecond\nthird\targ\x1b[201~")
        terminal.collect()
        self.assertFalse(select.select([terminal.process.stderr], [], [], 0)[0])
        terminal.send(b"\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"first second third arg"])
        self.assert_no_wrap(terminal, screen)

    def test_unbracketed_batch_and_cursor_edits_remain_in_one_row(self):
        terminal = self.terminal(columns=20)
        command = b"0123456789" * 30
        terminal.send(command + b"\x1b[D" * 150 + b"X\x1b[3~\x01H\x05E\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"H" + command[:150] + b"X" + command[151:] + b"E"])
        self.assert_no_wrap(terminal, screen)

    def test_read_ahead_survives_multiple_prompts_and_unknown_csi(self):
        terminal = self.terminal(reads=3, columns=24)
        terminal.send(b"first\rsecond\x1b[24~\rthird\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"first", b"second", b"third"])
        self.assert_no_wrap(terminal, screen, reads=3)

    def test_unsupported_csi_modifiers_are_consumed_without_cursor_actions(self):
        for sequence in (b"\x1b[?1;3D", b"\x1b[1;4D", b"\x1b[1;3;9C",
                         b"\x1b[200;3~"):
            with self.subTest(sequence=sequence):
                terminal = self.terminal(columns=40)
                terminal.send(b"alpha" + sequence + b"X\r")
                lines, screen = terminal.finish()
                self.assertEqual(lines, [b"alphaX"])
                self.assert_no_wrap(terminal, screen)

    def test_history_recall_restores_draft_without_old_wrapped_rows(self):
        terminal = self.terminal(reads=2, columns=24)
        command = b"long-history-command-" * 15
        terminal.send(command + b"\rdraft\x1b[A\x1b[B\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [command, b"draft"])
        self.assert_no_wrap(terminal, screen, reads=2)

    def test_wide_utf8_cursor_and_deletion_keep_complete_characters(self):
        terminal = self.terminal(columns=24)
        command = "界" * 30
        terminal.send(command.encode() + b"\x1b[D\x1b[3~\x7fX\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [("界" * 28 + "X").encode()])
        self.assert_no_wrap(terminal, screen)

    def test_long_prompt_is_clipped_and_eof_restores_terminal_mode(self):
        terminal = self.terminal(columns=16, prompt="PSVR2:/" + "directory/" * 20 + " # ")
        terminal.send(b"\x04")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [])
        self.assert_no_wrap(terminal, screen)

    def test_tab_completes_unique_file_and_adds_space(self):
        terminal = self.terminal(columns=40, completion=True)
        terminal.send(b"./open\t--stream\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./open_vrhmd --stream"])
        self.assertEqual(terminal.records.count(b"COMPLETE\n"), 1)
        self.assert_no_wrap(terminal, screen)

    def test_completion_stays_in_one_row_on_narrow_terminal(self):
        terminal = self.terminal(columns=12, completion=True)
        terminal.send(b"./open\t--stream\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./open_vrhmd --stream"])
        self.assert_no_wrap(terminal, screen)

    def test_tab_completes_directory_with_slash(self):
        terminal = self.terminal(columns=40, completion=True)
        terminal.send(b"cd ./dir\tchild\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"cd ./directory/child"])
        self.assert_no_wrap(terminal, screen)

    def test_ambiguous_tab_extends_common_prefix_then_lists_matches(self):
        terminal = self.terminal(columns=40, completion=True)
        terminal.send(b"./al\t")
        terminal.collect()
        self.assertNotIn(b"./alpha", terminal.output)
        terminal.send(b"\t\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./alp"])
        self.assertIn(b"./alpha", terminal.output)
        self.assertIn(b"./alpine", terminal.output)
        self.assertEqual(terminal.records.count(b"COMPLETE\n"), 2)
        self.assertEqual(screen.wraps, 0, bytes(terminal.output))

    def test_ambiguous_completion_does_not_insert_incomplete_escape(self):
        terminal = self.terminal(columns=40, completion=True)
        terminal.send(b"./a\t\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./a"])
        self.assertEqual(terminal.records.count(b"COMPLETE\n"), 1)
        self.assert_no_wrap(terminal, screen)

    def test_ambiguous_completion_normalizes_quoted_prefix_then_lists(self):
        terminal = self.terminal(columns=40, completion=True)
        terminal.send(b"'./al\t")
        terminal.collect()
        self.assertNotIn(b"./alpha", terminal.output)
        terminal.send(b"\t\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./alp"])
        self.assertIn(b"./alpha", terminal.output)
        self.assertIn(b"./alpine", terminal.output)
        self.assertEqual(terminal.records.count(b"COMPLETE\n"), 2)
        self.assertEqual(screen.wraps, 0, bytes(terminal.output))

    def test_ambiguous_completion_keeps_complete_utf8_characters(self):
        terminal = self.terminal(columns=40, completion=True)
        terminal.send(b"./ca\t\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./caf"])
        self.assertEqual(terminal.records.count(b"COMPLETE\n"), 1)
        self.assert_no_wrap(terminal, screen)

    def test_tab_with_no_matches_leaves_input_unchanged(self):
        terminal = self.terminal(completion=True)
        terminal.send(b"./missing\t\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./missing"])
        self.assertEqual(terminal.records.count(b"COMPLETE\n"), 1)
        self.assert_no_wrap(terminal, screen)

    def test_completion_at_cursor_replaces_token_and_preserves_tail(self):
        terminal = self.terminal(columns=48, completion=True)
        terminal.send(b"./openOLD --flag\x01" + b"\x1b[C" * 6 + b"\t\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./open_vrhmd --flag"])
        self.assert_no_wrap(terminal, screen)

    def test_tabs_inside_bracketed_paste_do_not_complete(self):
        terminal = self.terminal(completion=True)
        terminal.send(b"\x1b[200~./open\targument\x1b[201~\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"./open argument"])
        self.assertNotIn(b"COMPLETE", terminal.records)
        self.assert_no_wrap(terminal, screen)

    def test_completion_preserves_shell_escaping_for_spaces(self):
        for prefix, expected in ((b"./My\\ F", b"./My\\ File "),
                                 (b"'./My F", b"'./My File' ")):
            with self.subTest(prefix=prefix):
                terminal = self.terminal(columns=40, completion=True)
                terminal.send(prefix + b"\t\r")
                lines, screen = terminal.finish()
                self.assertEqual(lines, [expected])
                self.assert_no_wrap(terminal, screen)

    def test_option_word_movement_accepts_escape_b_and_f(self):
        terminal = self.terminal(columns=40)
        terminal.send(b"one two three\x1bbX\x1bfY\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"one two XthreeY"])
        self.assert_no_wrap(terminal, screen)

    def test_option_word_movement_stops_at_line_boundaries(self):
        terminal = self.terminal(columns=40)
        terminal.send(b"one two" + b"\x1bb" * 10 + b"X" + b"\x1bf" * 10 + b"Y\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, [b"Xone twoY"])
        self.assert_no_wrap(terminal, screen)

    def test_option_word_movement_accepts_modified_arrow_sequences(self):
        for left, right in ((b"\x1b[1;3D", b"\x1b[1;3C"),
                            (b"\x1b[1;5D", b"\x1b[1;5C"),
                            (b"\x1b[1;9D", b"\x1b[1;9C"),
                            (b"\x1b\x1b[D", b"\x1b\x1b[C")):
            with self.subTest(sequence=left):
                terminal = self.terminal(columns=40)
                terminal.send(b"one two three" + left * 2 + b"X" + right + b"Y\r")
                lines, screen = terminal.finish()
                self.assertEqual(lines, [b"one Xtwo Ythree"])
                self.assert_no_wrap(terminal, screen)

    def test_option_word_movement_preserves_unicode_characters(self):
        terminal = self.terminal(columns=40)
        terminal.send("café 茶葉 gamma".encode() + b"\x1bb\x1bbX\x1bfY\r")
        lines, screen = terminal.finish()
        # Darwin's en_US.UTF-8 iswalnum treats these Chinese characters as
        # separators, as does default macOS zsh; Linux C.UTF-8 groups them.
        expected = "Xcafé 茶葉 Ygamma" if sys.platform == "darwin" else "café X茶葉 Ygamma"
        self.assertEqual(lines, [expected.encode()])
        self.assert_no_wrap(terminal, screen)

    def test_option_word_movement_groups_accented_letters(self):
        terminal = self.terminal(columns=40)
        terminal.send("alpha café gamma".encode() + b"\x1bb\x1bbX\x1bfY\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, ["alpha Xcafé Ygamma".encode()])
        self.assert_no_wrap(terminal, screen)

    def test_option_word_movement_keeps_combining_mark_with_word(self):
        terminal = self.terminal(columns=40)
        terminal.send("cafe\u0301 gamma".encode() + b"\x1bb\x1bbX\x1bfY\r")
        lines, screen = terminal.finish()
        self.assertEqual(lines, ["Xcafe\u0301 Ygamma".encode()])
        self.assert_no_wrap(terminal, screen)

    def test_option_words_keep_path_punctuation_and_use_zsh_separators(self):
        for command, movement, expected in (
                (b"run /tmp/open_vrhmd --stream audio.wav",
                 b"\x1bb" * 3 + b"X\x1bfY",
                 b"run X/tmp/open_vrhmd Y--stream audio.wav"),
                (b"one,two:three+four", b"\x1bb" * 2 + b"X\x1bfY",
                 b"one,two:Xthree+Yfour")):
            with self.subTest(command=command):
                terminal = self.terminal(columns=60)
                terminal.send(command + movement + b"\r")
                lines, screen = terminal.finish()
                self.assertEqual(lines, [expected])
                self.assert_no_wrap(terminal, screen)


if __name__ == "__main__":
    unittest.main()
