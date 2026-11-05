#!/usr/bin/env python3
"""Owner: the semicolon terminal shell (vexgraph-ecosystem/semicolon/src/main/tui-main.c).

MODULE: tui_main_test.py
REGISTRY: main() builds the shell through its own build.py and proves the shell's
rendering, terminal input, editing, autosave, conflict preservation and Markdown
preview source refresh:
the deterministic --dump frame has the shape of the design, a non-tty launch is
refused with a diagnostic instead of a corrupt screen, an interactive launch under
a real pty enables mouse reporting and scrolls the editor when the terminal sends
a wheel event, and a divider press/drag/release survives without disturbing the
render or the exit. It also proves the colour path end to end: the pty plays the
terminal and answers the OSC 11 background query, and a translucent highlight is
asserted to reach the device as the accent composited over that backdrop - a
different palette index on a dark theme than on a light one, which is the one
result that cannot happen if alpha is quietly ignored. A fenced block is driven
as a copy target: the pty presses its top-right glyph, the block's exact source is
read back from the REAL pasteboard, the glyph becomes a check and lapses again on
its own, and an over-cap block refuses with a notice instead of truncating or
writing an escape. A wide table is panned with a modified wheel: its hidden
columns arrive while the prose above it keeps every cell where it was. The footer
crumb keeps
the first two path components and the last directory and file, with an ellipsis
for the omitted middle, and is proved on a deep workspace path whose full form
outgrows its fixed band.

Why this asserts shape rather than comparing rows to src/tui/tui_example.txt: that
file is the author's notebook as much as a design, and rows 1 and 13 there carry
trailing annotations past the frame, so no row of it is a 112-cell row any more.
The design's measured geometry (the 112-cell canvas, the rail seam at column 50,
the code column at 59) is asserted directly instead, which is the part that has to
keep being true.

Out of scope by design: whether the shell LOOKS right, and whether dragging feels
right. Those belong to the user, who runs `semicolon` and drags it.
"""
from pathlib import Path
import fcntl
import os
import pty
import select
import shutil
import struct
import subprocess
import sys
import re
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "vexgraph-ecosystem/semicolon"
BUILD = APP / "build.py"
BINARY = APP / ".build/semicolon-tui"

# The project the dumped frame is built from. The rail is a real directory listing now, so
# the dump has to be given a root and a file: an ambient cwd would make the frame - and so
# every assertion about it - a guess.
PROJECT_ROOT = APP
PROJECT_FILE = ROOT / "vexgraph-ecosystem/vexspoke/src/oop/type.h"

# The interactive runs are pinned to the same project and file as the dump, and to a
# throwaway state directory: a session cache is per project and lives outside the tree, so
# a test that read or wrote the real one would both depend on the developer's last session
# and overwrite it.
STATE_DIR = None

# The two glyphs the preview's copy affordance cycles through. They are UTF-8 because the
# terminal is, and the test drives them as the pty would see them.
COPY_GLYPH = "⧉".encode()
CHECK_GLYPH = "✓".encode()
FRAME_WIDTH = 112
# The copy glyph keeps one cell of right margin, so on the 112-cell canvas its cell is
# the second from the right edge.
COPY_ICON_COLUMN = FRAME_WIDTH - 2

# The footer reserves a fixed 44-cell band for the crumb, so a deep path is clipped there
# instead of drawing under the status chips.
CRUMB_CELLS = 44
# The borderless design measured from the notebook: the rail is 50 cells and the invisible
# seam is the divider's drag target. The gutter has no bar of its own, so the
# code begins after the seam, a right-aligned line number, its period, and the pad.
RAIL_DIVIDER_COLUMN = 50      # 1-based; the tab strip begins at this cell
CODE_COLUMN = 59              # 1-based for the type.h line-count gutter
# The host paints its own grid, so a run is addressed with CUP and one pen, and the
# divider grip's `|` is the first glyph of its run on every one of its rows. There is
# deliberately no VPA/backspace form to accept: nothing but CUP addresses a cell.
GRIP_ROW_RE = re.compile(rb"\x1b\[(\d+);(\d+)H(?:\x1b\[[0-9;]*m)*\|")
GRIP_START_RE = GRIP_ROW_RE
ROW_ONE_RE = re.compile(rb"\x1b\[1;1H(.*?)(?=\x1b\[\d+;\d+H|\Z)", re.S)
ANSI_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\([A-Za-z0-9]")


def ensure_built():
    result = subprocess.run([sys.executable, str(BUILD)], capture_output=True, text=True, timeout=600)
    if result.returncode:
        print(f"BUILD FAIL\n{result.stdout}{result.stderr}", file=sys.stderr)
        raise SystemExit(1)


def pasteboard_settles(expected, timeout=2.0):
    """Wait, bounded, for a pasteboard write to become visible to `pbpaste`.

    The pasteboard is shared with the desktop, so the reader can lag the writer by a
    moment. A single immediate read makes this a race; the app's write has returned
    before it paints the notice, so waiting on the reader is not hiding a product bug.
    Returns (matched, last_read).
    """
    if shutil.which("pbpaste") is None:
        return True, b""
    deadline = time.monotonic() + timeout
    last = b""
    while True:
        last = subprocess.run(["pbpaste"], capture_output=True).stdout
        if last == expected:
            return True, last
        if time.monotonic() >= deadline:
            return False, last
        time.sleep(0.02)


def dump(width, height, root=None, semicolon_file=None, icons="plain"):
    result = subprocess.run([str(BINARY), "--dump", f"{width}x{height}",
                             "--root", str(root or PROJECT_ROOT),
                              "--file", str(semicolon_file or PROJECT_FILE), "--icons", icons],
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        print(f"FAIL dump exited {result.returncode}\n{result.stderr}", file=sys.stderr)
        raise SystemExit(1)
    return result.stdout.splitlines()


def _own_the_terminal():
    """Make the pty the child's controlling terminal.

    Without this the child is not the pty's foreground process group, so job-control
    signals and terminal reads do not behave like a real shell session. A real terminal
    does this for every program it launches.
    """
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


class Pty:
    """A real pty on which the shell can be driven and watched."""

    def __init__(self, rows, columns, argv=None, env=None):
        self.master, slave = pty.openpty()
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        pinned = {"SEMICOLON_STATE": STATE_DIR} if STATE_DIR else {}
        locale_name = "en_US.UTF-8" if sys.platform == "darwin" else "C.UTF-8"
        self.process = subprocess.Popen(
            argv or [str(BINARY), "--file", str(PROJECT_FILE)],
            stdin=slave, stdout=slave, stderr=slave, close_fds=True, cwd=str(APP),
            preexec_fn=_own_the_terminal,
            env={**os.environ, "SEMICOLON_ICONS": "plain", "LC_ALL": locale_name, "LC_CTYPE": locale_name,
                 **pinned, **(env or {})})
        os.close(slave)
        self.captured = bytearray()
        self.repaint_rows = rows

    def force_repaint(self, rows=22):
        """Resize AWAY and back, so the shell rebinds against a blank mirror at a KNOWN
        size and the whole screen can be read as text.

        Two things had to be true and were not, twice: the size must actually CHANGE or
        nothing repaints at all, and the final size must be PREDICTABLE, because every
        row number below is relative to it. Bouncing through an intermediate height
        gives both.
        """
        other = 21 if rows == 22 else 22
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", other, 112, 0, 0))
        self.drain(0.35)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, 112, 0, 0))
        self.drain(0.55)
        self.rows = rows

    def drain(self, seconds=0.35, cap=3.0):
        start = time.time()
        deadline = start + seconds
        while time.time() < deadline and time.time() - start < cap:
            readable, _, _ = select.select([self.master], [], [], 0.05)
            if not readable:
                continue
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                return
            if not chunk:
                return
            self.captured += chunk
            # keep reading while output flows, but never past the absolute cap
            deadline = min(time.time() + seconds, start + cap)

    def repaint(self, rows=22):
        """Force a full repaint and return ONLY the bytes it produced.

        The presenter sends changed cells, so any assertion about the whole screen has to
        make the whole screen change - and then judge only what the repaint itself said,
        not the megabytes captured before it.
        """
        before = len(self.captured)
        self.force_repaint(rows=rows)
        return bytes(self.captured[before:])

    def click(self, x, y, button=0):
        """A press and a release on one cell: what a person does with a mouse."""
        self.send(b"\x1b[<%d;%d;%dM" % (button, x + 1, y + 1))
        self.drain(0.2)
        self.send(b"\x1b[<%d;%d;%dm" % (button, x + 1, y + 1))
        self.drain(0.35)

    @staticmethod
    def grip_rows(payload):
        # One match per row whose run begins with the grip glyph. A full repaint sends a
        # whole-row run (the grip is then inside it), so this is only meaningful for a
        # delta - which is exactly what a hover produces.
        return [int(row) for row, _column in GRIP_ROW_RE.findall(payload)]

    @staticmethod
    def row_text(payload, row=1):
        """One row of a FULL repaint as text, independent of how the runs split.

        A full repaint emits each row as one run that begins at column 1, so the whole
        row is there between that address and the next one.
        """
        match = ROW_ONE_RE.search(payload) if row == 1 else None
        if match is None:
            return None
        return ANSI_RE.sub(b"", match.group(1)).decode("utf-8", "replace")

    def wheel(self, x, y, button=65):
        self.send(b"\x1b[<%d;%d;%dM" % (button, x + 1, y + 1))
        self.drain(0.6)

    def send(self, payload):
        os.write(self.master, payload)

    def wait_for(self, needle, seconds=2.0):
        deadline = time.time() + seconds
        while needle not in self.captured and time.time() < deadline:
            self.drain(0.1)
        return needle in self.captured

    def finish(self):
        self.drain(0.2)
        if self.process.poll() is None:
            # Ctrl-C is the only quit key. The editor runs raw(), so 0x03 reaches the
            # event loop as byte 3 instead of becoming SIGINT; the shell handles it
            # explicitly. A letter key is deliberately not a quit shortcut.
            self.send(b"\x03")
            self.drain(0.4)
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        try:
            os.close(self.master)
        except OSError:
            pass
        return self.process.returncode, bytes(self.captured)


