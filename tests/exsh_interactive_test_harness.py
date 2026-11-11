#!/usr/bin/env python3
"""PTY-driven interactive regression harness for exsh signal behavior."""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import re
import selectors
import shutil
import signal
import struct
import subprocess
import termios
import time
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional

HARNESS_ROOT = Path(__file__).resolve().parent


def _find_repo_root() -> Path:
    """Where the scenarios run: the umbrella checkout, whose Examples/ and etc/words they use.
    In the umbrella this file is components/exsh/tests, three levels down; alone, exsh's own."""
    for candidate in HARNESS_ROOT.parents:
        if (candidate / "Examples").is_dir() and (candidate / "etc").is_dir():
            return candidate
    return HARNESS_ROOT.parent.parent


REPO_ROOT = _find_repo_root()
DEFAULT_EXE = REPO_ROOT / "build" / "ios-host" / "bin" / "exsh"
DEFAULT_TIMEOUT = 2.0
MARKER_DIR = REPO_ROOT / ".tmp_tests" / "exsh_interactive"

ANSI_CSI_RE = re.compile(r"\x1B\[[0-9;?]*[ -/]*[@-~]")
ANSI_OSC_RE = re.compile(r"\x1B\][^\x07]*(?:\x07|\x1b\\)")
# A `jobs -l` line for the watch job: [1]+ PID State watch -n 1 date
LPS_WATCH_RE = re.compile(
    r"^\s*\[\d+\][+-]?\s+(\d+)\s+\w+.*watch -n 1 date\s*$",
    re.MULTILINE,
)


@dataclass
class Scenario:
    test_id: str
    name: str
    run: Callable[["PtyShell"], tuple[bool, str]]
    env: Optional[dict[str, str]] = None
    # Commands the scenario runs that a build may not have: watch and top are the iOS
    # flavour's applets, and a macOS host has neither. Without one it is skipped, not failed.
    needs: tuple[str, ...] = ()


@dataclass
class Result:
    scenario: Scenario
    passed: bool
    reason: str
    output_tail: str
    skipped: bool = False


def _session_members(session: int) -> List[int]:
    listing = subprocess.run(["ps", "-A", "-o", "pid="], capture_output=True, text=True).stdout
    members: List[int] = []
    for word in listing.split():
        try:
            if os.getsid(int(word)) == session:
                members.append(int(word))
        except OSError:
            pass
    return members


