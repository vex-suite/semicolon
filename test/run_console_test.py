#!/usr/bin/env python3
"""Owner: Semicolon runnable-file admission and the b-backed console tab.

A fake b records argv for workspace-vs-standalone routing without running a
windowed battle test. The shell-script fixture separately exercises real b and
captured console output; no GUI launch or visual acceptance is claimed.
"""
import tempfile
import re
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "suite/projects/semicolon"


def without_styles(payload):
    """Token ink/weight may split a source run; retain all non-style bytes."""
    return re.sub(rb"\x1b\[[0-9;]*m|\x1b\(B", b"", payload)


def main():
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "semicolon_tui_helpers", ROOT / "semicolon/test/tui_main_test.py")
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)

    build = subprocess.run(["python3", str(APP / "build.py")], capture_output=True,
                           text=True, timeout=180)
    assert build.returncode == 0, build.stderr

    with tempfile.TemporaryDirectory(prefix="semicolon-run-") as directory, \
            tempfile.TemporaryDirectory(prefix="semicolon-run-state-") as state:
        root = Path(directory)
        (root / "no-entry.c").write_text("int helper(void) { return 7; }\n")
        standalone = root / "standalone.c"
        runnable = root / "run.sh"
        runnable.write_text("#!/bin/sh\nprintf 'SEMICOLON_RUN_OUTPUT\\n'\n")
        runnable.chmod(0o700)
        helpers.STATE_DIR = state
        shell = helpers.Pty(
            20, 100,
            argv=[str(APP / ".build/semicolon-tui"), "--root", str(root),
                  "--file", str(root / "no-entry.c"), "--icons", "plain"])
        try:
            assert shell.wait_for(b"no-entry.c"), \
                f"editor did not open the non-entry C file: {shell.process.poll()} {bytes(shell.captured)!r}"
            initial = bytes(shell.captured)
            assert b"[> run]" not in initial and b"[\xe2\x96\xb6 run]" not in initial, \
                "Run was shown for a C file without main"

            shell.click(83, 1)
            assert b"SEMICOLON_RUN_OUTPUT" not in shell.captured, \
                "non-runnable C file unexpectedly started"

            # Select the runnable shell file in the tree, then activate its right-side Run chip.
            shell.click(10, 2)
            assert shell.wait_for(b"run.sh"), "runnable script did not open"
            shell.click(83, 1)
            assert shell.wait_for(b"SEMICOLON_RUN_OUTPUT", seconds=5), \
                "b output did not reach the console tab"
            assert shell.wait_for(b"exit 0", seconds=5), \
                "successful b child has no observed exit status"
            assert b"running" in shell.captured or b"finished" in shell.captured, \
                "running file did not get a selectable console tab"
            assert b"????????" not in shell.captured, "console tab label lost its borrowed text"
            before_close = len(shell.captured)
            # Pan to the console before hitting its actual close glyph. Selected
            # tabs are revealed whole rather than squeezed into the old tail slot.
            for _ in range(4):
                shell.send(b"\x1b[<67;65;1M")
            shell.drain(0.2)
            before_close = len(shell.captured)
            shell.click(50 + 1 + len("finished (exit 0) run.sh  "), 0)
            close_frame = bytes(shell.captured[before_close:])
            assert b"finished" not in close_frame, \
                f"closed console tab remained visible: {close_frame!r}"
            assert shell.finish()[0] == 0, "Semicolon PTY did not exit cleanly"
        finally:
            if shell.process.poll() is None:
                shell.finish()

        standalone.write_text("int main(void) { return 0; }\n")
        # This registered Darling test needs the workspace graph's include paths
        # and libraries. Never execute its window: an injected b records argv.
        # The outside fixture must keep the single-source b route.
        workspace_source = ROOT / "darling/test/frame/ui_revalidate_cascade_battle_test.c"
        before_source = workspace_source.read_bytes()
        driver = root / "record-b.sh"
        driver.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$SEMICOLON_ROUTE_LOG\"\n"
                          "printf 'SEMICOLON_ROUTE_OK\\n'\n")
        driver.chmod(0o700)
        for source, expected in (
            (workspace_source, ["run", str(workspace_source)]),
            (standalone, ["run", "exec", str(standalone)]),
        ):
            log = root / "route-args.txt"
            log.unlink(missing_ok=True)
            routed = helpers.Pty(
                20, 100,
                argv=[str(APP / ".build/semicolon-tui"), "--root", str(root),
                      "--file", str(source), "--icons", "plain"],
                env={"SEMICOLON_B_PATH": str(driver), "SEMICOLON_ROUTE_LOG": str(log)})
            try:
                assert routed.wait_for(source.name.encode()), \
                    f"source did not open: {source.name}"
                assert b"[> run]" in routed.captured or b"[\xe2\x96\xb6 run]" in routed.captured, \
                    f"main-bearing source has no Run chip at initial scene build: {source.name}"
                routed.click(83, 1)
                assert routed.wait_for(b"SEMICOLON_ROUTE_OK"), \
                    f"routed child produced no console output: {source.name}; " \
                    f"log={log.read_text() if log.exists() else 'absent'} " \
                    f"capture={bytes(routed.captured)[-800:]!r}"
                assert routed.wait_for(b"exit 0", seconds=5), \
                    f"successful fake b reported no exit code: {source.name}"
                assert log.is_file() and log.read_text().splitlines() == expected, \
                    f"wrong b route for {source.name}: {log.read_text() if log.exists() else 'absent'}"
                assert routed.finish()[0] == 0, "routed PTY did not exit cleanly"
            finally:
                if routed.process.poll() is None:
                    routed.finish()
        assert workspace_source.read_bytes() == before_source, \
            "routing a registered workspace test modified its source"

        # Feed more than the retained 10,000 lines from a fake b, never a window.
        # The pane must have its own cells (no source gutter), continue showing a
        # finished child, and let the wheel navigate older output after EOF.
        stream = root / "stream-b.py"
        stream.write_text("#!/usr/bin/env python3\nimport sys\n"
                          "sys.stdout.write(''.join((('X' * 4096 if i == 5000 "
                          "else f'console-line-{i:05d}') + '\\n') "
                          "for i in range(10050)))\n")
        stream.chmod(0o700)
        scrollback = helpers.Pty(
            20, 100,
            argv=[str(APP / ".build/semicolon-tui"), "--root", str(root),
                  "--file", str(standalone), "--icons", "plain"],
            env={"SEMICOLON_B_PATH": str(stream)})
        try:
            assert scrollback.wait_for(b"standalone.c"), "scrollback fixture did not open"
            assert b"[> run]" in scrollback.captured or b"[\xe2\x96\xb6 run]" in scrollback.captured, \
                "Run chip absent for scrollback fixture"
            scrollback.click(83, 1)
            assert scrollback.wait_for(b"10049", seconds=10), \
                f"console did not drain the finished child: {bytes(scrollback.captured)[-1200:]!r}"
            assert scrollback.wait_for(b"exit 0", seconds=5), \
                "large-output child did not finish"
            settled = scrollback.repaint()
            assert b"omitted 51 clipped 1" in settled, \
                "bounded ring/line caps did not report their evictions"
            assert b"exit 0" in settled and b"console-line-10049" in settled, \
                "completed output did not stay on the console panel"
            assert b"int main(void)" not in without_styles(settled) and b"  1.  " not in without_styles(settled), \
                "editor source or gutter leaked into the console panel"
            scrollback.wheel(80, 7, button=64)
            scrollback.wheel(80, 7, button=64)
            older = scrollback.repaint()
            assert b"console-line-10049" not in older and b"console-line-100" in older, \
                "finished console wheel did not move up through retained lines"
            scrollback.wheel(80, 7, button=65)
            scrollback.wheel(80, 7, button=65)
            newer = scrollback.repaint()
            assert b"console-line-10049" in newer, \
                "wheel down did not return to the finished output tail"
            scrollback.send(b"\x1b[H")
            oldest = scrollback.repaint()
            assert b"console-line-00051" in oldest, \
                "Home did not reach the first retained line after ring exhaustion"
            scrollback.send(b"\x1b[F")
            assert b"console-line-10049" in scrollback.repaint(), \
                "End did not restore follow-tail after seeking to oldest output"
            scrollback.click(60, 0)
            editor = scrollback.repaint()
            assert b"int main(void)" in without_styles(editor) and b"omitted 51" not in without_styles(editor), \
                "switching back to the file did not restore its independent panel"
            scrollback.click(80, 0)
            assert b"console-line-10049" in scrollback.repaint(), \
                "finished console snapshot was lost after switching tabs"
            assert scrollback.finish()[0] == 0, "scrollback PTY did not exit cleanly"
        finally:
            if scrollback.process.poll() is None:
                scrollback.finish()
        assert standalone.read_text() == "int main(void) { return 0; }\n", \
            "console input contaminated the source buffer"

        for name, body, outcome in (
            ("failure", "raise SystemExit(37)\n", b"exit 37"),
            ("signaled", "import os, signal\nos.kill(os.getpid(), signal.SIGTERM)\n",
             b"signal 15"),
        ):
            fake = root / f"{name}-b.py"
            fake.write_text("#!/usr/bin/env python3\nprint('CHILD_DIAGNOSTIC', flush=True)\n" + body)
            fake.chmod(0o700)
            session = helpers.Pty(
                20, 100,
                argv=[str(APP / ".build/semicolon-tui"), "--root", str(root),
                      "--file", str(standalone), "--icons", "plain"],
                env={"SEMICOLON_B_PATH": str(fake)})
            try:
                assert session.wait_for(b"standalone.c"), "failure fixture did not open"
                session.click(83, 1)
                assert session.wait_for(b"CHILD_DIAGNOSTIC", seconds=5), \
                    f"{name} child output missing"
                assert session.wait_for(outcome, seconds=5), \
                    f"{name} child misreported completion: {bytes(session.captured)[-500:]!r}"
                assert outcome in session.repaint(), \
                    f"{name} status not retained in finished console"
                assert session.finish()[0] == 0, f"{name} PTY did not exit cleanly"
            finally:
                if session.process.poll() is None:
                    session.finish()

    print("Semicolon runnable-file console owner passed")


if __name__ == "__main__":
    main()