# ---------------------------------------------------------------- faded colour
#
# A terminal has no alpha channel - SGR sets ONE colour - so a translucent pen has to be
# composited by the host before the device ever sees it, and what it is composited against
# is the terminal's own background. Both halves are proved here: the backdrop the host
# RESOLVES, and the composite it EMITS. The second is checked against a value computed in
# this file from the same formula, so it cannot pass merely by agreeing with whatever the
# shell happened to do.

PROBE_BG_RE = re.compile(rb"device background: (#[0-9A-F]{6})")
PROBE_SOURCE_RE = re.compile(rb"source: ([^\r\n\x1b]*)")
BACKGROUND_INDEX_RE = re.compile(rb"48;5;(\d+)")
# The shell's COL_SELECT, restated: this test is a client of the design, so it names the
# colour it expects the one highlight to be rather than importing the shell's constant.
COL_SELECT = 0x7AC8FF38
COL_SELECT_OPAQUE = 0x7AC8FFFF     # what it would come out as if alpha were ignored


def blend(top, bottom):
    """`TerminalColor_over` restated: the primitive a fade has to be built from."""
    alpha = top & 0xFF
    keep = 0xFF - alpha
    return tuple((((top >> shift) & 0xFF) * alpha + ((bottom >> shift) & 0xFF) * keep + 127) // 255
                 for shift in (24, 16, 8))


