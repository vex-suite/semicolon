#!/usr/bin/env python3
"""PTY regression for full-row tree hits and the collapsed rail affordance."""
import importlib.util
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_MODULE = ROOT / "semicolon/test/tui_main_test.py"
SPEC = importlib.util.spec_from_file_location("tui_main_helpers", TEST_MODULE)
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)


def main():
    HELPERS.ensure_built()
    with tempfile.TemporaryDirectory(prefix="semicolon-rail-interaction-") as directory:
        root = Path(directory) / "proj"
        alpha = root / "alpha"
        alpha.mkdir(parents=True)
        child = alpha / "one.txt"
        child.write_text("alpha child\n")
        (root / "top.txt").write_text("top level\n")
        with tempfile.TemporaryDirectory(prefix="semicolon-rail-state-") as state:
            HELPERS.STATE_DIR = state
            shell = HELPERS.Pty(
                24, 112,
                argv=[str(HELPERS.BINARY), "--root", str(root), "--file",
                      str(root / "top.txt")],
                env={"SEMICOLON_STATE": state, "TERM": "xterm-256color",
                     "SEMICOLON_ICONS": "plain"},
            )
            try:
                if not shell.wait_for("⌄ [] proj".encode(), 3.0):
                    raise AssertionError(
                        f"the initial tree did not render: {bytes(shell.captured[-1500:])!r}")

                before_list_hover = len(shell.captured)
                shell.send(b"\x1b[<35;50;10M")
                shell.drain(0.2)
                if HELPERS.GRIP_START_RE.search(bytes(shell.captured[before_list_hover:])):
                    raise AssertionError("divider grip still overlaps the last list cell")
                # The final rail cell before the divider is blank after this short name.
                # It still belongs to the folder row and must open it.
                shell.click(49, 1)
                opened = shell.repaint(24)
                if "⌄ [] alpha".encode() not in opened or b"one.txt" not in opened:
                    raise AssertionError("clicking the folder row's blank cells did not expand it")

                # The same blank-cell hit on a child file must open that file.
                shell.click(49, 2)
                opened = shell.repaint(24)
                if b"proj > alpha > one.txt" not in opened:
                    raise AssertionError("clicking the file row's blank cells did not open it")

                # Collapse by dragging the divider to column 2 and releasing there.
                shell.send(b"\x1b[<0;51;10M")
                shell.drain(0.15)
                shell.send(b"\x1b[<32;2;10M")
                shell.drain(0.15)
                shell.send(b"\x1b[<0;2;10m")
                shell.drain(0.25)
                collapsed = shell.repaint(24)
                if "📁".encode() in collapsed:
                    raise AssertionError("the collapsed rail still showed a folder icon")
                grip_rows = HELPERS.Pty.grip_rows(collapsed)
                if len(grip_rows) < 3:
                    raise AssertionError(
                        f"collapsed left-edge grip did not paint three bars: {grip_rows!r}: "
                        f"{collapsed[-1200:]!r}")

                shell.click(0, 12)
                reopened = shell.repaint(24)
                if "📁".encode() in reopened or "⌄ [] proj".encode() not in reopened:
                    raise AssertionError("the left-edge grip did not reopen the file tree")

                # The default reopen width is 50 cells rather than the former 30.
                before_width_probe = len(shell.captured)
                shell.send(b"\x1b[<35;51;10M")
                shell.drain(0.2)
                width_probe = bytes(shell.captured[before_width_probe:])
                grip = HELPERS.GRIP_START_RE.search(width_probe)
                if grip is None or grip.group(2) is None or int(grip.group(2)) != 51:
                    raise AssertionError(
                        f"the reopened rail did not use the 50-cell default: "
                        f"{width_probe[-700:]!r}")

                # A deliberate resize is remembered across drag-to-collapse/reopen.
                shell.send(b"\x1b[<0;51;10M")
                shell.drain(0.15)
                shell.send(b"\x1b[<32;47;10M")
                shell.drain(0.15)
                shell.send(b"\x1b[<0;47;10M")
                shell.drain(0.15)
                shell.send(b"\x1b[<32;2;10M")
                shell.drain(0.15)
                shell.send(b"\x1b[<0;2;10m")
                shell.drain(0.2)
                shell.click(0, 12)
                shell.drain(0.2)
                before_custom_probe = len(shell.captured)
                shell.send(b"\x1b[<35;47;10M")
                shell.drain(0.2)
                custom_probe = bytes(shell.captured[before_custom_probe:])
                grip = HELPERS.GRIP_START_RE.search(custom_probe)
                if grip is None or grip.group(2) is None or int(grip.group(2)) != 47:
                    raise AssertionError("the rail forgot its user-resized width after collapsing")
            finally:
                status, _ = shell.finish()
            if status != 0:
                raise AssertionError(f"Semicolon exited with status {status}")
    print("PASS full-row folder/file clicks and near-edge collapse/reopen affordance")


if __name__ == "__main__":
    main()