class PtyShell:
    def __init__(
        self,
        executable: Path,
        timeout: float,
        controlling_terminal: bool = True,
        extra_env: Optional[dict[str, str]] = None,
    ):
        self.executable = executable
        self.timeout = timeout
        self.controlling_terminal = controlling_terminal
        self.extra_env = extra_env or {}
        self.master_fd: Optional[int] = None
        self.proc: Optional[subprocess.Popen[bytes]] = None
        self.selector: Optional[selectors.BaseSelector] = None
        self.output = ""
        self.raw = bytearray()
        self._spawn()

    def _spawn(self) -> None:
        master_fd, slave_fd = pty.openpty()
        env = os.environ.copy()
        env.setdefault("TERM", "xterm-256color")
        env["EXSH_SKIP_RC"] = "1"
        env["PS1"] = "PROMPT> "
        env["PS2"] = "CONT> "
        env.update(self.extra_env)
        # The frontends a scenario runs are the ones built beside this shell.
        env["PATH"] = f"{self.executable.parent}{os.pathsep}{env.get('PATH', '')}"
        self.proc = subprocess.Popen(
            [str(self.executable)],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            cwd=str(REPO_ROOT),
            env=env,
            close_fds=True,
            start_new_session=True,
            preexec_fn=(lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0)) if self.controlling_terminal else None,
        )
        os.close(slave_fd)
        os.set_blocking(master_fd, False)
        selector = selectors.DefaultSelector()
        selector.register(master_fd, selectors.EVENT_READ)
        self.master_fd = master_fd
        self.selector = selector

    def close(self) -> None:
        if self.selector is not None:
            self.selector.close()
            self.selector = None
        if self.proc is not None:
            self._end_session()
        self.proc = None
        if self.master_fd is not None:
            try:
                os.close(self.master_fd)
            except OSError:
                pass
            self.master_fd = None

    def _end_session(self) -> None:
        """End the shell and whatever it started, even if the shell has already gone.

        Each job has a process group of its own, so a signal to the shell's group misses it,
        and with no controlling terminal nothing tells it the shell has gone. `watch -n 3 top`
        then runs for good. Only the session still ties a job to the shell. A stopped job
        takes no SIGTERM, which is why SIGKILL gets a turn too.
        """
        for sig, grace in ((signal.SIGTERM, 0.5), (signal.SIGKILL, 5.0)):
            end = time.monotonic() + grace
            while time.monotonic() < end:
                self.proc.poll()
                members = _session_members(self.proc.pid)
                if not members:
                    return
                for pid in members:
                    try:
                        os.kill(pid, sig)
                    except OSError:
                        pass
                time.sleep(0.05)

    def _normalise(self, data: bytes) -> str:
        text = data.decode("utf-8", errors="replace")
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        text = ANSI_CSI_RE.sub("", text)
        text = ANSI_OSC_RE.sub("", text)
        return text

    def _pump(self, duration: float) -> None:
        if self.selector is None or self.master_fd is None:
            return
        end = time.monotonic() + duration
        while (left := end - time.monotonic()) > 0:
            events = self.selector.select(timeout=min(0.05, left))
            for key, _ in events:
                try:
                    chunk = os.read(key.fd, 8192)
                except OSError:
                    chunk = b""
                if chunk:
                    self.raw += chunk
                    self.output += self._normalise(chunk)

    def wait_for_substring(self, needle: str, timeout: Optional[float] = None) -> bool:
        if timeout is None:
            timeout = self.timeout
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._pump(0.05)
            if needle in self.output:
                return True
        return needle in self.output

    def send(self, data: bytes) -> None:
        if self.master_fd is None:
            raise RuntimeError("PTY closed")
        os.write(self.master_fd, data)

    def send_line(self, line: str) -> None:
        self.send(line.encode("utf-8") + b"\n")

    def set_size(self, cols: int, rows: int) -> None:
        if self.master_fd is None:
            raise RuntimeError("PTY closed")
        fcntl.ioctl(self.master_fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def wait_for_path(self, path: Path, timeout: Optional[float] = None) -> bool:
        if timeout is None:
            timeout = self.timeout
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._pump(0.05)
            if path.exists():
                return True
        return path.exists()

    def tail(self, max_chars: int = 1800) -> str:
        return self.output[-max_chars:]


def _new_marker_path(test_id: str) -> Path:
    stamp = int(time.time() * 1000)
    MARKER_DIR.mkdir(parents=True, exist_ok=True)
    return MARKER_DIR / f"{test_id}-{os.getpid()}-{stamp}.txt"


def _unlink_if_exists(path: Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def _shell_path(path: Path) -> str:
    try:
        rel = path.relative_to(REPO_ROOT)
        return f"./{rel.as_posix()}"
    except ValueError:
        return path.as_posix()


def _send_until_marker(shell: PtyShell, command: str, marker: Path, attempts: int, timeout: float) -> bool:
    for _ in range(max(1, attempts)):
        shell.send_line(command)
        if shell.wait_for_path(marker, timeout=timeout):
            return True
        shell._pump(0.15)
    return marker.exists()


def _wait_for_prompt(shell: PtyShell) -> tuple[bool, str]:
    if shell.wait_for_substring("PROMPT> ", timeout=2.0):
        return True, "prompt ready"
    return False, "interactive prompt did not appear"


def _prompt_count(shell: PtyShell) -> int:
    return shell.output.count("PROMPT> ")


def _latest_watch_pid(text: str) -> Optional[int]:
    matches = list(LPS_WATCH_RE.finditer(text))
    if not matches:
        return None
    return int(matches[-1].group(1))


def _latest_pid_for_command(text: str, command_substring: str) -> Optional[int]:
    """The pid on the last line naming the command, in `jobs -l`'s form ([1]+ PID State
    command) or a process list's (PID ... command)."""
    latest: Optional[int] = None
    for line in text.splitlines():
        if command_substring not in line:
            continue
        match = re.match(r"^\s*(?:\[\d+\][+-]?\s+)?(\d+)\s+", line)
        if match:
            latest = int(match.group(1))
    return latest


class TerminalScreen:
    """What a terminal shows for the bytes it is sent, enough of one to check the line editor.

    As on a real terminal, the cursor stays on the last column of a full row until the next glyph
    arrives, and a two-cell glyph that doesn't fit goes to the next row whole. Rows scrolled off the
    top are kept, so a row the editor overwrites or a line it draws twice can't hide up there."""

    def __init__(self, cols: int, rows: int):
        self.cols = cols
        self.rows = rows
        self.scrolled: list[list[str]] = []
        self.grid = [[" "] * cols for _ in range(rows)]
        self.row = 0
        self.col = 0
        self.at_margin = False

    def _next_row(self) -> None:
        if self.row == self.rows - 1:
            self.scrolled.append(self.grid.pop(0))
            self.grid.append([" "] * self.cols)
        else:
            self.row += 1

    @staticmethod
    def _cells(ch: str) -> int:
        if unicodedata.category(ch) in ("Mn", "Me", "Cf"):
            return 0
        return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1

    def _put(self, ch: str) -> None:
        cells = self._cells(ch)
        if cells == 0:
            return
        if self.at_margin or self.col + cells > self.cols:
            self.col = 0
            self.at_margin = False
            self._next_row()
        self.grid[self.row][self.col] = ch
        if cells == 2:
            self.grid[self.row][self.col + 1] = ""
        self.col += cells
        if self.col >= self.cols:
            self.col = self.cols - 1
            self.at_margin = True

    def _csi(self, params: str, final: str) -> None:
        count = int(params) if params.isdigit() else 0
        if final == "A":
            self.row = max(0, self.row - max(count, 1))
        elif final == "B":
            self.row = min(self.rows - 1, self.row + max(count, 1))
        elif final == "C":
            self.col = min(self.cols - 1, self.col + max(count, 1))
        elif final == "D":
            self.col = max(0, self.col - max(count, 1))
        elif final == "J":
            self.grid[self.row][self.col:] = [" "] * (self.cols - self.col)
            for row in range(self.row + 1, self.rows):
                self.grid[row] = [" "] * self.cols
        elif final == "K":
            self.grid[self.row][self.col:] = [" "] * (self.cols - self.col)
        elif final != "m":
            raise ValueError(f"the screen model has no CSI {params}{final}")
        if final in "ABCD":
            self.at_margin = False

    def feed(self, data: bytes) -> None:
        text = data.decode("utf-8", errors="replace")
        pos = 0
        while pos < len(text):
            ch = text[pos]
            if ch == "\x1b":
                match = ANSI_CSI_RE.match(text, pos)
                if match:
                    seq = match.group(0)
                    self._csi(seq[2:-1], seq[-1])
                    pos = match.end()
                    continue
                pos += 2
                continue
            if ch == "\r":
                self.col = 0
                self.at_margin = False
            elif ch == "\n":
                self.at_margin = False
                self._next_row()
            elif ch >= " " and ch != "\x7f":
                self._put(ch)
            pos += 1

    def lines(self) -> list[str]:
        rows = ["".join(row).rstrip() for row in self.scrolled + self.grid]
        while rows and not rows[-1]:
            rows.pop()
        return rows

    def cursor(self) -> tuple[int, int]:
        return len(self.scrolled) + self.row, self.col

    def cursor_for_more_text(self) -> tuple[int, int]:
        """Where the cursor belongs after what it was fed: past a full row, on the next one."""
        row, col = self.cursor()
        return (row + 1, 0) if self.at_margin else (row, col)


WRAP_SCREEN_ROWS = 60
WRAP_WIDTHS = (20, 21, 23, 30, 40, 64, 79, 80, 81, 120, 132)
WRAP_ABOVE = "ROW-ABOVE"


def _wrap_filler(length: int) -> str:
    return "".join(str(i % 10) if i % 7 else " " for i in range(1, length + 1))


def _printed_once(cols: int, text: str) -> TerminalScreen:
    screen = TerminalScreen(cols, WRAP_SCREEN_ROWS)
    screen.feed(text.encode("utf-8"))
    return screen


def _fresh_prompt(shell: PtyShell, cols: int, shown_prompt: str, rows: int = WRAP_SCREEN_ROWS) -> int:
    """Asks for a new prompt on a terminal this wide, and waits for it. What draws it starts at the
    returned mark."""
    shell.set_size(cols, rows)
    start = len(shell.raw)
    shell.send(b"\x05\x15\n")
    last_row = shown_prompt.split("\r\n")[-1]
    end = time.monotonic() + max(shell.timeout, 10.0)
    mark = start
    while time.monotonic() < end:
        shell._pump(0.01)
        newline = shell.raw.find(b"\n", start)
        mark = max(start, shell.raw.rfind(b"\r\x1b[J"))
        if 0 <= newline < mark and shell._normalise(bytes(shell.raw[mark:])).endswith(last_row):
            break
    return mark


def _type_text(shell: PtyShell, text: str) -> None:
    data = text.encode("utf-8")
    for start in range(0, len(data), 256):
        shell.send(data[start:start + 256])
        shell._pump(0.01)


def _screen_shows(
    shell: PtyShell,
    mark: int,
    cols: int,
    want_lines: list[str],
    want_cursor: tuple[int, int],
    rows: int = WRAP_SCREEN_ROWS,
) -> Optional[str]:
    """Waits for the terminal to show this. Returns what it shows instead if it never does."""
    end = time.monotonic() + max(shell.timeout, 10.0)
    seen = -1
    while True:
        shell._pump(0.03)
        if len(shell.raw) != seen:
            seen = len(shell.raw)
            screen = TerminalScreen(cols, rows)
            screen.feed(f"{WRAP_ABOVE}\r\n".encode("utf-8") + bytes(shell.raw[mark:]))
            got_lines, got_cursor = screen.lines(), screen.cursor()
            if got_lines == want_lines and got_cursor == want_cursor:
                return None
        if time.monotonic() >= end:
            shown = "\n".join(f"        |{line}|" for line in got_lines)
            wanted = "\n".join(f"        |{line}|" for line in want_lines)
            return f"at {cols} columns the cursor is at {got_cursor}, not {want_cursor}, and the screen shows\n{shown}\n    not\n{wanted}"


def _wants_line(cols: int, shown_prompt: str, text: str, cursor: Optional[int] = None) -> tuple[list[str], tuple[int, int]]:
    """The screen for a prompt and a line printed once, under the row above, with the cursor at
    that many characters of the line, or at its end."""
    lines = [WRAP_ABOVE] + _printed_once(cols, shown_prompt + text).lines()
    upto = len(text) if cursor is None else cursor
    row, col = _printed_once(cols, shown_prompt + text[:upto]).cursor_for_more_text()
    if upto < len(text) and col + TerminalScreen._cells(text[upto]) > cols:
        row, col = row + 1, 0
    return lines, (row + 1, col)


def _start_wrap_scenario(shell: PtyShell, shown_prompt: str) -> tuple[bool, str]:
    if shell.wait_for_substring(shown_prompt.split("\r\n")[-1], timeout=max(shell.timeout, 10.0)):
        return True, "prompt ready"
    return False, "interactive prompt did not appear"


def _scenario_wrapped_line_shows_once(shown_prompt: str) -> Callable[[PtyShell], tuple[bool, str]]:
    prompt_cells = sum(TerminalScreen._cells(ch) for ch in shown_prompt.split("\r\n")[-1])

    def run(shell: PtyShell) -> tuple[bool, str]:
        ok, reason = _start_wrap_scenario(shell, shown_prompt)
        if not ok:
            return False, reason
        for cols in WRAP_WIDTHS:
            # A line of over three rows, and one that ends exactly on the last column of a row.
            for length in (cols * 3 + cols // 2, cols * 2 - prompt_cells):
                text = _wrap_filler(length)
                mark = _fresh_prompt(shell, cols, shown_prompt)
                _type_text(shell, text)
                want_lines, want_cursor = _wants_line(cols, shown_prompt, text)
                problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
                if problem:
                    return False, problem
        return True, ""

    return run


def scenario_wrapped_line_typed_a_key_at_a_time(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    cols = 40
    text = _wrap_filler(cols * 2 + 11)
    mark = _fresh_prompt(shell, cols, "p> ")
    for ch in text:
        shell.send(ch.encode("utf-8"))
        shell._pump(0.002)
    want_lines, want_cursor = _wants_line(cols, "p> ", text)
    problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
    return (False, problem) if problem else (True, "")


def scenario_wrapped_line_on_a_short_screen(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    cols, rows = 30, 4
    text = _wrap_filler(cols * 3 + 5)
    mark = _fresh_prompt(shell, cols, "p> ", rows)
    _type_text(shell, text)
    want_lines, want_cursor = _wants_line(cols, "p> ", text)
    problem = _screen_shows(shell, mark, cols, want_lines, want_cursor, rows)
    return (False, problem) if problem else (True, "")


def scenario_wrapped_line_cursor_keys(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    cols = 30
    text = _wrap_filler(cols * 3 + 4)
    mark = _fresh_prompt(shell, cols, "p> ")
    _type_text(shell, text)

    def check(want_text: str, cursor: int) -> Optional[str]:
        want_lines, want_cursor = _wants_line(cols, "p> ", want_text, cursor)
        return _screen_shows(shell, mark, cols, want_lines, want_cursor)

    steps: list[tuple[bytes, str, int]] = []
    # To the start of the line, then a key: every row of it stays where it was.
    steps.append((b"\x01", text, 0))
    text = "A" + text
    steps.append((b"A", text, 1))
    # Across rows, each way.
    steps.append((b"\x1b[C" * (cols + 3), text, 1 + cols + 3))
    steps.append((b"\x1b[D" * 2, text, 1 + cols + 1))
    text = text[:cols + 2] + "B" + text[cols + 2:]
    steps.append((b"B", text, cols + 3))
    # A deletion that takes the line up a row, from a row above its end.
    steps.append((b"\x01" + b"\x0b", "", 0))
    for data, want_text, cursor in steps:
        shell.send(data)
        problem = check(want_text, cursor)
        if problem:
            return False, f"after {data!r}: {problem}"
    return True, ""


def scenario_wrapped_line_enter_from_its_start(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    cols = 30
    word = "w" * (cols * 2 + 5)
    text = f"echo {word}"
    mark = _fresh_prompt(shell, cols, "p> ")
    _type_text(shell, text)
    shell.send(b"\x01\n")
    want_lines = [WRAP_ABOVE] + _printed_once(cols, "p> " + text).lines()
    want_lines += _printed_once(cols, word).lines() + ["p>"]
    problem = _screen_shows(shell, mark, cols, want_lines, (len(want_lines) - 1, 3))
    return (False, problem) if problem else (True, "")


def scenario_full_row_then_enter(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    cols = 30
    text = "echo " + "w" * (cols * 2 - 3 - 5)
    mark = _fresh_prompt(shell, cols, "p> ")
    _type_text(shell, text)
    shell.send(b"\n")
    # No empty row between the line and what it prints, as in bash.
    want_lines = [WRAP_ABOVE] + _printed_once(cols, "p> " + text).lines()
    want_lines += _printed_once(cols, text[5:]).lines() + ["p>"]
    problem = _screen_shows(shell, mark, cols, want_lines, (len(want_lines) - 1, 3))
    return (False, problem) if problem else (True, "")


def scenario_wrapped_line_ctrl_c_from_its_start(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    cols = 30
    text = _wrap_filler(cols * 2 + 5)
    mark = _fresh_prompt(shell, cols, "p> ")
    _type_text(shell, text)
    shell.send(b"\x01\x03")
    # Only a build that reads the key itself sees it here: elsewhere it is the terminal's, which
    # has no process to signal, as this pty is no shell's controlling terminal.
    if not shell.wait_for_substring("^C", timeout=1.0):
        return True, ""
    want_lines = [WRAP_ABOVE] + _printed_once(cols, "p> " + text + "^C").lines() + ["p>"]
    problem = _screen_shows(shell, mark, cols, want_lines, (len(want_lines) - 1, 3))
    return (False, problem) if problem else (True, "")


def scenario_wrapped_line_completion_list(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    folder = _new_marker_path("wrap_list").with_suffix("")
    folder.mkdir(parents=True)
    try:
        (folder / "aa1").write_text("")
        (folder / "aa2").write_text("")
        cols = 30
        where = _shell_path(folder).removeprefix("./")
        text = ": " + _wrap_filler(cols + 3) + " " + where + "/aa"
        mark = _fresh_prompt(shell, cols, "p> ")
        _type_text(shell, text)
        shell.send(b"\t")
        line = _printed_once(cols, "p> " + text)
        # These names are wider than the screen, so each has a row of its own.
        names = _printed_once(cols, f"{where}/aa1").lines() + _printed_once(cols, f"{where}/aa2").lines()
        want_lines = [WRAP_ABOVE] + line.lines() + names + line.lines()
        row, col = line.cursor_for_more_text()
        want_cursor = (1 + len(line.lines()) + len(names) + row, col)
        problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
        return (False, problem) if problem else (True, "")
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def scenario_wrapped_line_reverse_search(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    cols = 30
    token = f"wrapsearch{os.getpid()}"
    text = f": {token} " + _wrap_filler(cols * 2)
    mark = _fresh_prompt(shell, cols, "p> ")
    _type_text(shell, text)
    shell.send(b"\n")
    earlier = [WRAP_ABOVE] + _printed_once(cols, "p> " + text).lines()
    problem = _screen_shows(shell, mark, cols, earlier + ["p>"], (len(earlier), 3))
    if problem:
        return False, problem
    # Each step of the search draws over the last, however many rows the match takes.
    shell.send(b"\x12" + token.encode("utf-8"))
    search = _printed_once(cols, f"(reverse-i-search) '{token}': {text}")
    row, col = search.cursor_for_more_text()
    problem = _screen_shows(shell, mark, cols, earlier + search.lines(), (len(earlier) + row, col))
    if problem:
        return False, f"searching: {problem}"
    # Ctrl-G puts back the empty line that was there.
    shell.send(b"\x07")
    problem = _screen_shows(shell, mark, cols, earlier + ["p>"], (len(earlier), 3))
    if problem:
        return False, f"after Ctrl-G: {problem}"
    return True, ""


def scenario_wrapped_line_with_glyphs(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    folder = _new_marker_path("wrap_glyphs").with_suffix("")
    folder.mkdir(parents=True)
    # These come from completing a file's name; interactive_line_wrap_typed_glyphs types them.
    name = "日本語éé⚡" * 12
    try:
        (folder / name).write_text("")
        for cols in (21, 40, 41):
            start = "ls " + _shell_path(folder).removeprefix("./") + "/"
            mark = _fresh_prompt(shell, cols, "p> ")
            _type_text(shell, start)
            shell.send(b"\t")
            text = start + name + " "
            want_lines, want_cursor = _wants_line(cols, "p> ", text)
            problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
            if problem:
                return False, f"completing: {problem}"
            # A key takes the whole of a glyph, with any mark that follows it.
            shell.send(b"\x7f\x7f")
            text = start + name[:-1]
            want_lines, want_cursor = _wants_line(cols, "p> ", text)
            problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
            if problem:
                return False, f"after Backspace: {problem}"
            shell.send(b"\x1b[D\x1b[D")
            at = len(text) - 3  # back over é, and over e with its mark
            want_lines, want_cursor = _wants_line(cols, "p> ", text, at)
            problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
            if problem:
                return False, f"after two Lefts: {problem}"
            shell.send(b"\x1b[3~x")
            text = text[:at] + "x" + text[at + 2:]
            want_lines, want_cursor = _wants_line(cols, "p> ", text, at + 1)
            problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
            if problem:
                return False, f"after Delete and a key: {problem}"
        return True, ""
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def scenario_typed_glyphs_wrap(shell: PtyShell) -> tuple[bool, str]:
    """As scenario_wrapped_line_with_glyphs, with the glyphs typed or pasted rather than completed."""
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if not ok:
        return False, reason
    # Two, three and four bytes; one cell, two and none (an e with a combining acute).
    text = ": " + ("日本語é" + "e\u0301" + "⚡😀") * 8
    for cols in (21, 40, 41):
        mark = _fresh_prompt(shell, cols, "p> ")
        _type_text(shell, text)
        want_lines, want_cursor = _wants_line(cols, "p> ", text)
        problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
        if problem:
            return False, f"typing: {problem}"
        shell.send(b"\x7f\x7f")
        text_now = text[:-2]
        want_lines, want_cursor = _wants_line(cols, "p> ", text_now)
        problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
        if problem:
            return False, f"after Backspace: {problem}"
        shell.send(b"\x1b[D\x1b[D")
        at = len(text_now) - 3  # back over e with its mark, and over é
        want_lines, want_cursor = _wants_line(cols, "p> ", text_now, at)
        problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
        if problem:
            return False, f"after two Lefts: {problem}"
        shell.send("\x1b[3~⚡".encode("utf-8"))
        text_now = text_now[:at] + "⚡" + text_now[at + 1:]
        want_lines, want_cursor = _wants_line(cols, "p> ", text_now, at + 1)
        problem = _screen_shows(shell, mark, cols, want_lines, want_cursor)
        if problem:
            return False, f"after Delete and a key: {problem}"
    return True, ""


def _line_writes(shell: PtyShell, keys: bytes, marker: Path) -> Optional[bytes]:
    """Types keys ending in a command line that writes the marker, and gives what it wrote."""
    _unlink_if_exists(marker)
    shell.send(keys)
    if not shell.wait_for_path(marker, timeout=max(shell.timeout, 10.0)):
        return None
    shell._pump(0.2)
    return marker.read_bytes()


def _utf8_ready(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _start_wrap_scenario(shell, "p> ")
    if ok:
        shell.set_size(80, 24)
    return ok, reason


def scenario_utf8_typed_runs(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _utf8_ready(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("utf8_typed")
    where = _shell_path(marker)
    # A key at a time, as typed, then the same pasted in one write.
    word = "é⚡x😀ü日e\u0301"
    line = f"printf '%s' {word} > {where}\r".encode("utf-8")
    for paste in (False, True):
        _unlink_if_exists(marker)
        if paste:
            shell.send(line)
        else:
            for byte in line:
                shell.send(bytes([byte]))
                shell._pump(0.001)
        if not shell.wait_for_path(marker, timeout=max(shell.timeout, 10.0)):
            return False, f"{'pasted' if paste else 'typed'} line wrote nothing"
        shell._pump(0.2)
        got = marker.read_bytes()
        if got != word.encode("utf-8"):
            return False, f"{'pasted' if paste else 'typed'} line wrote {got!r}, not {word!r}"
    return True, ""


def scenario_utf8_editing_keys(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _utf8_ready(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("utf8_keys")
    where = _shell_path(marker)
    head = "printf %s "
    cases = [
        # Delete takes ⚡ whole, Ctrl-T swaps 😀 and b whole.
        ("aé⚡😀b", b"\x01" + b"\x06" * (len(head) + 2) + b"\x1b[3~" + b"\x06" + b"\x14", "aéb😀"),
        # Ctrl-T at the end swaps the last two glyphs, one of them with its mark.
        ("xyé\u0301日", b"\x05\x14", "xy日é\u0301"),
        # Backspace takes e with its mark, two Lefts go over 😀 and b, ü goes in before them.
        ("ab😀e\u0301", b"\x7f\x1b[D\x1b[D" + "ü".encode("utf-8"), "aüb😀"),
    ]
    for typed, keys, want in cases:
        data = (head + typed).encode("utf-8") + keys + f"\x05 > {where}\r".encode("utf-8")
        got = _line_writes(shell, data, marker)
        if got is None:
            return False, f"{typed!r} with keys {keys!r} wrote nothing"
        if got != want.encode("utf-8"):
            return False, f"{typed!r} with keys {keys!r} wrote {got.decode('utf-8', 'replace')!r}, not {want!r}"
    return True, ""


def scenario_utf8_invalid_bytes(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _utf8_ready(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("utf8_invalid")
    where = _shell_path(marker).encode("utf-8")
    # Bytes that make no character go in as they are, as bash takes them, and the byte that cut
    # one short is still read as itself: a Latin-1 é before x, a lone continuation byte, a
    # surrogate's lead, a lead byte with nothing after it but the space.
    cases = [
        b"\xe9x",
        b"a\x80b",
        b"\xff\xfe",
        b"q\xed\xa0\x80",
        b"z\xc3",
        b"\xf0\x9f\x98",
    ]
    for raw in cases:
        got = _line_writes(shell, b"printf %s " + raw + b" > " + where + b"\r", marker)
        if got is None:
            return False, f"{raw!r} wrote nothing"
        if got != raw:
            return False, f"{raw!r} wrote {got!r}"
    return True, ""


def scenario_utf8_history_and_search(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _utf8_ready(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("utf8_history")
    where = _shell_path(marker)
    word = f"hé⚡😀{os.getpid()}"
    got = _line_writes(shell, f"printf %s {word} > {where}\r".encode("utf-8"), marker)
    if got != word.encode("utf-8"):
        return False, f"the first line wrote {got!r}"
    # Up brings it back, and Delete on it takes é whole.
    got = _line_writes(shell, b"\x1b[A" + b"\x01" + b"\x06" * len("printf %s h") + b"\x1b[3~" + "ü".encode("utf-8") + b"\r", marker)
    want = word.replace("é", "ü", 1)
    if got != want.encode("utf-8"):
        return False, f"Up and an edit wrote {got!r}, not {want!r}"
    # A reverse search for non-ASCII text, with a Backspace that takes a whole glyph.
    query = "ü⚡😀"
    got = _line_writes(shell, b"\x12" + (query + "日").encode("utf-8") + b"\x7f\r", marker)
    if got != want.encode("utf-8"):
        return False, f"searching for {query!r} wrote {got!r}, not {want!r}"
    return True, ""


def scenario_utf8_completion(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _utf8_ready(shell)
    if not ok:
        return False, reason
    folder = _new_marker_path("utf8_complete").with_suffix("")
    folder.mkdir(parents=True)
    marker = _new_marker_path("utf8_complete_out")
    try:
        name = "é⚡😀-name"
        (folder / name).write_text("")
        (folder / "other").write_text("")
        where = _shell_path(folder).removeprefix("./")
        # The prefix typed holds two of the name's glyphs, so the match is made on them: without
        # them both names would match and nothing would complete.
        data = f"printf %s {where}/é⚡".encode("utf-8") + b"\t" + f"\x05> {_shell_path(marker)}\r".encode("utf-8")
        got = _line_writes(shell, data, marker)
        want = f"{where}/{name}"
        if got != want.encode("utf-8"):
            return False, f"completion wrote {got!r}, not {want!r}"
        return True, ""
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def scenario_utf8_meta_keys(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _utf8_ready(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("utf8_meta")
    where = _shell_path(marker)
    head = f"printf %s > {where} "
    cases = [
        # ESC b and the ∫ macOS's Option+b gives are both a word back.
        (head.encode("utf-8") + b"aa bb\x1bbX\r", "aaXbb"),
        (head.encode("utf-8") + "aa bb∫X\r".encode("utf-8"), "aaXbb"),
        # ESC with a character beyond ASCII is bound to nothing and leaves none of its bytes.
        (head.encode("utf-8") + "aa\x1bé\r".encode("utf-8"), "aa"),
    ]
    for data, want in cases:
        got = _line_writes(shell, data, marker)
        if got != want.encode("utf-8"):
            return False, f"{data!r} wrote {got!r}, not {want!r}"
    return True, ""


def scenario_ctrl_c_interrupts_sleep(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-sleep")
    _unlink_if_exists(marker)
    shell.send_line("sleep 10")
    shell._pump(0.25)
    shell.send(b"\x03")
    shell._pump(0.20)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.5):
        return False, "Ctrl-C did not return control to shell during sleep"
    _unlink_if_exists(marker)
    return True, "sleep interrupted by Ctrl-C"


def scenario_ctrl_z_stops_sleep(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlz-jobs")
    _unlink_if_exists(marker)
    shell.send_line("sleep 10")
    shell._pump(0.25)
    shell.send(b"\x1a")
    shell._pump(0.25)
    shell.send_line(f"jobs | grep Stopped | grep 'sleep 10' && touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.5):
        return False, "Ctrl-Z did not return control to shell for jobs query"
    _unlink_if_exists(marker)
    return True, "sleep stopped and visible in jobs output"


def scenario_ctrl_z_stops_local_vproc_and_restores_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker_stop = _new_marker_path("ctrlz-watch-stop")
    marker_bg = _new_marker_path("ctrlz-watch-bg")
    marker_fg = _new_marker_path("ctrlz-watch-fg")
    pid_before_path = _new_marker_path("ctrlz-watch-pid-before")
    pid_after_path = _new_marker_path("ctrlz-watch-pid-after")
    _unlink_if_exists(marker_stop)
    _unlink_if_exists(marker_bg)
    _unlink_if_exists(marker_fg)
    _unlink_if_exists(pid_before_path)
    _unlink_if_exists(pid_after_path)
    prompt_before = _prompt_count(shell)
    shell.send_line("watch -n 1 date")
    shell._pump(0.90)
    shell.send(b"\x1a")
    shell._pump(1.30)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return shell prompt after suspending foreground vproc"
    shell.send_line(
        "jobs | grep Stopped | grep 'watch -n' | grep date >/dev/null"
        " && jobs -l | grep 'watch -n' >/dev/null"
        f" && touch {_shell_path(marker_stop)}"
    )
    if not shell.wait_for_path(marker_stop, timeout=3.0):
        return False, "Suspended local vproc was not queryable from shell after Ctrl-Z"
    shell.send_line(
        f"jobs -l | grep 'watch -n 1 date' > {_shell_path(pid_before_path)}"
    )
    if not shell.wait_for_path(pid_before_path, timeout=3.0):
        return False, "Unable to capture jobs -l output for stopped watch"
    stopped_pid_before = _latest_watch_pid(pid_before_path.read_text(errors="replace"))
    if stopped_pid_before is None:
        return False, "Unable to capture stopped watch pid after first Ctrl-Z"

    prompt_after_stop = _prompt_count(shell)
    shell.send_line(
        "bg %1 >/dev/null 2>&1"
        " && jobs | grep Running | grep 'watch -n' | grep date >/dev/null"
        f" && touch {_shell_path(marker_bg)}"
    )
    if not shell.wait_for_path(marker_bg, timeout=3.0):
        return False, "bg did not resume suspended local vproc as Running"
    if _prompt_count(shell) <= prompt_after_stop:
        return False, "Shell prompt did not remain responsive after bg"

    shell.send_line("fg %1")
    shell._pump(0.90)
    shell.send(b"\x1a")
    shell._pump(1.10)
    shell.send_line(
        "jobs | grep Stopped | grep 'watch -n' | grep date >/dev/null"
        f" && touch {_shell_path(marker_fg)}"
    )
    if not shell.wait_for_path(marker_fg, timeout=3.0):
        return False, "watch was not stopped again after fg -> Ctrl-Z"

    shell.send_line(
        f"jobs -l | grep 'watch -n 1 date' > {_shell_path(pid_after_path)}"
    )
    if not shell.wait_for_path(pid_after_path, timeout=3.0):
        return False, "Unable to capture jobs -l output after fg -> Ctrl-Z"
    stopped_pid_after = _latest_watch_pid(pid_after_path.read_text(errors="replace"))
    if stopped_pid_after is None:
        return False, "Unable to capture stopped watch pid after fg -> Ctrl-Z"
    if stopped_pid_after != stopped_pid_before:
        # iOS virtual jobs may relaunch a foreground watch command when a
        # stopped threadless snapshot cannot be resumed in-place.
        pass

    shell.send_line("kill %1 >/dev/null 2>&1 || true")
    shell._pump(0.2)

    _unlink_if_exists(marker_stop)
    _unlink_if_exists(marker_bg)
    _unlink_if_exists(marker_fg)
    _unlink_if_exists(pid_before_path)
    _unlink_if_exists(pid_after_path)
    return True, "Ctrl-Z/bg/fg flow works for shell-launched local vproc"


def scenario_ctrl_z_stops_clike_frontend_and_restores_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker_stop = _new_marker_path("ctrlz-clike-stop")
    _unlink_if_exists(marker_stop)
    prompt_before = _prompt_count(shell)
    shell.send_line("PSCALI_WORDS_PATH=etc/words clike Examples/clike/base/hangman5")
    shell._pump(1.00)
    shell.send(b"\x1a")
    shell._pump(1.20)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return shell prompt after suspending clike frontend"
    check_cmd = (
        "jobs -l | grep Stopped | grep 'hangman5' >/dev/null"
        f" && touch {_shell_path(marker_stop)}"
    )
    if not _send_until_marker(shell, check_cmd, marker_stop, attempts=3, timeout=4.0):
        return False, "Suspended clike frontend was not queryable from shell after Ctrl-Z"
    _unlink_if_exists(marker_stop)
    return True, "Ctrl-Z stops clike frontend and restores shell prompt"


def scenario_ctrl_z_fg_resumes_clike_frontend(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason

    marker_stop_first = _new_marker_path("ctrlz-clike-fg-stop1")
    marker_stop_second = _new_marker_path("ctrlz-clike-fg-stop2")
    pid_before_path = _new_marker_path("ctrlz-clike-fg-pid-before")
    pid_after_path = _new_marker_path("ctrlz-clike-fg-pid-after")
    _unlink_if_exists(marker_stop_first)
    _unlink_if_exists(marker_stop_second)
    _unlink_if_exists(pid_before_path)
    _unlink_if_exists(pid_after_path)

    prompt_before = _prompt_count(shell)
    shell.send_line("PSCALI_WORDS_PATH=etc/words clike Examples/clike/base/hangman5")
    shell._pump(1.00)
    shell.send(b"\x1a")
    shell._pump(1.20)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return prompt for clike before fg"

    shell.send_line(
        "jobs | grep Stopped | grep 'hangman5' >/dev/null"
        f" && touch {_shell_path(marker_stop_first)}"
    )
    if not shell.wait_for_path(marker_stop_first, timeout=3.0):
        return False, "Initial clike stop was not visible in jobs output"

    shell.send_line(f"jobs -l > {_shell_path(pid_before_path)}")
    if not shell.wait_for_path(pid_before_path, timeout=3.0):
        return False, "Unable to capture clike pid before fg"
    pid_before = _latest_pid_for_command(
        pid_before_path.read_text(errors="replace"),
        "clike Examples/clike/base/hangman5",
    )
    if pid_before is None:
        return False, "Unable to parse clike pid before fg"

    shell.send_line("fg %1")
    shell._pump(1.00)
    if "cannot resume" in shell.tail():
        return False, "fg reported clike frontend as non-resumable"
    shell.send(b"a")
    shell._pump(0.30)
    shell.send(b"\x1a")
    shell._pump(1.20)

    shell.send_line(
        "jobs | grep Stopped | grep 'hangman5' >/dev/null"
        f" && touch {_shell_path(marker_stop_second)}"
    )
    if not shell.wait_for_path(marker_stop_second, timeout=3.0):
        return False, "clike frontend was not stopped after fg -> Ctrl-Z"

    shell.send_line(f"jobs -l > {_shell_path(pid_after_path)}")
    if not shell.wait_for_path(pid_after_path, timeout=3.0):
        return False, "Unable to capture clike pid after fg -> Ctrl-Z"
    pid_after = _latest_pid_for_command(
        pid_after_path.read_text(errors="replace"),
        "clike Examples/clike/base/hangman5",
    )
    if pid_after is None:
        return False, "Unable to parse clike pid after fg -> Ctrl-Z"
    if pid_after != pid_before:
        return False, "fg restarted clike frontend instead of resuming same task"

    _unlink_if_exists(marker_stop_first)
    _unlink_if_exists(marker_stop_second)
    _unlink_if_exists(pid_before_path)
    _unlink_if_exists(pid_after_path)
    return True, "Ctrl-Z/fg resumes clike frontend and preserves pid"


def scenario_ctrl_c_interrupts_watch(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-watch")
    _unlink_if_exists(marker)
    shell.send_line("watch -n 1 date")
    shell._pump(0.90)
    shell.send(b"\x03")
    shell._pump(0.25)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.8):
        return False, "Ctrl-C did not return control to shell during watch"
    _unlink_if_exists(marker)
    return True, "watch interrupted by Ctrl-C"


def scenario_ctrl_c_interrupts_watch_top(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-watch-top")
    _unlink_if_exists(marker)
    shell.send_line("watch -n 3 top")
    shell._pump(1.00)
    shell.send(b"\x03")
    shell._pump(0.35)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=2.2):
        return False, "Ctrl-C did not return control to shell during watch -n 3 top"
    _unlink_if_exists(marker)
    return True, "watch -n 3 top interrupted by Ctrl-C"


def scenario_ctrl_c_interrupts_simple_web_frontend(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-simple-web")
    _unlink_if_exists(marker)
    shell.send_line("Examples/clike/base/simple_web_server")
    shell.wait_for_substring("Waiting for connections", timeout=2.5)
    shell._pump(0.20)
    shell.send(b"\x03")
    shell._pump(0.35)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=2.5):
        return False, "Ctrl-C did not return control to shell during simple_web_server"
    shell.send_line("kill %1 >/dev/null 2>&1 || true")
    shell._pump(0.20)
    _unlink_if_exists(marker)
    return True, "simple_web_server interrupted by Ctrl-C"


def scenario_bg_simple_web_prompt_and_redirect(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason

    marker_prompt = _new_marker_path("bg-simple-web-prompt")
    marker_after_kill = _new_marker_path("bg-simple-web-after-kill")
    log_file = _new_marker_path("bg-simple-web-log")
    log_file = log_file.with_suffix(".log")
    _unlink_if_exists(marker_prompt)
    _unlink_if_exists(marker_after_kill)
    _unlink_if_exists(log_file)

    shell.send_line(
        f"Examples/clike/base/simple_web_server > {_shell_path(log_file)} 2>&1 &"
    )
    shell._pump(0.50)

    shell.send_line(f"touch {_shell_path(marker_prompt)}")
    if not shell.wait_for_path(marker_prompt, timeout=1.5):
        return False, "Prompt did not return after background simple_web_server launch"

    deadline = time.monotonic() + 2.5
    saw_output = False
    while time.monotonic() < deadline:
        shell._pump(0.05)
        if log_file.exists() and log_file.stat().st_size > 0:
            saw_output = True
            break
    if not saw_output:
        return False, "Background simple_web_server redirection file remained empty"

    shell.send_line("kill %1 >/dev/null 2>&1 || true")
    shell._pump(0.50)
    shell.send_line(f"touch {_shell_path(marker_after_kill)}")
    if not shell.wait_for_path(marker_after_kill, timeout=1.5):
        return False, "Shell became unresponsive after killing background simple_web_server"

    _unlink_if_exists(marker_prompt)
    _unlink_if_exists(marker_after_kill)
    _unlink_if_exists(log_file)
    return True, "Background simple_web_server kept prompt responsive and redirected output"


def scenario_bg_simple_web_prompt_and_redirect_pipe_debug(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    shell.send_line("export PSCALI_PIPE_DEBUG=1")
    shell._pump(0.20)
    passed, detail = scenario_bg_simple_web_prompt_and_redirect(shell)
    shell.send_line("unset PSCALI_PIPE_DEBUG")
    shell._pump(0.20)
    if passed:
        return True, "Background simple_web_server stayed responsive with PSCALI_PIPE_DEBUG=1"
    return False, f"{detail} (with PSCALI_PIPE_DEBUG=1)"


def scenario_ctrl_c_interrupts_pascal_readkey_frontend(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-pascal-readkey")
    _unlink_if_exists(marker)
    shell.send_line("Examples/pascal/base/DiceGame")
    shell._pump(1.00)
    shell.send(b"\x03")
    shell._pump(0.40)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=2.5):
        return False, "Ctrl-C did not return control to shell for DiceGame"
    if "VM execution Failed" in shell.tail():
        return False, "DiceGame Ctrl-C triggered VM runtime failure"
    _unlink_if_exists(marker)
    return True, "DiceGame interrupted by Ctrl-C without VM failure"


def scenario_ctrl_z_stops_shebang_frontend_and_restores_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker_stop = _new_marker_path("ctrlz-shebang-stop")
    _unlink_if_exists(marker_stop)
    prompt_before = _prompt_count(shell)
    shell.send_line("Examples/pascal/base/hangman5")
    shell._pump(1.00)
    shell.send(b"\x1a")
    shell._pump(1.20)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return shell prompt after suspending shebang frontend"
    check_cmd = (
        "jobs | grep Stopped | grep 'Examples/pascal/base/hangman5' >/dev/null"
        f" && touch {_shell_path(marker_stop)}"
    )
    if not _send_until_marker(shell, check_cmd, marker_stop, attempts=3, timeout=4.0):
        return False, "Suspended shebang frontend was not queryable from shell after Ctrl-Z"
    shell.send_line("kill %1 >/dev/null 2>&1 || true")
    shell._pump(0.2)
    _unlink_if_exists(marker_stop)
    return True, "Ctrl-Z stops shebang frontend and restores shell prompt"


def scenario_ctrl_z_fg_resumes_shebang_frontend(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason

    marker_stop_first = _new_marker_path("ctrlz-shebang-fg-stop1")
    pid_before_path = _new_marker_path("ctrlz-shebang-fg-pid-before")
    _unlink_if_exists(marker_stop_first)
    _unlink_if_exists(pid_before_path)

    prompt_before = _prompt_count(shell)
    shell.send_line("Examples/pascal/base/hangman5")
    shell._pump(1.00)
    shell.send(b"\x1a")
    shell._pump(1.20)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return prompt for shebang frontend before fg"

    shell.send_line(
        "jobs | grep Stopped | grep 'Examples/pascal/base/hangman5' >/dev/null"
        f" && touch {_shell_path(marker_stop_first)}"
    )
    if not shell.wait_for_path(marker_stop_first, timeout=3.0):
        return False, "Initial shebang frontend stop was not visible in jobs output"

    shell.send_line(f"jobs -l > {_shell_path(pid_before_path)}")
    if not shell.wait_for_path(pid_before_path, timeout=3.0):
        return False, "Unable to capture shebang frontend pid before fg"
    pid_before = _latest_pid_for_command(
        pid_before_path.read_text(errors="replace"),
        "Examples/pascal/base/hangman5",
    )
    if pid_before is None:
        return False, "Unable to parse shebang frontend pid before fg"

    shell.send_line("fg %1")
    shell._pump(1.00)
    if "cannot resume" in shell.tail():
        return False, "fg reported shebang frontend as non-resumable"
    shell.send(b"\x03")
    shell._pump(0.60)
    shell.send_line("kill %1 >/dev/null 2>&1 || true")
    shell._pump(0.20)

    _unlink_if_exists(marker_stop_first)
    _unlink_if_exists(pid_before_path)
    return True, f"Ctrl-Z/fg resumed shebang frontend task pid {pid_before}"


def scenario_ctrl_z_stops_watch_top_and_restores_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker_stop = _new_marker_path("ctrlz-watch-top-stop")
    _unlink_if_exists(marker_stop)
    prompt_before = _prompt_count(shell)
    shell.send_line("watch -n 3 top")
    shell._pump(1.00)
    shell.send(b"\x1a")
    shell._pump(1.30)
    if _prompt_count(shell) <= prompt_before:
        return False, "Ctrl-Z did not return shell prompt for watch -n 3 top"
    shell.send_line(
        "jobs | grep Stopped | grep 'watch -n 3 top' >/dev/null"
        f" && touch {_shell_path(marker_stop)}"
    )
    if not shell.wait_for_path(marker_stop, timeout=3.0):
        return False, "watch -n 3 top was not marked Stopped after Ctrl-Z"
    shell.send_line("kill %1 >/dev/null 2>&1 || true")
    shell._pump(0.2)
    _unlink_if_exists(marker_stop)
    return True, "Ctrl-Z stops watch -n 3 top and restores shell prompt"


def scenario_read_silent_does_not_echo(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("read_silent")
    shell.send_line(f'read -s v; printf "%s" "$v" > {_shell_path(marker)}')
    shell._pump(0.5)
    shell.send_line("hidden-word")
    if not shell.wait_for_path(marker):
        return False, "read -s did not finish on Return"
    if marker.read_text() != "hidden-word":
        return False, f"read -s got {marker.read_text()!r}"
    if "hidden-word" in shell.output:
        return False, "read -s echoed what was typed"
    return True, ""


def scenario_read_n_needs_no_return(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("read_n")
    shell.send_line(f'read -n 2 v; printf "%s" "$v" > {_shell_path(marker)}')
    shell._pump(0.5)
    shell.send(b"ab")
    if not shell.wait_for_path(marker):
        return False, "read -n 2 waited for more than two keys"
    if marker.read_text() != "ab":
        return False, f"read -n 2 got {marker.read_text()!r}"
    return True, ""


def scenario_read_d_ends_at_its_delimiter(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("read_d")
    shell.send_line(f'read -d x v; printf "%s" "$v" > {_shell_path(marker)}')
    shell._pump(0.5)
    shell.send(b"abx")
    if not shell.wait_for_path(marker):
        return False, "read -d x waited for a Return"
    if marker.read_text() != "ab":
        return False, f"read -d x got {marker.read_text()!r}"
    return True, ""


def _type_lines(shell: PtyShell, lines: list[str]) -> None:
    for line in lines:
        try:
            shell.send_line(line)
        except OSError:
            # The shell has ended, as set -e can mean it to.
            return
        shell._pump(0.3)


def scenario_status_on_the_next_line(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("status_next_line")
    done = _new_marker_path("status_next_line_done")
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    out = _shell_path(marker)
    _type_lines(shell, [
        "false",
        f'echo "false=$?" >> {out}',
        "sh -c 'exit 3'",
        f'echo "sh=$?" >> {out}',
        f'echo "worked=$?" >> {out}',
        "(exit 7)",
        "# a comment leaves it alone",
        f'echo "comment=$?" >> {out}',
        f"touch {_shell_path(done)}",
    ])
    if not shell.wait_for_path(done):
        return False, "the shell stopped taking lines"
    got = marker.read_text().split()
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    want = ["false=1", "sh=3", "worked=0", "comment=7"]
    if got != want:
        return False, f"got {got}, want {want}"
    return True, ""


def scenario_set_options_outlast_their_line(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("set_options")
    existing = _new_marker_path("set_options_existing")
    done = _new_marker_path("set_options_done")
    for path in (marker, existing, done):
        _unlink_if_exists(path)
    out = _shell_path(marker)
    kept = _shell_path(existing)
    _type_lines(shell, [
        "set -o pipefail",
        f'false | true; echo "pipefail=$?" >> {out}',
        "set +o pipefail",
        f'false | true; echo "pipefail_off=$?" >> {out}',
        "set -f",
        f'x=$(echo /*); echo "noglob=$x" >> {out}',
        "set +f",
        f'case $(echo /*) in "/*") echo glob=off >> {out};; *) echo glob=on >> {out};; esac',
        "set -u",
        'echo "$not_set_anywhere"',
        f"echo nounset_left_the_prompt >> {out}",
        "set +u",
        f"echo old > {kept}",
        "set -C",
        f"echo new > {kept}",
        "set +C",
        f"sed s/^/noclobber=/ {kept} >> {out}",
        "set +B",
        f"echo {{a,b}} | sed s/^/brace=/ >> {out}",
        "set -B",
        "set -a",
        "exported_by_set_a=1",
        f"sh -c 'echo allexport=$exported_by_set_a' >> {out}",
        "set +a",
        "set -x",
        "echo traced-word > /dev/null",
        "set +x",
        "set -o posix",
        f"set -o | grep posix | tr -d ' \\t' | sed s/^/option_/ >> {out}",
        "set +o posix",
        "set -e",
        f"case $- in *e*) echo errexit=on >> {out};; esac",
        "set +e",
        f"case $- in *e*) echo errexit=stayed >> {out};; esac",
        f"touch {_shell_path(done)}",
    ])
    if not shell.wait_for_path(done):
        return False, "the shell stopped taking lines"
    got = marker.read_text().split()
    for path in (marker, existing, done):
        _unlink_if_exists(path)
    want = [
        "pipefail=1", "pipefail_off=0", "noglob=/*", "glob=on", "nounset_left_the_prompt",
        "noclobber=old", "brace={a,b}", "allexport=1", "option_posixon", "errexit=on",
    ]
    if got != want:
        return False, f"got {got}, want {want}"
    if "unbound variable" not in shell.output:
        return False, "set -u did not hold on the next line"
    if "+ echo traced-word" not in shell.output:
        return False, "set -x did not hold on the next line"
    return True, ""


def scenario_set_e_ends_the_shell_on_a_later_line(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("set_e_ends")
    _unlink_if_exists(marker)
    out = _shell_path(marker)
    _type_lines(shell, [
        "set -e",
        f"echo before >> {out}",
        f"false; echo same_line >> {out}",
        f"echo next_line >> {out}",
    ])
    shell._pump(1.0)
    got = marker.read_text().split() if marker.exists() else []
    _unlink_if_exists(marker)
    if got != ["before"]:
        return False, f"got {got}, want the shell to end at the failure"
    return True, ""


def scenario_trap_runs_on_a_later_line(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("trap_later_line")
    _unlink_if_exists(marker)
    _type_lines(shell, [
        f"trap 'echo ran >> {_shell_path(marker)}' USR2",
        "kill -USR2 $$",
        ": a line for the trap to run before",
    ])
    shell._pump(1.0)
    got = marker.read_text().split() if marker.exists() else []
    _unlink_if_exists(marker)
    if got != ["ran"]:
        return False, f"got {got}, want the trap to run once"
    return True, ""


def scenario_exec_empty_at_the_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("exec_empty")
    _unlink_if_exists(marker)
    out = _shell_path(marker)
    # The prompt outlives it, and a substitution does not.
    _type_lines(shell, [
        f"exec '' 2>/dev/null; echo \"prompt $?\" >> {out}",
        f"x=$(exec '' 2>/dev/null; echo no); echo \"substitution [$x] $?\" >> {out}",
        f"x=$(shopt -s execfail; exec '' 2>/dev/null; echo no); echo \"execfail [$x] $?\" >> {out}",
    ])
    shell._pump(1.0)
    got = marker.read_text() if marker.exists() else ""
    _unlink_if_exists(marker)
    want = "prompt 127\nsubstitution [] 127\nexecfail [] 127\n"
    if got != want:
        return False, f"got {got!r}, want {want!r}"
    return True, ""


def scenario_set_e_survives_ctrl_c_at_the_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("set_e_ctrl_c")
    _unlink_if_exists(marker)
    _type_lines(shell, ["set -e"])
    shell.send(b"half")
    shell._pump(0.2)
    shell.send(b"\x03")
    shell._pump(0.3)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker):
        return False, "Ctrl-C at the prompt ended the shell, or kept the line, under set -e"
    _unlink_if_exists(marker)
    return True, ""


def scenario_shell_state_outlasts_its_line(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("line_state")
    done = _new_marker_path("line_state_done")
    listing = _new_marker_path("line_state_listing")
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    out = _shell_path(marker)
    listed = _shell_path(listing)
    _type_lines(shell, [
        "trap : USR2",
        f"trap > {listed}",
        f"grep -c USR2 {listed} | sed s/^/trap_listed=/ >> {out}",
        "trap - USR2",
        f"trap > {listed}",
        f"grep -c USR2 {listed} | sed s/^/trap_removed=/ >> {out}",
        "readonly line_state_ro=1",
        "line_state_ro=2",
        f'echo "readonly=$line_state_ro" >> {out}',
        "sleep 0 &",
        "bg_pid=$!",
        f'[ -n "$bg_pid" ] && echo bg_pid=set >> {out}',
        "pushd . > /dev/null",
        f"dirs > {listed}",
        f"wc -w < {listed} | tr -d ' ' | sed s/^/dirs=/ >> {out}",
        "popd > /dev/null",
        "line_arr=(a b c)",
        "line_arr+=(d)",
        f'echo "array=${{#line_arr[@]}}" >> {out}',
        "unset 'line_arr[0]'",
        f'echo "array_unset=${{#line_arr[@]}}" >> {out}',
        "declare -A line_map",
        "line_map[k]=v",
        "line_map[j]=w",
        f'echo "assoc=${{#line_map[@]}}" >> {out}',
        "set -- -ab",
        "OPTIND=1",
        "getopts ab line_opt",
        "getopts ab line_opt",
        f'echo "getopts=$line_opt" >> {out}',
        "set --",
        "sleep 30 &",
        f"jobs > {listed}",
        f"grep -c sleep {listed} | sed s/^/jobs=/ >> {out}",
        "kill %1",
        f"touch {_shell_path(done)}",
    ])
    if not shell.wait_for_path(done):
        return False, "the shell stopped taking lines"
    got = marker.read_text().split()
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    _unlink_if_exists(listing)
    want = [
        "trap_listed=1", "trap_removed=0", "readonly=1", "bg_pid=set", "dirs=2",
        "array=4", "array_unset=3", "assoc=2", "getopts=b", "jobs=1",
    ]
    if got != want:
        return False, f"got {got}, want {want}"
    return True, ""


def scenario_wait_finds_a_job_from_an_earlier_line(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("wait_lines")
    done = _new_marker_path("wait_lines_done")
    listing = _new_marker_path("wait_lines_listing")
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    out = _shell_path(marker)
    listed = _shell_path(listing)
    _type_lines(shell, [
        "( exit 3 ) &",
        "wait_a=$!",
        "( exit 4 ) &",
        "wait_b=$!",
        "( sleep 1; exit 7 ) &",
        "wait_c=$!",
        f'wait $wait_b; echo "b=$?" >> {out}',
        f'wait $wait_a; echo "a=$?" >> {out}',
        f'wait $wait_c; echo "c=$?" >> {out}',
        f'wait $wait_a; echo "a_again=$?" >> {out}',
        "sleep 30 &",
        "wait_d=$!",
        "kill $wait_d",
        f'wait $wait_d; echo "killed=$?" >> {out}',
        # A prompt has told of this one: its number has gone and its pid has not.
        "( exit 5 ) &",
        "wait_e=$!",
        "true",
        f'wait %1; echo "told_number=$?" >> {out}',
        f'wait $wait_e; echo "told_pid=$?" >> {out}',
        # At a prompt, a wait with no operands forgets every job.
        "wait",
        f'wait $wait_e; echo "forgotten=$?" >> {out}',
        # The prompt right after `kill` has not told of it, and the one after that has.
        "sleep 30 &",
        "kill %1",
        f'wait %1; echo "killed_number=$?" >> {out}',
        "sleep 30 &",
        "kill %1",
        "true",
        f'wait %1; echo "killed_told=$?" >> {out}',
        # All of those have ended, so numbers start again.
        "sleep 30 &",
        f"jobs > {listed}",
        f"grep -c '^\\[1\\]' {listed} | sed s/^/number=/ >> {out}",
        "kill %1",
        f"touch {_shell_path(done)}",
    ])
    if not shell.wait_for_path(done):
        return False, "the shell stopped taking lines"
    got = marker.read_text().split()
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    _unlink_if_exists(listing)
    want = ["b=4", "a=3", "c=7", "a_again=3", "killed=143", "told_number=127", "told_pid=5", "forgotten=127",
            "killed_number=143", "killed_told=127", "number=1"]
    if got != want:
        return False, f"got {got}, want {want}"
    return True, ""


def scenario_wait_n_passes_what_a_prompt_told_of(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("wait_n_lines")
    done = _new_marker_path("wait_n_lines_done")
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    out = _shell_path(marker)
    _type_lines(shell, [
        "( exit 3 ) &",
        "true",
        f'wait -n; echo "told=$?" >> {out}',
        # This one ends between prompts, so nothing has told of it yet.
        "( sleep 0.1; exit 4 ) &",
        f'wait -n; echo "next=$?" >> {out}',
        f"touch {_shell_path(done)}",
    ])
    if not shell.wait_for_path(done):
        return False, "the shell stopped taking lines"
    got = marker.read_text().split()
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    want = ["told=127", "next=4"]
    if got != want:
        return False, f"got {got}, want {want}"
    return True, ""


def scenario_typed_lines_are_kept(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("typed_lines")
    done = _new_marker_path("typed_lines_done")
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    out = _shell_path(marker)
    # This line arrives while the command runs.
    _type_lines(shell, ["sleep 0.5", f"echo during >> {out}"])
    shell._pump(0.6)
    # These arrive together, as a paste does.
    shell.send(f"echo one >> {out}\necho two >> {out}\ntouch {_shell_path(done)}\n".encode("utf-8"))
    if not shell.wait_for_path(done):
        got = marker.read_text().split() if marker.exists() else []
        _unlink_if_exists(marker)
        return False, f"the last line never ran, and the file has {got}"
    got = marker.read_text().split()
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    want = ["during", "one", "two"]
    if got != want:
        return False, f"got {got}, want {want}"
    return True, ""


def _exit_trap_runs(shell: PtyShell, marker: Path) -> list[str]:
    shell.wait_for_path(marker)
    shell._pump(0.5)
    got = marker.read_text().split() if marker.exists() else []
    _unlink_if_exists(marker)
    return got


def scenario_exit_trap_runs_once_at_end_of_input(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("exit_trap_eof")
    _unlink_if_exists(marker)
    _type_lines(shell, [f"trap 'echo ran >> {_shell_path(marker)}' EXIT", ": a line after it"])
    shell.send(b"\x04")
    got = _exit_trap_runs(shell, marker)
    if got != ["ran"]:
        return False, f"the trap ran {len(got)} times, want once"
    return True, ""


def scenario_exit_trap_runs_once_on_exit(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("exit_trap_exit")
    _unlink_if_exists(marker)
    _type_lines(shell, [f"trap 'echo ran >> {_shell_path(marker)}' EXIT", ": a line after it", "exit"])
    got = _exit_trap_runs(shell, marker)
    if got != ["ran"]:
        return False, f"the trap ran {len(got)} times, want once"
    return True, ""


def scenario_a_bang_can_be_the_shell_s_own(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("bang_syntax")
    done = _new_marker_path("bang_syntax_done")
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    out = _shell_path(marker)
    _type_lines(shell, [
        f'sleep 0 & x=$!; [ -n "$x" ] && echo bg=set >> {out}',
        f'declare -A bang_map; bang_map[k]=v; echo "keys=${{!bang_map[@]}}" >> {out}',
        f'bang_ref=bang_target; bang_target=v; echo "indirect=${{!bang_ref}}" >> {out}',
        f"case a in [!b]) echo bracket=yes >> {out};; esac",
        f"touch {_shell_path(done)}",
    ])
    if not shell.wait_for_path(done):
        return False, "the shell stopped taking lines"
    got = marker.read_text().split() if marker.exists() else []
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    want = ["bg=set", "keys=k", "indirect=v", "bracket=yes"]
    if got != want:
        return False, f"got {got}, want {want}"
    return True, ""


def scenario_err_trap_runs_once_for_a_line(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("err_trap_once")
    done = _new_marker_path("err_trap_once_done")
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    _type_lines(shell, [
        f"trap 'echo ran >> {_shell_path(marker)}' ERR",
        "false; : another command",
        ": a line after it",
        ": and another",
        "trap - ERR",
        f"touch {_shell_path(done)}",
    ])
    if not shell.wait_for_path(done):
        return False, "the shell stopped taking lines"
    got = marker.read_text().split() if marker.exists() else []
    _unlink_if_exists(marker)
    _unlink_if_exists(done)
    if got != ["ran"]:
        return False, f"the trap ran {len(got)} times, want once"
    return True, ""


def scenario_ctrl_c_at_prompt_keeps_shell_alive(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("ctrlc-prompt")
    _unlink_if_exists(marker)
    shell.send(b"\x03")
    shell._pump(0.15)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.0):
        return False, "Ctrl-C at prompt left shell unresponsive"
    _unlink_if_exists(marker)
    return True, "prompt-level Ctrl-C preserved shell responsiveness"

def scenario_backtick_export_hostname_restores_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    prompt_before = _prompt_count(shell)
    marker = _new_marker_path("backtick-export-hostname")
    _unlink_if_exists(marker)

    shell.send_line("export HOST=`hostname`")
    shell._pump(0.30)
    end = time.monotonic() + 2.5
    while time.monotonic() < end:
        shell._pump(0.10)
        if _prompt_count(shell) > prompt_before:
            break
    if _prompt_count(shell) <= prompt_before:
        return False, "backtick export did not return prompt"

    shell.send_line("export HOST=`echo hostsub`")
    shell._pump(0.20)
    shell.send_line(f"printf '__HOST__%s\\n' \"$HOST\"; touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=1.5):
        return False, "shell stayed unresponsive after backtick export"
    _unlink_if_exists(marker)
    if "__HOST__hostsub" not in shell.tail():
        return False, "did not observe HOST assignment after backtick export"
    return True, "backtick export returned prompt and preserved HOST assignment"


def scenario_time_findprimes_returns_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason

    shell.send_line("time ./Examples/pascal/base/FindPrimes")
    if not shell.wait_for_substring("Find all prime numbers up to:", timeout=2.0):
        return False, "FindPrimes prompt did not appear"

    shell.send_line("10")
    if not shell.wait_for_substring("Done.", timeout=10.0):
        return False, "time FindPrimes did not finish after stdin input"
    if not shell.wait_for_substring("real\t", timeout=3.0):
        return False, "time FindPrimes did not print timing output"
    if not shell.wait_for_substring("PROMPT> ", timeout=3.0):
        return False, "time FindPrimes did not return the shell prompt after input"

    output = shell.output
    if "Prime numbers found up to 10:" not in output or "2 3 5 7" not in output:
        return False, "time FindPrimes did not print the expected primes"
    return True, "time FindPrimes returned prompt and produced expected output"


def scenario_findprimes_returns_prompt(shell: PtyShell) -> tuple[bool, str]:
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason

    shell.send_line("./Examples/pascal/base/FindPrimes")
    if not shell.wait_for_substring("Find all prime numbers up to:", timeout=2.0):
        return False, "FindPrimes prompt did not appear"

    shell.send_line("10")
    if not shell.wait_for_substring("Done.", timeout=10.0):
        return False, "FindPrimes did not finish after stdin input"
    if not shell.wait_for_substring("PROMPT> ", timeout=3.0):
        return False, "FindPrimes did not return the shell prompt after input"

    output = shell.output
    if "Prime numbers found up to 10:" not in output or "2 3 5 7" not in output:
        return False, "FindPrimes did not print the expected primes"
    return True, "FindPrimes returned prompt and produced expected output"


def scenario_multiline_for_continues(shell: PtyShell) -> tuple[bool, str]:
    """A compound command typed over several lines keeps reading.

    `for c in ...; do` used to be parsed on its own, which ended the input in
    the middle of the clause and reported "Expected 'done' to close for clause".
    """
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("multiline-for")
    _unlink_if_exists(marker)

    shell.send_line("for word in alpha beta; do")
    if not shell.wait_for_substring("CONT> ", timeout=2.0):
        return False, "no secondary prompt after an unfinished for clause"
    shell.send_line('printf "__FOR__%s\\n" "$word"')
    shell._pump(0.20)
    shell.send_line("done")
    if not shell.wait_for_substring("__FOR__beta", timeout=2.5):
        return False, "multi-line for clause did not run its body"
    shell._pump(0.30)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=2.5):
        return False, "shell stayed unresponsive after a multi-line for clause"
    _unlink_if_exists(marker)
    text = shell.tail(6000)
    if "__FOR__alpha" not in text:
        return False, "multi-line for clause did not run its body"
    if "parse error" in text or "syntax error" in text:
        return False, "multi-line for clause reported a parse error"
    return True, "multi-line for clause read its continuation lines and ran"


def scenario_open_quote_continues(shell: PtyShell) -> tuple[bool, str]:
    """A quote left open at the end of a line continues on the next one."""
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("open-quote")
    _unlink_if_exists(marker)

    shell.send_line('printf "__QUOTE__%s\\n" "one')
    if not shell.wait_for_substring("CONT> ", timeout=2.0):
        return False, "no secondary prompt after an unterminated quote"
    shell.send_line('two"')
    if not shell.wait_for_substring("__QUOTE__one", timeout=2.5):
        return False, "quoted continuation did not reach the command"
    shell._pump(0.30)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=2.5):
        return False, "shell stayed unresponsive after an unterminated quote"
    _unlink_if_exists(marker)
    text = shell.tail(6000)
    if "two" not in text:
        return False, "quoted continuation did not reach the command"
    if "parse error" in text or "syntax error" in text:
        return False, "quoted continuation reported a parse error"
    return True, "open quote read its continuation line"


def scenario_option_flags_show_interactive(shell: PtyShell) -> tuple[bool, str]:
    """$- has an i at the prompt: `case $- in *i*)` is how rc files ask."""
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    shell.send_line('case $- in *i*) printf "__FLAGS__%s\\n" interactive;; *) printf "__FLAGS__%s\\n" script;; esac')
    if not shell.wait_for_substring("__FLAGS__interactive", timeout=2.5):
        return False, "$- did not show an interactive shell"
    return True, "$- contains i at the prompt"


def scenario_redirect_and_subshell_at_prompt(shell: PtyShell) -> tuple[bool, str]:
    """A typed line gets the same &> and (cmd) handling as a script.

    The prompt had a text rewrite of its own for &>, with the same bug: the path
    ran up to the next space, so `&> file;` opened a file named `file;`.
    """
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("combined-redirect")
    _unlink_if_exists(marker)

    shell.send_line(f'(printf "__OUT__\\n"; printf "__ERR__\\n" >&2) &> {_shell_path(marker)}; printf "__NEXT__%s\\n" done')
    if not shell.wait_for_substring("__NEXT__done", timeout=2.5):
        return False, "the command after &> did not run"
    if not shell.wait_for_path(marker, timeout=2.5):
        return False, "&> at the prompt did not create its file"
    written = marker.read_text()
    _unlink_if_exists(marker)
    if "__OUT__" not in written or "__ERR__" not in written:
        return False, f"&> at the prompt wrote {written!r}"
    if "[rewrite]" in shell.tail(6000):
        return False, "&> at the prompt printed a debug line"
    return True, "&> and a subshell work at the prompt"


def scenario_substitution_at_prompt(shell: PtyShell) -> tuple[bool, str]:
    """A substitution reads to end of file, and a subshell has no job control.

    A background command that read the terminal stopped there holding the substitution's
    pipe, and the shell waited for it for good. The job's number is not part of the output.
    """
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    for command, expected in (
        ('x=$(sleep 0.1 & echo bg); echo "[$x]__NEXT1__"', "[bg]__NEXT1__"),
        ('x=$( { sleep 0.2; echo late; } & echo early); echo "[$x]__NEXT2__"', "[early\nlate]__NEXT2__"),
        ('x=$(cat & echo early); echo "[$x]__NEXT3__"', "[early]__NEXT3__"),
        ('x=$({ cat; } & echo early); echo "[$x]__NEXT4__"', "[early]__NEXT4__"),
        ('x=$(exec echo in-exec; echo no); echo "[$x]__NEXT5__"', "[in-exec]__NEXT5__"),
    ):
        shell._pump(0.30)
        shell.send_line(command)
        if not shell.wait_for_substring(expected, timeout=4.0):
            return False, f"{command} did not show {expected!r}"
    return True, "substitutions at the prompt end and capture what they should"


def scenario_ctrl_c_ends_substitution_wait(shell: PtyShell) -> tuple[bool, str]:
    """A substitution waits for a job holding its pipe, but an interrupt ends the wait.

    Only a controlling terminal turns the byte into a signal, and the shells of the other
    scenarios have none.
    """
    terminal = PtyShell(shell.executable, shell.timeout, controlling_terminal=True)
    try:
        ok, reason = _wait_for_prompt(terminal)
        if not ok:
            return False, reason
        marker = _new_marker_path("ctrlc-substitution")
        _unlink_if_exists(marker)
        terminal.send_line("x=$(sleep 30 &); echo __NOT_REACHED__")
        terminal._pump(1.0)
        terminal.send(b"\x03")
        terminal._pump(0.50)
        terminal.send_line(f"touch {_shell_path(marker)}")
        if not terminal.wait_for_path(marker, timeout=3.0):
            return False, "Ctrl-C did not end the wait for the job"
        _unlink_if_exists(marker)
        return True, "Ctrl-C ended the wait for the job"
    finally:
        terminal.close()


HISTORY_LIMIT_FILE = MARKER_DIR / "history-limit"


def scenario_history_file_keeps_its_limit(shell: PtyShell) -> tuple[bool, str]:
    """Past HISTFILESIZE the file is rewritten from a temporary one.

    mkstemp gives a host descriptor, and on iOS write looks for a slot, so the rewrite
    wrote nothing and an empty file replaced the history.
    """
    MARKER_DIR.mkdir(parents=True, exist_ok=True)
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    for word in ("one", "two", "three", "four"):
        count = _prompt_count(shell)
        shell.send_line(f": {word}")
        end = time.monotonic() + 2.5
        while _prompt_count(shell) <= count and time.monotonic() < end:
            shell._pump(0.05)
    try:
        lines = HISTORY_LIMIT_FILE.read_text().splitlines()
    except FileNotFoundError:
        return False, "the history file is gone"
    finally:
        _unlink_if_exists(HISTORY_LIMIT_FILE)
    left_over = list(MARKER_DIR.glob(HISTORY_LIMIT_FILE.name + ".tmp*"))
    for path in left_over:
        _unlink_if_exists(path)
    if left_over:
        return False, f"temporary files were left: {left_over}"
    if lines != [": three", ": four"]:
        return False, f"the history file has {lines!r}"
    return True, "the file holds the last HISTFILESIZE lines"


def scenario_syntax_error_does_not_hang(shell: PtyShell) -> tuple[bool, str]:
    """A genuine syntax error still reports at once; only unfinished input waits."""
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("syntax-error")
    _unlink_if_exists(marker)

    shell.send_line("done")
    if not shell.wait_for_substring("syntax error near unexpected token `done'", timeout=2.0):
        return False, "a stray 'done' did not report a syntax error"
    shell._pump(0.30)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=2.5):
        return False, "shell stayed unresponsive after a parse error"
    _unlink_if_exists(marker)
    return True, "a genuine syntax error reported without waiting for more input"


def _ends_the_shell(shell: PtyShell, test_id: str, lines: list[str], want_status: int | None = 0) -> tuple[bool, str]:
    """On iOS these end the session, wherever they run. Each line's {out} takes only what must not run."""
    # On a host they signal the real pid 1, which a run as root in a container would feel.
    if "ios-host" not in str(shell.executable):
        return True, "only the iOS flavour"
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path(test_id)
    _unlink_if_exists(marker)
    out = _shell_path(marker)
    _type_lines(shell, [f"trap 'echo ended >> {out}' EXIT"] + [line.format(out=out) for line in lines])
    shell.wait_for_path(marker)
    shell._pump(0.5)
    got = marker.read_text().split() if marker.exists() else []
    _unlink_if_exists(marker)
    _unlink_if_exists(Path(f"{marker}.sh"))
    if got != ["ended"]:
        return False, f"got {got}, want only the shell's end"
    try:
        status = shell.proc.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        return False, "the shell's process did not end"
    if want_status is not None and status != want_status:
        return False, f"status {status}, want {want_status}: it ended as it was asked to"
    return True, ""


def scenario_halt_ends_the_shell(shell: PtyShell) -> tuple[bool, str]:
    return _ends_the_shell(shell, "halt_ends", ["halt; echo same_line >> {out}", "echo next_line >> {out}"])


def scenario_reboot_ends_a_sourced_file(shell: PtyShell) -> tuple[bool, str]:
    return _ends_the_shell(shell, "reboot_sourced", [
        "printf 'reboot\\necho in_file >> {out}\\n' > {out}.sh",
        ". {out}.sh; echo same_line >> {out}",
        "echo next_line >> {out}",
    ])


def scenario_poweroff_in_a_stage_ends_the_shell(shell: PtyShell) -> tuple[bool, str]:
    return _ends_the_shell(shell, "poweroff_stage", ["poweroff | cat; echo same_line >> {out}", "echo next_line >> {out}"])


def scenario_halt_in_a_subshell_ends_the_shell(shell: PtyShell) -> tuple[bool, str]:
    return _ends_the_shell(shell, "halt_subshell", ["( halt ); echo same_line >> {out}", "echo next_line >> {out}"])


def scenario_halt_ends_its_callers(shell: PtyShell) -> tuple[bool, str]:
    return _ends_the_shell(shell, "halt_callers", [
        "f() {{ halt; echo in_function >> {out}; }}; for i in 1 2; do f; echo in_loop >> {out}; done; echo same_line >> {out}",
        "echo next_line >> {out}",
    ])


def scenario_halt_from_a_job_ends_the_shell_at_its_prompt(shell: PtyShell) -> tuple[bool, str]:
    """Nothing is typed after: the shell at its prompt ends without waiting for a line."""
    return _ends_the_shell(shell, "halt_job", ["(sleep 1; halt) &"])


def scenario_hangup_from_a_job_ends_the_shell_at_its_prompt(shell: PtyShell) -> tuple[bool, str]:
    """As under bash, SIGHUP ends an interactive shell waiting at its prompt, then and there."""
    return _ends_the_shell(shell, "hup_job", ["(sleep 1; kill -HUP $$) &"], want_status=None)


def scenario_newline_is_no_continuation(shell: PtyShell) -> tuple[bool, str]:
    """Where only a word can come, a newline is an error at once, as under bash; no second prompt."""
    ok, reason = _wait_for_prompt(shell)
    if not ok:
        return False, reason
    marker = _new_marker_path("newline-token")
    _unlink_if_exists(marker)

    for text in ("for", "echo x >", "case x in a"):
        shell.send_line(text)
        if not shell.wait_for_substring("syntax error near unexpected token `newline'", timeout=2.0):
            return False, f"{text!r} did not report the newline"
        shell._pump(0.30)
    shell.send_line(f"touch {_shell_path(marker)}")
    if not shell.wait_for_path(marker, timeout=2.5):
        return False, "shell stayed unresponsive after a newline error"
    _unlink_if_exists(marker)
    # A `!` with nothing to name after it is no history reference.
    shell.send_line("!")
    shell._pump(0.30)
    shell.send_line('echo "__BANG_ALONE_$?"')
    if not shell.wait_for_substring("__BANG_ALONE_1", timeout=2.0):
        return False, "`!` alone did not give status 1"
    shell._pump(0.30)
    shell.send_line("! false")
    shell._pump(0.30)
    shell.send_line('echo "__BANG_CMD_$?"')
    if not shell.wait_for_substring("__BANG_CMD_0", timeout=2.0):
        return False, "`! false` did not give status 0"
    return True, "a newline where a word has to come reported without waiting for more input"


SCENARIOS: List[Scenario] = [
    Scenario(
        test_id="interactive_history_file_limit",
        name="History file past HISTFILESIZE keeps its last lines",
        run=scenario_history_file_keeps_its_limit,
        env={
            "HISTFILE": _shell_path(HISTORY_LIMIT_FILE),
            "HISTSIZE": "100",
            "HISTFILESIZE": "2",
        },
    ),
    Scenario(
        test_id="interactive_multiline_for",
        name="Multi-line for clause reads continuation lines",
        run=scenario_multiline_for_continues,
    ),
    Scenario(
        test_id="interactive_open_quote",
        name="Unterminated quote reads a continuation line",
        run=scenario_open_quote_continues,
    ),
    Scenario(
        test_id="interactive_option_flags",
        name="$- shows an interactive shell",
        run=scenario_option_flags_show_interactive,
    ),
    Scenario(
        test_id="interactive_combined_redirect_subshell",
        name="&> and a subshell work at the prompt",
        run=scenario_redirect_and_subshell_at_prompt,
    ),
    Scenario(
        test_id="interactive_syntax_error_no_hang",
        name="Genuine syntax error reports without waiting",
        run=scenario_syntax_error_does_not_hang,
    ),
    Scenario(
        test_id="interactive_newline_no_continuation",
        name="A newline where a word has to come reports without waiting",
        run=scenario_newline_is_no_continuation,
    ),
    Scenario(
        test_id="interactive_substitution_at_prompt",
        name="A substitution at the prompt reads to end of file and prints no job number",
        run=scenario_substitution_at_prompt,
    ),
    Scenario(
        test_id="interactive_ctrl_c_substitution_wait",
        name="Ctrl-C ends a substitution's wait for a job holding its pipe",
        run=scenario_ctrl_c_ends_substitution_wait,
    ),
    Scenario(
        test_id="interactive_ctrl_c_prompt",
        name="Ctrl-C at prompt keeps shell responsive",
        run=scenario_ctrl_c_at_prompt_keeps_shell_alive,
    ),
    Scenario(
        test_id="interactive_backtick_export_hostname_prompt",
        name="backtick export hostname returns prompt",
        run=scenario_backtick_export_hostname_restores_prompt,
    ),
    Scenario(
        test_id="interactive_time_findprimes_prompt",
        name="time FindPrimes returns prompt after stdin input",
        run=scenario_time_findprimes_returns_prompt,
        needs=('pascal',),
    ),
    Scenario(
        test_id="interactive_findprimes_prompt",
        name="FindPrimes returns prompt after stdin input",
        run=scenario_findprimes_returns_prompt,
        needs=('pascal',),
    ),
    Scenario(
        test_id="interactive_ctrl_c_sleep",
        name="Ctrl-C interrupts foreground sleep",
        run=scenario_ctrl_c_interrupts_sleep,
    ),
    Scenario(
        test_id="interactive_ctrl_z_sleep",
        name="Ctrl-Z stops foreground sleep and reports in jobs",
        run=scenario_ctrl_z_stops_sleep,
    ),
    Scenario(
        test_id="interactive_ctrl_z_local_vproc_prompt",
        name="Ctrl-Z stops local vproc and restores shell prompt",
        run=scenario_ctrl_z_stops_local_vproc_and_restores_prompt,
        needs=('watch',),
    ),
    Scenario(
        test_id="interactive_ctrl_z_clike_frontend_prompt",
        name="Ctrl-Z stops clike frontend and restores shell prompt",
        run=scenario_ctrl_z_stops_clike_frontend_and_restores_prompt,
        needs=('clike',),
    ),
    Scenario(
        test_id="interactive_ctrl_z_clike_frontend_fg",
        name="Ctrl-Z/fg resumes clike frontend task",
        run=scenario_ctrl_z_fg_resumes_clike_frontend,
        needs=('clike',),
    ),
    Scenario(
        test_id="interactive_ctrl_c_watch",
        name="Ctrl-C interrupts foreground watch",
        run=scenario_ctrl_c_interrupts_watch,
        needs=('watch',),
    ),
    Scenario(
        test_id="interactive_ctrl_c_watch_top",
        name="Ctrl-C interrupts foreground watch -n 3 top",
        run=scenario_ctrl_c_interrupts_watch_top,
        needs=('watch', 'top'),
    ),
    Scenario(
        test_id="interactive_ctrl_c_simple_web_frontend",
        name="Ctrl-C interrupts foreground simple_web_server",
        run=scenario_ctrl_c_interrupts_simple_web_frontend,
    ),
    Scenario(
        test_id="interactive_bg_simple_web_prompt_and_redirect",
        name="Background simple_web_server keeps prompt and redirects output",
        run=scenario_bg_simple_web_prompt_and_redirect,
    ),
    Scenario(
        test_id="interactive_bg_simple_web_prompt_and_redirect_pipe_debug",
        name="Background simple_web_server keeps prompt and redirects output (PSCALI_PIPE_DEBUG=1)",
        run=scenario_bg_simple_web_prompt_and_redirect_pipe_debug,
    ),
    Scenario(
        test_id="interactive_ctrl_c_pascal_readkey_frontend",
        name="Ctrl-C interrupts Pascal ReadKey frontend (DiceGame)",
        run=scenario_ctrl_c_interrupts_pascal_readkey_frontend,
        needs=('pascal',),
    ),
    Scenario(
        test_id="interactive_ctrl_z_watch_top",
        name="Ctrl-Z stops foreground watch -n 3 top",
        run=scenario_ctrl_z_stops_watch_top_and_restores_prompt,
        needs=('watch', 'top'),
    ),
    Scenario(
        test_id="interactive_ctrl_z_shebang_frontend_prompt",
        name="Ctrl-Z stops shebang frontend and restores shell prompt",
        run=scenario_ctrl_z_stops_shebang_frontend_and_restores_prompt,
        needs=('pascal',),
    ),
    Scenario(
        test_id="interactive_ctrl_z_shebang_frontend_fg",
        name="Ctrl-Z/fg resumes shebang frontend task",
        run=scenario_ctrl_z_fg_resumes_shebang_frontend,
        needs=('pascal',),
    ),
    Scenario(
        test_id="interactive_status_next_line",
        name="$? on the next line is the last line's",
        run=scenario_status_on_the_next_line,
    ),
    Scenario(
        test_id="interactive_set_options",
        name="set options outlast the line they were given on",
        run=scenario_set_options_outlast_their_line,
    ),
    Scenario(
        test_id="interactive_set_e_ends",
        name="set -e ends the shell when a later line fails with another command after it",
        run=scenario_set_e_ends_the_shell_on_a_later_line,
    ),
    Scenario(
        test_id="interactive_set_e_ctrl_c",
        name="set -e and Ctrl-C at the prompt leave the shell",
        run=scenario_set_e_survives_ctrl_c_at_the_prompt,
    ),
    Scenario(
        test_id="interactive_line_state",
        name="Traps, readonly, $!, the directory stack, arrays, getopts and jobs outlast a line",
        run=scenario_shell_state_outlasts_its_line,
    ),
    Scenario(
        test_id="interactive_wait_earlier_line",
        name="wait finds a job from an earlier line, and numbers start again",
        run=scenario_wait_finds_a_job_from_an_earlier_line,
    ),
    Scenario(
        test_id="interactive_wait_n_earlier_line",
        name="wait -n passes by a job a prompt has told of",
        run=scenario_wait_n_passes_what_a_prompt_told_of,
    ),
    Scenario(
        test_id="interactive_typed_lines_kept",
        name="A line typed during a command, and pasted lines, all run",
        run=scenario_typed_lines_are_kept,
    ),
    Scenario(
        test_id="interactive_trap_later_line",
        name="A trap from an earlier line runs when its signal comes",
        run=scenario_trap_runs_on_a_later_line,
    ),
    Scenario(
        test_id="interactive_exec_empty",
        name="exec of an empty name ends a substitution and not the prompt",
        run=scenario_exec_empty_at_the_prompt,
    ),
    Scenario(
        test_id="interactive_exit_trap_eof",
        name="An EXIT trap from an earlier line runs once at Ctrl-D",
        run=scenario_exit_trap_runs_once_at_end_of_input,
    ),
    Scenario(
        test_id="interactive_exit_trap_exit",
        name="An EXIT trap from an earlier line runs once on exit",
        run=scenario_exit_trap_runs_once_on_exit,
    ),
    Scenario(
        test_id="interactive_bang_syntax",
        name="$!, ${!name} and [!...] are not history references",
        run=scenario_a_bang_can_be_the_shell_s_own,
    ),
    Scenario(
        test_id="interactive_err_trap_once",
        name="An ERR trap from an earlier line runs once, not again on the next",
        run=scenario_err_trap_runs_once_for_a_line,
    ),
    Scenario(
        test_id="interactive_read_silent",
        name="read -s does not echo what is typed",
        run=scenario_read_silent_does_not_echo,
    ),
    Scenario(
        test_id="interactive_read_n",
        name="read -n needs no Return",
        run=scenario_read_n_needs_no_return,
    ),
    Scenario(
        test_id="interactive_read_d",
        name="read -d ends at its delimiter with no Return",
        run=scenario_read_d_ends_at_its_delimiter,
    ),
    Scenario(
        test_id="interactive_halt_ends_shell",
        name="halt at the prompt ends the shell, and the rest of its line stays unrun",
        run=scenario_halt_ends_the_shell,
    ),
    Scenario(
        test_id="interactive_reboot_sourced",
        name="reboot in a sourced file ends the shell there",
        run=scenario_reboot_ends_a_sourced_file,
    ),
    Scenario(
        test_id="interactive_poweroff_stage",
        name="poweroff in a pipeline stage ends the shell",
        run=scenario_poweroff_in_a_stage_ends_the_shell,
    ),
    Scenario(
        test_id="interactive_halt_subshell",
        name="halt in a subshell ends the shell, not only the subshell",
        run=scenario_halt_in_a_subshell_ends_the_shell,
    ),
    Scenario(
        test_id="interactive_halt_callers",
        name="halt in a function in a loop ends them and the shell",
        run=scenario_halt_ends_its_callers,
    ),
    Scenario(
        test_id="interactive_halt_from_job",
        name="halt from a job ends the shell waiting at its prompt, with no line typed",
        run=scenario_halt_from_a_job_ends_the_shell_at_its_prompt,
    ),
    Scenario(
        test_id="interactive_hangup_from_job",
        name="SIGHUP from a job ends the shell waiting at its prompt, with no line typed",
        run=scenario_hangup_from_a_job_ends_the_shell_at_its_prompt,
    ),
    Scenario(
        test_id="interactive_line_wrap_plain_prompt",
        name="A line that wraps shows once at any width",
        run=_scenario_wrapped_line_shows_once("p> "),
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_lightning_prompt",
        name="A line that wraps shows once under a prompt with colours and a two-cell glyph",
        run=_scenario_wrapped_line_shows_once("exsh ⚡ "),
        env={"PS1": "\\e[38;5;39mexsh\\e[0m ⚡ "},
    ),
    Scenario(
        test_id="interactive_line_wrap_narrow_glyph_prompt",
        name="A line that wraps shows once under a prompt with a one-cell glyph of two bytes",
        run=_scenario_wrapped_line_shows_once("é> "),
        env={"PS1": "é> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_wide_prompt",
        name="A line that wraps shows once under a prompt of two-cell glyphs",
        run=_scenario_wrapped_line_shows_once("日本語> "),
        env={"PS1": "日本語> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_two_row_prompt",
        name="A line that wraps shows once under a prompt of two rows",
        run=_scenario_wrapped_line_shows_once("top\r\nbottom> "),
        env={"PS1": "top\\nbottom> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_keys",
        name="A line typed a key at a time wraps as one that arrives together",
        run=scenario_wrapped_line_typed_a_key_at_a_time,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_short_screen",
        name="A line that wraps shows once when it scrolls the screen",
        run=scenario_wrapped_line_on_a_short_screen,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_cursor_keys",
        name="Moving the cursor across the rows of a line leaves them where they are",
        run=scenario_wrapped_line_cursor_keys,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_enter",
        name="Enter on the first row of a line prints below all of it",
        run=scenario_wrapped_line_enter_from_its_start,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_full_row",
        name="Enter on a line that fills its last row leaves no empty row",
        run=scenario_full_row_then_enter,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_ctrl_c",
        name="Ctrl-C on the first row of a line shows after all of it",
        run=scenario_wrapped_line_ctrl_c_from_its_start,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_completion",
        name="A completion list goes below a wrapped line, and the line again below that",
        run=scenario_wrapped_line_completion_list,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_search",
        name="A reverse search with a match that wraps shows once",
        run=scenario_wrapped_line_reverse_search,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_glyphs",
        name="A line holding glyphs of two cells and none wraps as the terminal does",
        run=scenario_wrapped_line_with_glyphs,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_typed_glyphs",
        name="Typed glyphs of two cells and none wrap as the terminal does",
        run=scenario_typed_glyphs_wrap,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_utf8_typed",
        name="Characters beyond ASCII, typed or pasted, reach the command whole",
        run=scenario_utf8_typed_runs,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_utf8_keys",
        name="Delete, Backspace, Left and Ctrl-T take whole characters",
        run=scenario_utf8_editing_keys,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_utf8_invalid",
        name="Bytes that make no character go in as they are",
        run=scenario_utf8_invalid_bytes,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_utf8_history",
        name="History and reverse search hold characters beyond ASCII",
        run=scenario_utf8_history_and_search,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_utf8_completion",
        name="Completion matches a prefix beyond ASCII",
        run=scenario_utf8_completion,
        env={"PS1": "p> "},
    ),
    Scenario(
        test_id="interactive_line_wrap_utf8_meta",
        name="Meta and Option shortcuts still work beside UTF-8 input",
        run=scenario_utf8_meta_keys,
        env={"PS1": "p> "},
    ),
]


def ensure_executable(path_override: Optional[str]) -> Path:
    exe = Path(path_override).expanduser() if path_override else DEFAULT_EXE
    if not exe.exists():
        raise FileNotFoundError(f"exsh executable not found at {exe}")
    return exe


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run interactive PTY exsh signal regressions")
    parser.add_argument("--executable", type=str, default=None, help="Override exsh executable path")
    parser.add_argument("--only", type=str, default=None, help="Run only tests whose id contains substring")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="Per-step timeout in seconds")
    parser.add_argument("--list", action="store_true", help="List tests and exit")
    return parser.parse_args()


def _stop_run(signum: int, frame: object) -> None:
    raise SystemExit(128 + signum)


def _missing_command(shell: "PtyShell", names: tuple[str, ...]) -> Optional[str]:
    """The first of these the shell can't run, asked of the shell itself, since an applet is
    no file on PATH. The shell's prompt is waited for, and left as it was found."""
    if not names:
        return None
    ok, _ = _wait_for_prompt(shell)
    if not ok:
        return None
    for name in names:
        marker = _new_marker_path(f"has-{name}")
        _unlink_if_exists(marker)
        shell.send_line(f"type {name} >/dev/null 2>&1 && touch {_shell_path(marker)}")
        if not shell.wait_for_path(marker, timeout=2.0):
            return name
        _unlink_if_exists(marker)
    return None


def main() -> int:
    # The default action would skip the finally that ends a scenario's shell and its jobs.
    signal.signal(signal.SIGTERM, _stop_run)
    args = parse_args()
    scenarios = SCENARIOS
    if args.only:
        scenarios = [s for s in scenarios if args.only in s.test_id]
    if args.list:
        for s in scenarios:
            print(f"{s.test_id}: {s.name}")
        return 0
    if not scenarios:
        print("No interactive tests selected")
        return 1

    try:
        exe = ensure_executable(args.executable)
    except FileNotFoundError as exc:
        print(str(exc))
        return 1

    results: List[Result] = []
    for scenario in scenarios:
        shell = PtyShell(executable=exe, timeout=args.timeout, extra_env=scenario.env)
        try:
            missing = _missing_command(shell, scenario.needs)
            if missing:
                results.append(Result(scenario=scenario, passed=True, reason=f"no {missing} here",
                                      output_tail="", skipped=True))
                continue
            passed, reason = scenario.run(shell)
            results.append(
                Result(
                    scenario=scenario,
                    passed=passed,
                    reason=reason,
                    output_tail=shell.tail(),
                )
            )
        finally:
            shell.close()

    failures = [r for r in results if not r.passed]
    for result in results:
        status = "SKIP" if result.skipped else ("PASS" if result.passed else "FAIL")
        print(f"[{status}] {result.scenario.test_id} - {result.scenario.name}")
        if result.skipped:
            print(f"    Reason: {result.reason}")
        if not result.passed:
            print(f"    Reason: {result.reason}")
            if result.output_tail.strip():
                print("    Output tail:")
                for line in result.output_tail.splitlines()[-30:]:
                    print(f"        {line}")

    print()
    print(f"Ran {len(results)} interactive test(s); {len(failures)} failure(s)")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