def palette_index(color):
    """The 256-colour cube index the shell maps an opaque colour to."""
    cube = [(value * 5 + 127) // 255 for value in color]
    return 16 + 36 * cube[0] + 6 * cube[1] + cube[2]


def probe_background(reply=None, extra_env=None, seconds=2.5):
    """Run --bg-probe with this pty PLAYING THE TERMINAL.

    We answer the OSC 11 query ourselves, which is the only way to prove a reply is parsed
    without depending on the emulator the suite happens to run under. Returns
    (background, source), either of which is None if the shell never reported.
    """
    shell = Pty(24, 112, argv=[str(BINARY), "--bg-probe"], env=extra_env)
    answered = False
    deadline = time.time() + seconds
    while time.time() < deadline:
        shell.drain(0.05)
        if not answered and b"\x1b]11;?" in shell.captured:
            answered = True
            if reply is not None:
                shell.send(reply)
        if shell.process.poll() is not None and answered:
            break
    if shell.process.poll() is None:
        shell.process.kill()
    shell.process.wait(timeout=5)
    raw = bytes(shell.captured)
    try:
        os.close(shell.master)
    except OSError:
        pass
    where = PROBE_BG_RE.search(raw)
    origin = PROBE_SOURCE_RE.search(raw)
    return (where.group(1).decode() if where else None,
            origin.group(1).decode().strip() if origin else None)


def painted_backgrounds(semicolon_bg):
    """Every background index the shell emits for one full repaint, at a named backdrop.

    TERM is pinned so the shell picks its 256-colour path: the point is to read the
    composited colour, and an 8-colour terminal would collapse it to a primary.
    """
    shell = Pty(22, 112, env={"TERM": "xterm-256color", "SEMICOLON_BG": semicolon_bg})
    shell.drain(1.0)
    shell.force_repaint(rows=22)
    raw = shell.finish()[1]
    return {int(m) for m in BACKGROUND_INDEX_RE.findall(raw)}


def scratch_project(directory):
    """A project small enough to reason about completely.

    A click assertion has to be about the click, not about whatever happens to be in a real
    checkout, so the project is built here: two directories, a nested file, a top-level
    file, a dotfile, and enough siblings to make the rail longer than the screen.
    """
    root = Path(directory) / "proj"
    (root / "alpha").mkdir(parents=True)
    (root / "beta").mkdir(parents=True)
    (root / "alpha" / "one.txt").write_text("alpha one\nalpha two\n")
    (root / "top.txt").write_text("top line\n")
    (root / ".hidden").write_text("hidden\n")
    for index in range(30):
        (root / f"file{index:02d}.txt").write_text("x\n")
    return root


def main():
    global STATE_DIR
    ensure_built()
    failures = 0
    state = tempfile.TemporaryDirectory(prefix="semicolon-state-")
    STATE_DIR = state.name

    def check(condition, message):
        nonlocal failures
        if not condition:
            print(f"FAIL {message}", file=sys.stderr)
            failures += 1

    # --- The dumped frame has the design's shape, over a real file ---
    lines = dump(FRAME_WIDTH, 23)
    check(len(lines) == 23, f"dump produced {len(lines)} rows, expected 23")
    if len(lines) == 23:
        chrome, body, status = lines[0], lines[1], lines[22]
        # Row 0 is the header: the tab strip starts at the seam, and the rail's columns
        # above the first body row show the pinned project and muted location.
        check("semicolon (main project)" not in chrome,
              f"project root leaked into the tab row: {chrome[:40]!r}")
        check(chrome[RAIL_DIVIDER_COLUMN] == "[",
              f"the tab strip does not start at the seam: "
              f"{chrome[RAIL_DIVIDER_COLUMN:RAIL_DIVIDER_COLUMN + 18]!r}")
        check("[- type.h  \uf00d]" in chrome, "the tab strip does not name the open file")
        check("placeholder" not in chrome and "helloWorld.c" not in chrome,
              "the tab strip still contains placeholder files")
        check("[! off]" in body, "the action chips are missing from the editor")
        # The rail is the PROJECT: the root it was given, opened, with its own children
        # beneath it and its directories collapsed. The PTY forces a UTF-8 character locale
        # and proves the actual wide-curses device output, not just the Unicode canvas dump.
        check(chrome.startswith(" ⌄ [] semicolon"), f"the rail header is wrong: {chrome[:50]!r}")
        rail = "".join(line[:RAIL_DIVIDER_COLUMN] for line in lines[1:23])
        check("› [] src" in rail, "the rail does not show the project's directories")
        check("README.md" in rail, "the rail does not show the project's files")
        # Dotfiles are browsable through the API but hidden here, like every file pane.
        check(".build" not in rail, "the rail shows dotfiles")
        # The invisible seam is followed by the right-aligned number field, its period,
        # two spaces, then code.
        check("  1.  #ifndef OOP_TYPE_H" in body, "the editor's first line is wrong")
        check("21." in lines[21], "the last visible editor row is not line 21")
        check(lines[1][CODE_COLUMN - 1] == "#", "the code does not start where the gutter ends")
        # The breadcrumb names the ROOT and the file's own path, spaced, because it is read
        # as words. type.h lives outside the rail's project, so the crumb marks it external
        # instead of claiming a root that does not contain it.
        check(status.startswith("[();][>w<] [… > oop > type.h]"),
              f"the breadcrumb row is wrong: {status[:48]!r}")
        check("[semicolon]" in status, "the footer does not name the project")
        check("[ln:1:1]" in status, "the footer does not show the cursor position")
        check("[utf-8]" in status, "the footer does not show the encoding")
        check(re.search(r"\[\d{2}:\d{2}:\d{2}\]", status) is not None,
              f"the footer does not show the clock: {status!r}")

    # The crumb keeps the two ends of a deep path: the checkout and its first child, then
    # the last directory and the file, with the omitted middle collapsed to an ellipsis.
    # The 44-cell band is narrower than today's top-level directory names, so the fixture
    # is a temporary checkout of the shape the band was designed for: the first-two /
    # last-two rule is what is under test, not the current name of the workspace root.
    with tempfile.TemporaryDirectory(prefix="semicolon-deep-") as directory:
        deep_root = Path(directory) / "vexgraph"
        deep_file = deep_root / "suite/personal/api-haven/src/com/discord/discord.h"
        deep_file.parent.mkdir(parents=True)
        deep_file.write_text("int value;\n")
        deep = dump(FRAME_WIDTH, 23, deep_root, deep_file, "plain")
        check(len(deep) == 23, f"deep-path dump produced {len(deep)} rows, expected 23")
        if len(deep) == 23:
            check(deep[22][11:11 + CRUMB_CELLS].startswith(
                      "[vexgraph > suite > … > discord > discord.h]"),
                  f"the deep crumb does not keep both ends: {deep[22][:60]!r}")
            check(deep[22][11 + CRUMB_CELLS] == " ",
                  "the deep crumb ran past its fixed band into the status chips")
        long_file = deep_root / "suite/personal/api-haven/src/com/discord/discord_webhook.h"
        long_file.write_text("int value;\n")
        long = dump(FRAME_WIDTH, 23, deep_root, long_file, "plain")
        check(len(long) == 23, f"long-path dump produced {len(long)} rows, expected 23")
        if len(long) == 23:
            check(long[22][11:11 + CRUMB_CELLS] ==
                  "[vexgraph > suite > … > discord > discord_we",
                  f"the long crumb is not bounded at the fixed band: "
                  f"{long[22][11:11 + CRUMB_CELLS]!r}")
            check("[utf-8]" in long[22][11 + CRUMB_CELLS:],
                  "the bounded crumb displaced the status chips")

    # Four relative components exactly fill the first-two/last-two breadcrumb shape; no
    # middle component is omitted, so showing an ellipsis here would invent missing path.
    with tempfile.TemporaryDirectory(prefix="semicolon-breadcrumb-") as directory:
        project = Path(directory) / "r"
        exact_depth_file = project / "a" / "b" / "c" / "file.h"
        exact_depth_file.parent.mkdir(parents=True)
        exact_depth_file.write_text("int value;\n")
        exact_depth = dump(FRAME_WIDTH, 23, project, exact_depth_file, "plain")
        check(len(exact_depth) == 23,
              f"exact-depth dump produced {len(exact_depth)} rows, expected 23")
        if len(exact_depth) == 23:
            check(exact_depth[22][11:].startswith("[r > a > b > c > file.h]"),
                  f"an exact four-component path showed a false ellipsis: {exact_depth[22]!r}")
        # The divider stays invisible at rest and on hover; the seam is still interactive.
        for row in (1, 12, 21):
            check(lines[row][RAIL_DIVIDER_COLUMN - 1] != "|", f"row {row} shows an idle divider")
            check(lines[row][0] != "|", f"row {row} still draws a left frame edge")
            check(lines[row][FRAME_WIDTH - 1] != "|", f"row {row} still draws a right frame edge")

    # Nerd icons are filename presentation, not inferred language semantics.
    with tempfile.TemporaryDirectory(prefix="semicolon-icons-") as directory:
        project_icons = Path(directory)
        (project_icons / "folder").mkdir()
        expected = {"a.c": "\ue61e", "b.h": "\uf1dc", "c.py": "\ue73c",
                    "d.rs": "\ue7a8", "e.json": "\ue60b", "f.md": "\ue73e",
                    "g.unknown": "\uf15c", "UPPER.C": "\ue61e",
                    "Makefile": "\uf425", "h.java": "\ue738",
                    "i.js": "\ue74e", "j.cs": "\ue648", "k.ts": "\ue628",
                    "l.r": "\ue68a", "m.f90": "\ue69e", "n.cpp": "\ue61d"}
        for name in expected:
            (project_icons / name).write_text("content\n")
        nerd_frame = "\n".join(dump(160, 22, project_icons, project_icons / "a.c", "nerd"))
        for name, icon in expected.items():
            check(f"{icon} {name}" in nerd_frame, f"missing file icon for {name}")
        check("\uf07c" in nerd_frame and "\uf07b folder" in nerd_frame,
              "open/closed folder icons were not distinct")
        check("[\ue61e a.c  \uf00d]" in nerd_frame,
              "the tab strip did not share the file-type icon lookup")
        base_command = [str(BINARY), "--dump", "160x22", "--root", str(project_icons),
                        "--file", str(project_icons / "a.c")]
        default_env = dict(os.environ)
        default_env.pop("SEMICOLON_ICONS", None)
        default_run = subprocess.run(base_command, env=default_env, capture_output=True,
                                     text=True, timeout=60)
        check(default_run.returncode == 0 and "\ue61e a.c" in default_run.stdout,
              "Nerd icons were not the default")
        plain_env = {**default_env, "SEMICOLON_ICONS": "plain"}
        override = subprocess.run(base_command + ["--icons", "nerd"], env=plain_env,
                                  capture_output=True, text=True, timeout=60)
        check(override.returncode == 0 and "\ue61e a.c" in override.stdout,
              "explicit icon setting did not override the environment")
        plain_frame = "\n".join(dump(160, 22, project_icons, project_icons / "a.c"))
        catalog_glyphs = set(expected.values()) | {"\uf07b", "\uf07c"}
        check(not any(glyph in plain_frame for glyph in catalog_glyphs),
              "plain icon setting leaked catalog glyphs")
        check("- a.c" in plain_frame,
              "plain icon setting did not use the ASCII file marker")
        color_cases = {
            "a.c": ("\ue61e", 147), "b.h": ("\uf1dc", 147),
            "c.py": ("\ue73c", 79), "d.rs": ("\ue7a8", 181),
            "e.json": ("\ue60b", 183), "f.md": ("\ue73e", 111),
            "UPPER.C": ("\ue61e", 147), "g.unknown": ("\uf15c", 145),
            "Makefile": ("\uf425", 180), "h.java": ("\ue738", 215),
            "i.js": ("\ue74e", 221), "j.cs": ("\ue648", 114),
            "k.ts": ("\ue628", 75), "l.r": ("\ue68a", 110),
            "m.f90": ("\ue69e", 140), "n.cpp": ("\ue61d", 176),
            "folder": ("\uf07b", 180),
        }
        icon_pty = Pty(32, 160,
                       argv=[str(BINARY), "--root", str(project_icons), "--file",
                             str(project_icons / "a.c")],
                       env={"SEMICOLON_ICONS": "nerd", "TERM": "xterm-256color"})
        check(icon_pty.wait_for("\uf07c".encode()), "wide curses omitted Nerd folder glyph")
        icon_pty.drain(0.2)
        icon_output = bytes(icon_pty.captured)
        # A pen starts with `0`, so the colour parameter is never the first thing
        # after `ESC [`; the pattern accepts the reset and any weights before it.
        for filename, (glyph, color) in color_cases.items():
            colored_icon = re.escape(glyph.encode())
            pattern = (rb"\x1b\[0;(?:[0-9]+;)*38;5;" + str(color).encode()
                       + rb"m(?:\x1b\[[0-9;]*m)*" + colored_icon)
            check(re.search(pattern, icon_output) is not None,
                  f"{filename} icon did not use its intended indexed color {color}")
        check(re.search(rb"\x1b\[0;38;5;231(?:;[0-9]+)*m(?:\x1b\[[0-9;]*m)* a\.c",
                        icon_output) is not None,
              "the file label was not white beside its colored icon")
        check(re.search(rb"\x1b\[0;38;5;180(?:;[0-9]+)*m(?:\x1b\[[0-9;]*m)*\xef\x81\xbb"
                        rb"\x1b\[0;38;5;231(?:;[0-9]+)*m folder", icon_output) is not None,
              "the folder icon was not colored independently from its white label")
        folder_hover_start = len(icon_pty.captured)
        icon_pty.send(b"\x1b[<35;10;3M")
        icon_pty.drain(0.2)
        folder_hover = bytes(icon_pty.captured[folder_hover_start:])
        check(b"38;5;145" not in folder_hover,
              "hovering a folder label changed its text to the former hover shade")
        hover_start = len(icon_pty.captured)
        icon_pty.send(b"\x1b[<35;10;4M")  # pointer over the a.c label, not its icon
        icon_pty.drain(0.2)
        hovered_name = bytes(icon_pty.captured[hover_start:])
        check(b"38;5;145" not in hovered_name,
              "hovering a file label changed its text to the former hover shade")
        code, _ = icon_pty.finish()
        check(code == 0, "colored-icon PTY did not shut down cleanly")

    with tempfile.TemporaryDirectory(prefix="semicolon-markdown-") as directory:
        markdown_project = Path(directory)
        markdown_file = markdown_project / "preview.md"
        markdown_file.write_text("# Preview Heading\n\nA **BOLD_MARKER** paragraph with `code`.\n")
        markdown_frame = "\n".join(dump(100, 14, markdown_project, markdown_file, "plain"))
        check("Code" in markdown_frame and "Preview" in markdown_frame,
              "Markdown mode controls were not shown in Code-then-Preview order")
        check("open in browser" not in markdown_frame,
              "Markdown preview still offers an external-browser action")
        check("[] []" not in markdown_frame,
              "empty language hint chips left bracket artifacts in the Markdown header")
        check("20!!, 20!" not in markdown_frame,
              "Markdown still shows per-language hint buttons")
        check("Preview Heading" in markdown_frame and "# Preview Heading" not in markdown_frame,
              "Markdown did not open in rendered preview mode")
        preview_row = next(line for line in markdown_frame.splitlines()
                           if "Preview Heading" in line)
        check(preview_row.index("Preview Heading") == 53,
              "Markdown preview content did not start three cells after the file rail")
        check(preview_row[50:53] == "   ",
              "Markdown preview seam was visible at rest or its one-cell gaps were lost")
        markdown_tui = Pty(22, 112,
                           argv=[str(BINARY), "--root", str(markdown_project), "--file",
                                 str(markdown_file)],
                           env={"SEMICOLON_ICONS": "plain"})
        check(markdown_tui.wait_for(b"BOLD_MARKER"),
              "interactive Markdown did not start in preview mode")
        before_divider_hover = len(markdown_tui.captured)
        markdown_tui.send(b"\x1b[<35;51;10M")
        markdown_tui.drain(0.2)
        divider_hover = bytes(markdown_tui.captured[before_divider_hover:])
        markdown_grip = Pty.grip_rows(divider_hover)
        check(markdown_grip == [11, 12, 13],
              f"Markdown divider grip rows were {markdown_grip!r}, expected three centered rows")
        before_code = len(markdown_tui.captured)
        markdown_tui.click(94, 2)
        code_delta = bytes(markdown_tui.captured[before_code:])
        check(b"**BOLD_MARKER**" in code_delta,
              "clicking Code did not restore Markdown source")
        markdown_tui.send(b"\x1b[C\x1b[CEdited ")
        markdown_tui.drain(0.2)
        before_preview = len(markdown_tui.captured)
        markdown_tui.click(105, 2)
        preview_delta = bytes(markdown_tui.captured[before_preview:])
        preview_text = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\([A-Za-z0-9]",
                              b"", preview_delta)
        check(b"Edited Preview Heading" in preview_text,
              f"preview did not reparse edits made in Code mode: {preview_text!r}")
        check(b"BOLD_MARKER" in preview_delta and b"**BOLD_MARKER**" not in preview_delta,
              "clicking Preview did not render the edited Markdown source")
        code_exit, _ = markdown_tui.finish()
        check(code_exit == 0, "Markdown mode switching did not shut down cleanly")

        restored_tui = Pty(22, 112,
                           argv=[str(BINARY), "--root", str(markdown_project)],
                           env={"SEMICOLON_ICONS": "plain"})
        check(restored_tui.wait_for(b"BOLD_MARKER"),
              "restored Markdown session did not reopen in its saved Preview mode")
        restored_initial = bytes(restored_tui.captured)
        check(b"**BOLD_MARKER**" not in restored_initial,
              "restored Markdown session showed source instead of its saved Preview mode")
        restored_tui.click(94, 2)
        check(restored_tui.wait_for(b"**BOLD_MARKER**"),
              "restored Markdown mode controls did not switch back to Code")
        restored_exit, _ = restored_tui.finish()
        check(restored_exit == 0, "restored Markdown mode session did not shut down cleanly")

        # THE TABLE PANS; THE DOCUMENT DOES NOT. A warped table is wider than the
        # preview, so a modified wheel reaches its hidden right-hand columns. The
        # prose above it must not move by a single cell while that happens - the
        # pan belongs to the table, not to the page.
        pan_file = markdown_project / "pan.md"
        pan_file.write_text(
            "# Pan Heading\n\n"
            "PROSE_ANCHOR stays fixed.\n\n"
            "| Repository | Workspace path |\n"
            "| :--- | :--- |\n"
            "| FIRSTCOL | vexgraph-hobby-projects/hotcwap/hotcwap-preferences.md |\n"
        )
        pan_tui = Pty(22, 112,
                      argv=[str(BINARY), "--root", str(markdown_project), "--file",
                            str(pan_file)],
                      env={"SEMICOLON_ICONS": "plain"})
        check(pan_tui.wait_for(b"PROSE_ANCHOR"),
              "the wide-table Markdown preview never painted its prose")
        # The saved mode may be Code (an earlier session leaves it there), so select
        # Preview explicitly and wait for a rendered border before panning.
        pan_tui.click(105, 2)
        check(pan_tui.wait_for("\u250c".encode()),
              "clicking Preview did not render the Markdown table")
        # One notch pans one cell, so a whole first column needs enough notches to
        # leave the viewport: the proof is that the table moved while the 24-cell
        # prose sentence above it is still whole (a document-wide pan would clip it).
        for _ in range(24):
            pan_tui.send(b"\x1b[<73;60;6M")   # option + wheel-down -> pan right
            pan_tui.drain(0.12)
        panned = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\([A-Za-z0-9]",
                        b"", pan_tui.repaint(rows=22))
        check(b"PROSE_ANCHOR stays fixed." in panned,
              "horizontal pan moved the Markdown prose instead of the table alone")
        check(b"Repository" not in panned,
              "the warped table did not pan inside the preview")
        check(b"FIRSTCOL" not in panned,
              "the first table column did not move with the table")
        pan_exit, _ = pan_tui.finish()
        check(pan_exit == 0, "wide-table preview session did not shut down cleanly")

        # A VERTICAL SCROLL MUST NOT BECOME A SCROLL HINT. A device that is asked to
        # shift a region (`rin`, CSI n T), repeat the last glyph (`rep`, CSI n b),
        # erase characters (`ech`, CSI n X) or insert/delete lines (CSI n L/M) is a
        # device being asked to reinterpret the frame - and several terminals list one
        # of those and then handle it differently, which leaves stale or duplicated
        # rows. The host writes every cell at an absolute address instead, so none of
        # them may appear anywhere in the stream; --dump cannot see this, because the
        # device path is not running there.
        scroll_file = markdown_project / "scroll.md"
        scroll_file.write_text(
            "# Scroll Heading\n\n"
            + "".join(f"line {index} keeps the body tall.\n\n" for index in range(60))
            + "| Left | Right |\n| :--- | :--- |\n| ALPHA | omega |\n")
        scroll_tui = Pty(46, 200,
                         argv=[str(BINARY), "--root", str(markdown_project), "--file",
                               str(scroll_file)],
                         env={"SEMICOLON_ICONS": "plain"})
        check(scroll_tui.wait_for(b"Scroll Heading"),
              "the tall Markdown preview never painted its heading")
        scroll_tui.send(b"\x1b[F")           # End: jump to the table at the bottom
        scroll_tui.drain(0.5)
        for _ in range(3):
            scroll_tui.send(b"\x1b[5~")      # PageUp: three full-row scrolls
            scroll_tui.drain(0.35)
        scrolled = bytes(scroll_tui.captured)
        check(re.search(rb"\x1b\[[0-9]*T", scrolled) is None,
              "a scrolled frame asked the terminal for rin (CSI n T); the host writes "
              "absolute cells and a terminal that ignores rin would duplicate rows")
        check(re.search(rb"\x1b\[[0-9]+b", scrolled) is None,
              "the frame asked the terminal to repeat a glyph (rep, CSI n b) instead of "
              "writing the run itself")
        check(re.search(rb"\x1b\[[0-9]+X", scrolled) is None,
              "the frame asked the terminal to erase characters (ech, CSI n X)")
        check(re.search(rb"\x1b\[[0-9]*[LM]", scrolled) is None,
              "the frame asked the terminal to insert or delete a line (CSI n L/M)")
        scroll_exit, _ = scroll_tui.finish()
        check(scroll_exit == 0, "scrolled preview session did not shut down cleanly")

        dynamic_project = markdown_project / "dynamic"
        dynamic_project.mkdir()
        plain_file = dynamic_project / "notes.txt"
        dynamic_markdown = dynamic_project / "preview.md"
        plain_file.write_text("ordinary source\n")
        dynamic_markdown.write_text("# Dynamic Preview\n\n**DYNAMIC_MARKER**\n")
        dynamic_tui = Pty(22, 112,
                          argv=[str(BINARY), "--root", str(dynamic_project), "--file",
                                str(plain_file)],
                          env={"SEMICOLON_ICONS": "plain"})
        check(dynamic_tui.wait_for(b"ordinary source"),
              "ordinary file did not open before the Markdown selection")
        dynamic_tui.click(10, 2)
        check(dynamic_tui.wait_for(b"DYNAMIC_MARKER"),
              "opening Markdown from the rail did not enable rendered preview")
        check(b"placeholder.txt" not in dynamic_tui.captured
              and b"helloWorld.c" not in dynamic_tui.captured,
              "the tab strip still painted placeholder files")
        dynamic_tui.click(52, 0)
        check(dynamic_tui.wait_for(b"ordinary source"),
              "selecting the first file tab did not switch the editor buffer")
        dynamic_tui.click(70, 0)
        check(dynamic_tui.wait_for(b"DYNAMIC_MARKER"),
              "selecting the Markdown tab did not restore its view")
        dynamic_tui.click(94, 2)
        dynamic_tui.click(60, 3)
        dynamic_tui.send(b"CARET_MARKER")
        # Let the one-second autosave deadline pass while the Markdown tab is active,
        # so closing it cannot discard the edit that the final file check reads back.
        dynamic_tui.drain(1.3, cap=1.3)
        check(dynamic_markdown.read_text() ==
              "# Dynamic Preview\n\n**DYCARET_MARKERNAMIC_MARKER**\n",
              f"click-to-caret changed the wrong source position: {dynamic_markdown.read_text()!r}")
        dynamic_tui.click(82, 0)
        # The presenter sends only changed cells, so a delta is not the screen: read the
        # screen after a full repaint instead. If the close did not happen the Markdown
        # tab stays active and this text is nowhere on it.
        check(b"ordinary source" in dynamic_tui.repaint(),
              "clicking a tab's Nerd-font close control did not remove it")
        dynamic_tui.click(10, 2)
        check(dynamic_tui.wait_for(b"DYNAMIC_MARKER"),
              "opening a closed file from the tree did not create a fresh tab")
        dynamic_tui.click(70, 0, button=1)
        check(b"ordinary source" in dynamic_tui.repaint(),
              "middle-clicking a tab did not close it and return to the remaining file")
        dynamic_exit, _ = dynamic_tui.finish()
        check(dynamic_exit == 0, "dynamic Markdown selection did not shut down cleanly")
        check("CARET_MARKER" in dynamic_markdown.read_text(),
              "text entered after clicking the caret was not saved to the selected file")

    # A fenced block carries a copy glyph on its top-right cell. A press writes the
    # block through the REAL system clipboard (not an OSC 52 escape), the glyph becomes
    # a check, and the check lapses on its own. The expected text is computed here from
    # the fixture; pbpaste reads the same pasteboard a person would. The over-cap fixture
    # proves the boundary refuses visibly instead of truncating or writing anything.
    with tempfile.TemporaryDirectory(prefix="semicolon-copy-") as directory:
        copy_project = Path(directory)
        copy_file = copy_project / "copy.md"
        copy_file.write_text("# Copymarker\n\n```\nint main(void) {\n    return 0;\n}\n```\n\nAfter paragraph.\n")
        copy_tui = Pty(22, 112,
                       argv=[str(BINARY), "--root", str(copy_project), "--file",
                             str(copy_file)],
                       env={"SEMICOLON_ICONS": "plain"})
        check(copy_tui.wait_for(b"Copymarker"), "the copy fixture did not render")
        copy_tui.repaint()
        initial = bytes(copy_tui.captured)
        check(COPY_GLYPH in initial, "the fenced block did not draw its copy glyph")
        check(b"[copy]" not in initial, "the legacy text chip is still painted")
        check(b"\x1b]52;" not in initial, "the preview still emits an OSC 52 clipboard escape")
        icon_row = None
        for row in range(4, 16):
            before = len(copy_tui.captured)
            copy_tui.click(COPY_ICON_COLUMN, row)
            if CHECK_GLYPH in bytes(copy_tui.captured[before:]):
                icon_row = row
                break
        check(icon_row is not None, "pressing the copy glyph did not show its check")
        if icon_row is not None:
            delta = bytes(copy_tui.captured[-4096:])
            plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\([A-Za-z0-9]", b"", delta)
            check(b"Copied to clipboard" in plain,
                  "a successful copy did not show its notice")
            check(b"\x1b]52;" not in bytes(copy_tui.captured),
                  "a copy wrote an OSC 52 escape instead of using the pasteboard")
            if shutil.which("pbpaste") is not None:
                matched, pasted = pasteboard_settles(b"int main(void) {\n    return 0;\n}")
                check(matched,
                      f"the clipboard did not hold the fenced block: {pasted!r}")
            # The check is feedback, not retained state: the copy glyph comes back when
            # its window closes, even with no further input.
            copy_tui.drain(1.8)
            cleared = copy_tui.repaint()
            check(COPY_GLYPH in cleared and CHECK_GLYPH not in cleared,
                  "the copied check did not lapse back to the copy glyph")
        code_exit, _ = copy_tui.finish()
        check(code_exit == 0, "the copy fixture did not shut down cleanly")

    with tempfile.TemporaryDirectory(prefix="semicolon-copy-cap-") as directory:
        large_project = Path(directory)
        large_file = large_project / "large.md"
        large_file.write_text("# Largemarker\n\n```\n" + ("x" * 70000) + "\n```\n")
        large_tui = Pty(22, 112,
                        argv=[str(BINARY), "--root", str(large_project), "--file",
                              str(large_file)],
                        env={"SEMICOLON_ICONS": "plain"})
        check(large_tui.wait_for(b"Largemarker"), "the over-cap fixture did not render")
        large_tui.repaint()
        large_icon_row = None
        for row in range(4, 16):
            before = len(large_tui.captured)
            large_tui.click(COPY_ICON_COLUMN, row)
            delta = bytes(large_tui.captured[before:])
            if b"Code block too large to copy" in re.sub(
                    rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\([A-Za-z0-9]", b"", delta):
                large_icon_row = row
                break
        check(large_icon_row is not None, "the over-cap block did not report its refusal")
        if large_icon_row is not None:
            delta = bytes(large_tui.captured[-4096:])
            plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\([A-Za-z0-9]", b"", delta)
            check(b"Code block too large to copy" in plain,
                  "the over-cap refusal was not visible as a notice")
            check(CHECK_GLYPH not in delta,
                  "an over-cap block pretended to have been copied")
        code_exit, _ = large_tui.finish()
        check(code_exit == 0, "the over-cap fixture did not shut down cleanly")

    # A link press resolves through the same action table and writes its target to the
    # real pasteboard. The preview never hands an arbitrary URI to a desktop opener.
    with tempfile.TemporaryDirectory(prefix="semicolon-link-") as directory:
        link_project = Path(directory)
        link_file = link_project / "link.md"
        link_file.write_text("# Linkmarker\n\n[site](https://example.test/path)\n")
        link_tui = Pty(22, 112,
                       argv=[str(BINARY), "--root", str(link_project), "--file",
                             str(link_file)],
                       env={"SEMICOLON_ICONS": "plain"})
        check(link_tui.wait_for(b"Linkmarker"), "the link fixture did not render")
        link_tui.repaint()
        # The heading is document row 0, the blank line row 1, so the label starts at the
        # content's first column a few rows down. The warning chip is present by default,
        # which is why the content begins at screen row 5; scan the small band instead of
        # baking in that offset.
        link_hit = False
        for row in range(4, 11):
            before = len(link_tui.captured)
            link_tui.click(RAIL_DIVIDER_COLUMN + 3, row)
            delta = bytes(link_tui.captured[before:])
            plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\([A-Za-z0-9]", b"", delta)
            if b"Link copied to clipboard" in plain:
                link_hit = True
                break
        check(link_hit, "pressing a link label did not report the link action")
        if shutil.which("pbpaste") is not None:
            matched, pasted = pasteboard_settles(b"https://example.test/path")
            check(matched,
                  f"the clipboard did not hold the link target: {pasted!r}")
        check(b"\x1b]52;" not in bytes(link_tui.captured),
              "a link press emitted an OSC 52 escape")
        link_exit, _ = link_tui.finish()
        check(link_exit == 0, "the link fixture did not shut down cleanly")

    # C implementation files form one alphabetical run before the header run.
    with tempfile.TemporaryDirectory(prefix="semicolon-c-order-") as directory:
        project = Path(directory)
        (project / "zfolder").mkdir()
        for name in ("z.c", "alpha.c", "source.C", "z.h", "alpha.h", "other.cpp", "notes.txt"):
            (project / name).write_text("int value;\n")
        order_lines = dump(120, 18, project, project / "alpha.c", "plain")[1:]
        positions = {}
        for name in ("alpha.c", "source.C", "z.c", "alpha.h", "z.h"):
            positions[name] = next((index for index, line in enumerate(order_lines)
                                    if name in line), None)
        ordered_positions = [positions[name] for name in ("alpha.c", "source.C", "z.c",
                                                           "alpha.h", "z.h")]
        check(all(position is not None for position in ordered_positions),
              f"the project rail omitted C source/header rows: {positions!r}")
        if all(position is not None for position in ordered_positions):
            check(ordered_positions == sorted(ordered_positions),
                  f"C files and headers were not grouped alphabetically: {positions!r}")
        folder_row = next((index for index, line in enumerate(order_lines) if "zfolder" in line), None)
        check(folder_row is not None and folder_row < positions["alpha.c"],
              "C grouping moved folders behind files")
    invalid_icons = subprocess.run([str(BINARY), "--icons", "invalid"],
                                   capture_output=True, text=True, timeout=60)
    check(invalid_icons.returncode != 0 and "icons must be nerd or plain" in invalid_icons.stderr,
          "invalid icon mode was not rejected loudly")

    # --- A non-tty launch is refused, not painted into a pipe ---
    refused = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
    check(refused.returncode != 0, "a non-tty launch succeeded")
    check("requires terminal" in refused.stderr, f"non-tty diagnostic missing: {refused.stderr!r}")

    # --- Under a pty: mouse reporting, then a wheel scroll through the real path ---
    # This interactive shell receives printable input later to prove that `q` is not
    # a quit key. Keep that editable file disposable: PROJECT_FILE is a real checkout
    # header and autosave must never rewrite it as a side effect of this owner test.
    project_file_before_shell = PROJECT_FILE.read_bytes()
    shell_file_directory = tempfile.TemporaryDirectory(prefix="semicolon-shell-file-")
    shell_file = Path(shell_file_directory.name) / PROJECT_FILE.name
    shell_file.write_bytes(project_file_before_shell)
    shell = Pty(24, 112, argv=[str(BINARY), "--root", str(PROJECT_ROOT),
                               "--file", str(shell_file)])
    # The escape for any-motion plus SGR encoding is what makes hover and drag
    # possible at all, so the shell must ask for it.
    check(shell.wait_for(b"\x1b[?1003h"), "mouse reporting was never enabled")
    check(shell.wait_for(b"\x1b[?1006h"), "SGR mouse encoding was never requested")
    check(shell.wait_for(b"[] semicolon"), "the shell never painted the project's rail")
    check("⌄ [] semicolon".encode() in shell.captured,
          "the UTF-8 PTY did not receive the Unicode expanded disclosure glyph")
    before_hover = len(shell.captured)
    shell.send(b"\x1b[<35;51;10M")
    shell.drain(0.2)
    hover_output = bytes(shell.captured[before_hover:])
    even_rows = Pty.grip_rows(hover_output)
    check(even_rows == [12, 13, 14],
          f"even-height hover grip rows were {even_rows!r}, expected three centered rows")
    even_start = GRIP_START_RE.search(hover_output)
    check(even_start is not None and int(even_start.group(2)) == RAIL_DIVIDER_COLUMN + 1,
          "even-height hover grip was not drawn on the divider")

    # Odd terminal heights keep the same invisible divider behavior.
    odd = Pty(23, 112)
    check(odd.wait_for(b"[] semicolon"), "odd-height shell never painted its rail")
    before_odd_hover = len(odd.captured)
    odd.send(b"\x1b[<35;51;10M")
    odd.drain(0.2)
    odd_output = bytes(odd.captured[before_odd_hover:])
    odd_rows = Pty.grip_rows(odd_output)
    check(odd_rows == [11, 12, 13],
          f"odd-height hover grip rows were {odd_rows!r}, expected three centered rows")
    odd_code, _ = odd.finish()
    check(odd_code == 0, f"odd-height pty run exited {odd_code}")

    # A PRESENT IS A TRANSACTION. The frame must be bracketed, and the CELLS must sit
    # INSIDE the bracket: a close that arrives before the content it wraps makes the
    # bracket decorative, which is exactly the failure that would go unnoticed.
    check(shell.wait_for(b"\x1b[?2026l"), "the frame transaction was never closed")
    painted = bytes(shell.captured)
    opened = painted.find(b"\x1b[?2026h")
    closed = painted.find(b"\x1b[?2026l")
    check(0 <= opened < closed, f"the frame transaction is not ordered (h={opened}, l={closed})")
    # A wheel-down at (60, 5): the pointer is in the code area, so the decoder turns
    # it into the event that scrolls the editor rather than the rail.
    shell.send(b"\x1b[<65;60;5M")
    shell.drain(0.3)
    # The presenter only sends cells that CHANGED, so a scroll shows up as fragments
    # rather than as whole rows. Resizing the pty forces the shell to rebind its
    # surface with a blank mirror, which is the one way to make it repaint the whole
    # screen and therefore the only way to read the result as text.
    before_scroll_repaint = len(shell.captured)
    fcntl.ioctl(shell.master, termios.TIOCSWINSZ, struct.pack("HHHH", 23, 112, 0, 0))
    shell.drain(0.5)
    scrolled = bytes(shell.captured[before_scroll_repaint:])
    # The line chip is the CURSOR, not the viewport, so a wheel must leave it alone.
    check(b"[ln:1:1]" in scrolled, "a wheel event moved the cursor-position chip")
    # The gutter and the code are DIFFERENT COLOURS, so ncurses emits an SGR
    # sequence between them and "4|#include" can never appear contiguously.
    # Assert on the gutter instead: one notch down, at 23 rows, leaves line 22 as
    # the last visible row, which is absent from an unscrolled frame.
    check(b"22." in scrolled, "the scrolled rows did not re-point")
    # HORIZONTAL SCROLL, checked so it cannot alias itself. "#include <stdint.h>"
    # CONTAINS "lude <stdint.h>" from character 4, so searching for the shifted text
    # proves nothing - an earlier version of this test passed while the row had not
    # moved at all. The discriminating check is that the FULL row is gone.
    def visible(payload):
        text = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", payload)
        return re.sub(rb"\x1b\([A-Za-z0-9]", b"", text)

    def horizontal_shift(payload, label):
        before = len(shell.captured)
        shell.send(payload)
        shell.drain(0.35)
        # The presenter only sends CHANGED cells, so force a full repaint before
        # reading the result as text.
        shell.force_repaint()
        # Per-token coloring inserts SGR runs inside the row, so compare visible
        # text rather than raw bytes; otherwise the row can never read contiguously.
        check(b"#include <stdint.h>" not in visible(shell.captured[before:]),
              f"{label} did not shift the row")

    # The arrow key is the point: nobody has a horizontal wheel, and Shift+wheel is
    # routinely eaten by the terminal's own scrollback.
    horizontal_shift(b"\x1b[<67;60;5M", "wheel-right")
    horizontal_shift(b"\x1b[<67;60;5M", "wheel-right (repeat)")
    # Shift is the modifier terminals most often steal for their own scrollback, so
    # Alt and Ctrl must pan too: whichever one the emulator forwards is the one that
    # reaches the columns.
    horizontal_shift(b"\x1b[<73;60;5M", "alt+wheel (65+8)")
    shell.send(b"\x1b[D")
    shell.drain(0.3)
    horizontal_shift(b"\x1b[<81;60;5M", "ctrl+wheel (65+16)")
    shell.send(b"\x1b[D")
    shell.drain(0.3)

    # BOTH DIRECTIONS on a MODIFIED wheel, which is the case a trackpad produces:
    # the emulator reports every swipe as a vertical wheel and distinguishes intent
    # only by the modifier bit, so wheel-down+meta must pan right and wheel-up+meta
    # must pan back left. Without this, a modified swipe could only ever travel one
    # way.
    def horizontal_pan_left(payload, label):
        before = len(shell.captured)
        shell.send(payload)
        shell.drain(0.35)
        shell.force_repaint()
        check(b"#include <stdint.h>" in visible(shell.captured[before:]),
              f"{label} did not pan back to column 0")

    shell.send(b"\x1b[<73;60;5M")              # option + wheel-down -> pan right
    shell.drain(0.3)
    horizontal_pan_left(b"\x1b[<72;60;5M", "option+wheel-up")

    # Reading the viewport from the interactive screen needs a full repaint: the
    # presenter only sends changed cells. These helpers ignore the live clock, whose
    # once-per-second tick is the only output a successful no-op click may produce.
    def screen_plain():
        # A forced repaint can race a clock update or a still-arriving previous frame, so
        # a partial capture is retried until it contains both the footer and a gutter row.
        for _ in range(3):
            raw = shell.repaint()
            plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", raw)
            plain = re.sub(rb"\x1b\([A-Za-z0-9]", b"", plain)
            # A clock update that lands during the forced repaint is emitted as bare digits
            # positioned after the footer; keep it from looking like a changed screen.
            plain = re.sub(rb"(\[utf-8\])\d+", rb"\1", plain)
            if b"[utf-8]" in plain and re.search(rb"(?<!\d)\d+\.", plain) is not None:
                return plain
        return plain

    def first_gutter(plain):
        found = re.findall(rb"(?<!\d)(\d+)\.", plain)
        return int(found[0]) if found else None

    def meaningful_delta(payload):
        """What a delta says after terminal mechanics and the live clock are removed.

        A clock tick is digits only; an inert pointer still lets it through. A scroll or
        caret move changes text, so the remaining letters are the signal.
        """
        plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", payload)
        plain = re.sub(rb"\x1b\([A-Za-z0-9]", b"", plain)
        plain = re.sub(rb"\[\d{2}:\d{2}:\d{2}\]", b"", plain)
        plain = re.sub(rb"[\x00-\x1f\x7f]+", b"", plain)
        plain = re.sub(rb"\d+", b"", plain)
        return re.sub(rb"[\s\[\]:;()]+", b"", plain)

    # THE TRACKING LINE is the bottom TWO rows: the last editor row and the frame's
    # bottom border. Both are the same slider, so the whole floor of the window is
    # draggable. A drag maps the pointer straight onto the offset; a CLICK does
    # nothing, because press and drag are separable gestures. At 22 rows (1-based):
    # last editor row 21, border row 22, code columns 59..111.
    code_left, code_right = 59, 111
    last_editor_row, border_row = 21, 22

    # A relative drag only covers the code area's own width, so reaching the far end of
    # a wider document takes more than one drag - exactly like a trackpad. Repeating is
    # therefore part of the contract, not a workaround.
    def tracking_pan_right(times, row):
        for _ in range(times):
            shell.send(b"\x1b[<0;%d;%dM" % (code_left, row))
            shell.drain(0.12)
            shell.send(b"\x1b[<32;%d;%dM" % (code_right, row))
            shell.drain(0.22)
            shell.send(b"\x1b[<0;%d;%dm" % (code_left, row))
            shell.drain(0.1)

    for row in (last_editor_row, border_row):
        before = len(shell.captured)
        tracking_pan_right(4, row)
        shell.force_repaint()
        check(b"#include <stdint.h>" not in shell.captured[before:],
              f"repeated drags on tracking row {row} did not pan the view right")

    # NO JUMP. The drag is RELATIVE: the content moves by the pointer's own delta, so the
    # point under your finger stays under your finger. The first version mapped the
    # pointer's ABSOLUTE position onto the offset, which flung the view to wherever the
    # press happened to land - grab near the left end while panned right and it snapped
    # home. This is the regression test for exactly that.
    before = len(shell.captured)
    shell.send(b"\x1b[<0;%d;%dM" % (code_left + 1, last_editor_row))
    shell.drain(0.15)
    shell.send(b"\x1b[<32;%d;%dM" % (code_left + 2, last_editor_row))
    shell.drain(0.3)
    shell.send(b"\x1b[<0;%d;%dm" % (code_left + 1, last_editor_row))
    shell.drain(0.2)
    shell.force_repaint()
    check(b"#include <stdint.h>" not in shell.captured[before:],
          "grabbing near the left end flung the view home; the drag must be relative")

    # A CLICK on either row must not pan - not "leave the offset alone", but leave the
    # visible screen exactly where it was. The live clock can tick during the click, so
    # clock digits and terminal mechanics are normalized out rather than requiring the
    # byte stream to be empty.
    for row in (last_editor_row, border_row):
        before = len(shell.captured)
        shell.send(b"\x1b[<0;60;%dM" % row)
        shell.drain(0.25)
        shell.send(b"\x1b[<0;60;%dm" % row)
        shell.drain(0.4)
        delta = meaningful_delta(bytes(shell.captured[before:]))
        check(delta == b"",
              f"a click on tracking row {row} changed the view; only a drag may pan "
              f"({delta[:80]!r})")

    # ...and the vertical keys work too.
    beforeDown = len(shell.captured)
    shell.send(b"\x1b[6~")                     # PageDown scrolls; arrows move the edit cursor
    shell.drain(0.6)
    fcntl.ioctl(shell.master, termios.TIOCSWINSZ, struct.pack("HHHH", 21, 112, 0, 0))
    shell.drain(0.6)
    check(b"[ln:" in shell.captured[beforeDown:], "PageDown did not scroll vertically")

    # HOVER IS INERT. Any-motion reporting sends an event for every cell the
    # pointer crosses, so acting on plain motion made the view scroll whenever the
    # pointer passed over the first or last editor row. A motion event with no
    # button must therefore leave the screen's text unchanged; the live clock is
    # normalized out because it ticks independently of the pointer.
    shell.drain(0.4)
    hover_before = len(shell.captured)
    shell.send(b"\x1b[<35;60;2M")             # motion, no button, first editor row
    shell.drain(0.4)
    hover_delta = meaningful_delta(bytes(shell.captured[hover_before:]))
    check(hover_delta == b"",
          f"hovering the edge row changed the screen; hover must be inert "
          f"({hover_delta[:80]!r})")

    # ...but a DRAG there does scroll, because edge autoscroll belongs to a drag.
    shell.send(b"\x1b[<0;60;2M")              # press
    shell.drain(0.15)
    before = len(shell.captured)
    shell.send(b"\x1b[<32;60;2M")             # drag on the first editor row
    shell.drain(0.4)
    check(len(shell.captured) > before, "dragging at the edge row did not scroll")
    shell.send(b"\x1b[<0;60;2m")              # release

    # THE DIVIDER DRAG REALLY RESIZES. The press lands ON the invisible seam (column 50),
    # the drag moves 10 cells right, and the rail and first tab follow it together.
    shell.send(b"\x1b[<0;51;10M")
    shell.drain(0.15)
    shell.send(b"\x1b[<32;61;10M")
    shell.drain(0.15)
    shell.send(b"\x1b[<0;61;10m")
    shell.drain(0.3)
    before_repaint = len(shell.captured)
    shell.force_repaint()
    repaint = bytes(shell.captured[before_repaint:])
    header = Pty.row_text(repaint)
    check(header is not None, "the repainted header did not emit its first row")
    if header is not None:
        # The first tab's chip opens at the seam. Column 61 is index 60.
        check(len(header) > 61 and header[60] == "[",
              f"the tab strip did not begin at column 61 beside the moved seam "
              f"({header[50:80]!r})")

    # 'q' IS NOT A QUIT KEY. A letter is input - 'q' is a legitimate thing to want to
    # type - so the only exit is Ctrl-C, which raw mode delivers as byte 3.
    shell.send(b"q")
    shell.drain(0.4)
    check(shell.process.poll() is None, "pressing q quit the shell; Ctrl-C is the only exit")

    # SENSITIVITY AND CADENCE, measured last so scrolling the view cannot disturb any
    # assertion above. One notch is ONE line, not three: a trackpad quantises a gesture
    # into dozens of notches, so three each was a screen and a half per flick. And a
    # burst ANIMATES - the loop used to present only when input arrived, so the whole
    # burst landed in one frame, which is why no cadence was felt.
    #
    # The frame count comes from the transaction brackets: every frame opens exactly one,
    # so the bracket doubles as a frame counter.
    # After the divider drag the rail is 60 cells wide, so x=70 is inside the code
    # area; x=60 now lands on the rail and would scroll the file list instead.
    first_line = first_gutter(screen_plain())
    check(first_line is not None, "the repaint did not expose the editor's first gutter row")
    burst = b"".join(b"\x1b[<65;70;5M" for _ in range(12))
    beforeBurst = len(shell.captured)
    shell.send(burst)
    shell.drain(1.4)
    frames = shell.captured[beforeBurst:].count(b"\x1b[?2026h")
    check(frames >= 6, f"a 12-notch burst produced {frames} frames; it must animate, not jump")
    last_line = first_gutter(screen_plain())
    check(last_line is not None and last_line - first_line == 12,
          f"twelve notches moved {None if last_line is None else last_line - first_line} lines, not twelve")

    # MOMENTUM. A burst must DECELERATE and settle on its own, and a PRESS must catch it.
    # A fixed-rate queue could do neither: it ran at constant speed until it emptied,
    # which is the conveyor belt this replaced - no friction, and nothing to stop.
    shell.send(b"".join(b"\x1b[<65;70;5M" for _ in range(12)))
    shell.drain(1.6)                       # arrive and settle
    settled_line = first_gutter(screen_plain())
    shell.drain(0.6)
    check(first_gutter(screen_plain()) == settled_line,
          "a burst never settled; the glide must come to rest by itself")

    beforeCatchLine = first_gutter(screen_plain())
    shell.send(b"".join(b"\x1b[<65;70;5M" for _ in range(12)))
    shell.drain(0.12)                      # catch it mid-flight
    shell.send(b"\x1b[<0;70;10M")         # press
    shell.drain(0.25)
    afterPressLine = first_gutter(screen_plain())
    shell.drain(0.7)
    check(first_gutter(screen_plain()) == afterPressLine,
          "a press did not catch the scroll; momentum must be stoppable by touching")
    check(afterPressLine != beforeCatchLine,
          "the burst produced no movement before the press")
    shell.send(b"\x1b[<0;70;10m")         # release
    shell.drain(0.2)

    code, captured = shell.finish()
    check(shell_file.read_bytes() != project_file_before_shell,
          "the disposable PTY file did not retain the typed q")
    shell_file_directory.cleanup()
    check(code == 0, f"the pty run exited {code}")
    check(PROJECT_FILE.read_bytes() == project_file_before_shell,
          "interactive PTY input changed the checkout's real source file")
    text = captured.decode("utf-8", "replace")
    check("⌄ [] semicolon" in text,
          "the pty run never painted the project's rail")
    check("\x1b[?1003l" in text, "mouse reporting was not switched off on exit")
    check("\x1b[?1049l" in text, "the screen was never restored")

    # --- FADED COLOURS, first half: the backdrop the host RESOLVES ---
    # Every fade is composited against the terminal's own background, so resolving it
    # correctly is what makes a faded colour land where it was meant to.
    for label, reply, expected in (
        ("a 16-bit rgb: reply", b"\x1b]11;rgb:1e1e/1e1e/2e2e\x07", "#1E1E2E"),
        ("a short 8-bit rgb: reply", b"\x1b]11;rgb:11/22/33\x07", "#112233"),
        ("an ST-terminated reply", b"\x1b]11;rgb:aabb/ccdd/eeff\x1b\\", "#AACCEE"),
        ("a reply preceded by junk", b"junk\x1b]11;rgb:0102/0304/0506\x07", "#010305"),
    ):
        background, _ = probe_background(reply=reply)
        check(background == expected,
              f"{label} was not parsed: got {background}, expected {expected}")
    # A terminal that ignores the query is a perfectly normal terminal. The shell must
    # not wait on it - the fallback stands and it says so.
    background, source = probe_background()
    check(background == "#1A1A1A", f"an unanswered query did not fall back: {background}")
    check("did not answer" in (source or ""), f"the fallback was not reported: {source}")
    # A stated value beats a guess, even when the terminal does answer.
    background, source = probe_background(reply=b"\x1b]11;rgb:ffff/ffff/ffff\x07",
                                          extra_env={"SEMICOLON_BG": "#112233"})
    check(background == "#112233", f"SEMICOLON_BG did not win: {background}")
    check(source == "SEMICOLON_BG", f"the source was not named: {source}")
    # A malformed declaration is refused loudly rather than parsed into a colour nobody
    # asked for: a silently wrong backdrop would fade the whole shell the wrong way.
    background, _ = probe_background(extra_env={"SEMICOLON_BG": "nope"})
    check(background == "#1A1A1A", f"a malformed SEMICOLON_BG was not refused: {background}")

    # --- FADED COLOURS, second half: the composite the host EMITS ---
    # The open tab carries the shell's one highlight. It must reach the device as the
    # accent COMPOSITED OVER THE BACKDROP, so the same pen emits a different palette index
    # on a dark theme than on a light one. That difference is the whole proof: ignoring
    # alpha would emit one fixed index in both runs, which is exactly the defect that made
    # a translucent colour render opaque.
    dark_expected = palette_index(blend(COL_SELECT, 0x000000FF))
    light_expected = palette_index(blend(COL_SELECT, 0xFFFFFFFF))
    unblended = palette_index(blend(COL_SELECT_OPAQUE, 0x000000FF))
    check(dark_expected != light_expected,
          "the composite gave one index for both themes, so nothing was composited")
    check(unblended not in (dark_expected, light_expected),
          "an un-composited accent is indistinguishable here, so this proves nothing")
    for backdrop, expected in (("#000000", dark_expected), ("#FFFFFF", light_expected)):
        emitted = painted_backgrounds(backdrop)
        check(expected in emitted,
              f"over {backdrop} the highlight did not reach the device as the accent at 22% "
              f"(wanted index {expected}, emitted {sorted(emitted)})")
        check(unblended not in emitted,
              f"over {backdrop} the highlight emitted the un-composited colour")

    # Number alignment follows the largest line number, not a hard-coded gutter. A
    # five-digit file reserves a five-cell number field: two fixed cells, four alignment
    # spaces, `1.`, then the two-cell code pad.
    many_lines = Path(STATE_DIR) / "many-lines.txt"
    many_lines.write_text("x\n" * 12345)
    numbered = dump(112, 8, root=PROJECT_ROOT, semicolon_file=many_lines)
    check(numbered[1][50:56] == "      " and numbered[1][56:61] == "1.  x",
          f"line numbers did not align to the five-digit maximum: {numbered[1]!r}")

    # --- The file process: browse, fold, open, and come back to it ---
    # The rail IS the project: rooted where the shell was started (or where --root says),
    # read one directory at a time, folded by clicking, and opened by clicking a file.
    project = scratch_project(state.name)

    scratch = dump(100, 8, root=project, semicolon_file=project / "top.txt")
    dumps = "".join(line[:RAIL_DIVIDER_COLUMN] for line in scratch[1:7])
    check(" ⌄ [] proj" in scratch[0], f"the rail is not the project root: {scratch[0]!r}")
    check("› [] alpha" in dumps and "› [] beta" in dumps,
          "the rail's directories are not collapsed, so a project dumps itself on screen")
    check("file00.txt" in dumps, "the rail does not show the project's files")
    check(".hidden" not in dumps, "the rail shows dotfiles")

    tree = Pty(26, 112, argv=[str(BINARY), "--root", str(project),
                              "--file", str(project / "top.txt")])
    first_frame = tree.repaint(26)
    check("⌄ [] proj".encode() in first_frame,
          "the interactive rail is not the project")

    tree.click(6, 1)                       # the `alpha` directory
    fresh = tree.repaint(26)
    check("⌄ [] alpha".encode() in fresh and b"one.txt" in fresh,
          "clicking a directory did not open it")
    tree.click(6, 2)                       # `one.txt`, now directly beneath alpha
    fresh = tree.repaint(26)
    check(b"proj > alpha > one.txt" in fresh, "clicking a file did not open it")
    check("[- one.txt  \uf00d]".encode() in fresh,
          "the tab does not name the file that was clicked")
    tree.click(6, 1)                       # fold alpha again
    fresh = tree.repaint(26)
    check("› [] alpha".encode() in fresh,
          "clicking the directory again did not fold it")

    # A wheel over the RAIL scrolls the rail, not the editor: the pointer says which list
    # is meant, so no mode or modifier is needed.
    before = tree.repaint(26)
    check("⌄ [] proj".encode() in before,
          "the rail is not at its top")
    tree.wheel(6, 10)
    after = tree.repaint(26)
    check("⌄ [] proj".encode() in after and "› [] alpha".encode() not in after,
          "rail scrolling lost the pinned root or did not move the children")
    check(b"[ln:1:1]" in after, "a wheel over the rail scrolled the editor")

    # THE SESSION. It is written on the way out, in a directory of its own, and the next
    # run in this project comes back to the same file - which is what makes
    # `cd <project> && semicolon` resume rather than restart.
    code, _ = tree.finish()
    check(code == 0, f"the pty run exited {code}")
    # One session file PER PROJECT, so find this project's rather than the first one: the
    # earlier interactive run wrote one of its own, and that is the point of the naming.
    sessions = [path.read_text() for path in Path(STATE_DIR).glob("*.state")]
    mine = [text for text in sessions if f"root {project}" in text]
    check(len(mine) == 1, f"this project has no session (sessions: {sessions!r})")
    if mine:
        check(f"open {project}/alpha/one.txt" in mine[0],
              f"the session did not record the open file: {mine[0]!r}")

    again = Pty(26, 112, argv=[str(BINARY), "--root", str(project)])
    check(b"proj > alpha > one.txt" in again.repaint(26),
          "the next run did not restore the file the session recorded")
    again.finish()

    # --- The editor: typed bytes stay in memory until one idle second, then save atomically ---
    with tempfile.TemporaryDirectory(prefix="semicolon-edit-") as directory:
        edit_root = Path(directory) / "edit"
        edit_root.mkdir()
        edit_file = edit_root / "edit.txt"
        original = b"one\r\nline2\n"
        edit_file.write_bytes(original)
        editing = Pty(22, 112, argv=[str(BINARY), "--root", str(edit_root),
                                    "--file", str(edit_file)])
        check(editing.wait_for(b"one"), "editable file did not render")
        editing.send(b"AB")
        editing.drain(0.6, cap=0.6)
        check(edit_file.read_bytes() == original,
              "autosave ran before one second of idle time")
        editing.send(b"C")                 # restart the idle deadline
        editing.drain(0.55, cap=0.55)
        check(edit_file.read_bytes() == original,
              "a later keystroke did not restart the autosave deadline")
        before_save = len(editing.captured)
        editing.drain(0.7, cap=0.7)
        check(edit_file.read_bytes() == b"ABCone\r\nline2\n",
              f"typed text or mixed line endings were not saved exactly: {edit_file.read_bytes()!r}")
        check(f"[{edit_root.name}]".encode() in editing.captured[before_save:],
              "successful autosave did not return the status chip to its clean state")
        editing.send(b"\x7f")             # backspace removes the final typed ASCII scalar
        editing.drain(1.2)
        check(edit_file.read_bytes() == b"ABone\r\nline2\n",
              "cursor movement or deletion changed the wrong byte")
        editing.send("界".encode("utf-8"))
        editing.drain(1.2)
        check(edit_file.read_bytes() == "AB界one\r\nline2\n".encode("utf-8"),
              "a multibyte UTF-8 character did not survive editing and save")
        # Ctrl-Z has to reach the editor as byte 26: under cbreak the line discipline
        # would turn it into SIGTSTP and suspend the whole shell before ncurses saw it.
        # Each state is autosaved, so the on-disk bytes are the oracle for the snapshot.
        editing.send(b"\x1a")
        editing.drain(1.2)
        check(edit_file.read_bytes() == b"ABone\r\nline2\n",
              "Ctrl-Z did not restore the pre-edit document exactly")
        editing.send(b"\x19")
        editing.drain(1.2)
        check(edit_file.read_bytes() == "AB界one\r\nline2\n".encode("utf-8"),
              "Ctrl-Y did not reapply the undone edit exactly")
        editing.send(b"\x1a")
        editing.drain(1.2)
        editing.send(b"Z")                 # a fresh edit must discard the redo path
        editing.drain(1.2)
        check(edit_file.read_bytes() == b"ABZone\r\nline2\n",
              "editing after undo did not commit the new state")
        editing.send(b"\x19")              # nothing to redo; the document stays put
        editing.drain(0.5)
        check(edit_file.read_bytes() == b"ABZone\r\nline2\n",
              "Ctrl-Y resurrected a redo that a new edit should have discarded")
        edit_exit, _ = editing.finish()
        check(edit_exit == 0, f"editing PTY exited {edit_exit}")

    # Exact plain-source click admission: screen row five is source row five,
    # not the preceding row. Persisted bytes are the oracle, not cursor escape text.
    with tempfile.TemporaryDirectory(prefix="semicolon-caret-click-") as directory:
        root = Path(directory)
        source = root / "click.txt"
        lines = [f"row{i}_abcdefgh\n" for i in range(1, 9)]
        source.write_text("".join(lines))
        shell = Pty(22, 112, argv=[str(BINARY), "--root", str(root), "--file", str(source)])
        try:
            check(shell.wait_for(b"row5_abcdefgh"), "click fixture did not paint")
            shell.click(60, 5)
            shell.send(b"!")
            shell.drain(1.3, cap=1.3)
            lines[4] = "row5!_abcdefgh\n"
            check(source.read_text() == "".join(lines),
                  f"plain click inserted at the wrong row/cell: {source.read_text()!r}")
        finally:
            check(shell.finish()[0] == 0, "click fixture did not shut down")

    # Each wheel axis targets the pointed pane. Long tabs remain reachable and
    # long tree names reveal their tail without moving the pinned root.
    with tempfile.TemporaryDirectory(prefix="semicolon-pane-scroll-") as directory:
        root = Path(directory) / "proj"
        root.mkdir()
        names = ["a_first_long_filename.txt", "b_second_long_filename.txt",
                 "c_third_long_filename.txt", "d_" + "long_" * 12 + "TAIL.txt"]
        for name in names:
            (root / name).write_text("pane source marker\n")
        shell = Pty(22, 92, argv=[str(BINARY), "--root", str(root), "--file", str(root / names[0])],
                    env={"SEMICOLON_ICONS": "plain"})
        try:
            check(shell.wait_for(b"pane source marker"), "pane fixture did not paint")
            shell.click(12, 2)
            shell.click(12, 3)
            for _ in range(4):
                shell.send(b"\x1b[<66;65;1M")  # left over tabs
            shell.drain(0.2)
            check(b"a_first_long_filename" in shell.repaint(), "left wheel did not reveal first tab")
            for _ in range(2):
                shell.send(b"\x1b[<65;65;1M")  # down over tabs pans right
            shell.drain(0.2)
            check(b"c_third_long_filename" in shell.repaint(), "down wheel did not reveal hidden tab")
            shell.send(b"\x1b[<64;65;1M")
            shell.drain(0.2)
            check(b"b_second_long_filename" in shell.repaint(), "up wheel did not pan tabs left")
            shell.send(b"\x1b[<67;65;1M")
            shell.drain(0.2)
            check(b"c_third_long_filename" in shell.repaint(), "right wheel did not pan tabs right")
            for _ in range(40):
                shell.send(b"\x1b[<67;15;5M")  # right over tree
            shell.drain(0.2)
            panned = shell.repaint()
            check(b"TAIL.txt" in panned and "⌄ [] proj".encode() in panned,
                  "horizontal tree scroll did not reveal the filename tail")
            for _ in range(40):
                shell.send(b"\x1b[<68;15;5M")  # modified up pans left
            shell.drain(0.2)
            check(b"a_first_long_filename" in shell.repaint(), "modified wheel did not restore tree origin")
        finally:
            check(shell.finish()[0] == 0, "pane fixture did not shut down")

    # A changed-on-disk source is not overwritten: the buffer stays open and the
    # status becomes an explicit error rather than claiming the save succeeded.
    with tempfile.TemporaryDirectory(prefix="semicolon-save-conflict-") as directory:
        conflict_root = Path(directory)
        conflict_file = conflict_root / "conflict.txt"
        conflict_file.write_text("original\n")
        conflict = Pty(22, 112, argv=[str(BINARY), "--root", str(conflict_root),
                                     "--file", str(conflict_file)])
        check(conflict.wait_for(b"original"), "conflict fixture did not open")
        conflict.send(b"local")
        conflict_file.write_text("external change\n")
        conflict.drain(1.3, cap=1.3)
        check(conflict_file.read_text() == "external change\n",
              "autosave overwrote a file changed by another writer")
        check(b"[save]" in conflict.captured,
              "a rejected autosave was not visible in the status chip")
        conflict.process.kill()
        conflict.process.wait(timeout=5)
        os.close(conflict.master)

    state.cleanup()
    if failures:
        print(f"FAIL {failures} check(s)", file=sys.stderr)
        return 1
    print("PASS semicolon shell: frame shape, non-tty refusal, pty mouse reporting, wheel "
          "scroll, divider drag, faded colours, Markdown copy/link through the real "
          "clipboard, bounded "
          "two-ended breadcrumb")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
