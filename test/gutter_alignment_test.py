#!/usr/bin/env python3
"""The code gutter right-aligns line numbers to the largest line in the file."""
from pathlib import Path
import importlib.util
import subprocess
import re
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "suite/projects/semicolon"
BINARY = APP / ".build/semicolon-tui"
HELPER_SPEC = importlib.util.spec_from_file_location(
    "semicolon_tui_helpers", ROOT / "semicolon/test/tui_main_test.py")
HELPERS = importlib.util.module_from_spec(HELPER_SPEC)
HELPER_SPEC.loader.exec_module(HELPERS)


def main():
    subprocess.run(["python3", str(APP / "build.py")], check=True, timeout=600)
    with tempfile.TemporaryDirectory(prefix="semicolon-gutter-") as directory:
        root = Path(directory)
        source = root / "lines.c"
        source.write_text("".join(f"int line_{number};\n" for number in range(1, 1001)))
        result = subprocess.run(
            [str(BINARY), "--dump", "80x12", "--root", str(root), "--file", str(source),
             "--icons", "plain"],
            check=True, capture_output=True, text=True, timeout=60,
        )
        lines = result.stdout.splitlines()
        first = next(line for line in lines if "int line_1;" in line)
        second = next(line for line in lines if "int line_2;" in line)
        first_code = first.index("int line_1;")
        second_code = second.index("int line_2;")
        assert first_code == second_code, "code text does not share a stable left edge"
        first_field = first[first_code - 7:first_code - 3]
        second_field = second[second_code - 7:second_code - 3]
        assert first_field == "   1" and second_field == "   2", \
            f"line numbers are not right-aligned in the four-digit field: {first_field!r}, {second_field!r}"
        assert first[first_code - 3] == second[second_code - 3] == ".", \
            "the period is not immediately after the number field"

    # Open at 999 rows, then create row 1000 in the live editor. The gutter must
    # widen with the document or its fixed panel clips the new row's trailing period.
    with tempfile.TemporaryDirectory(prefix="semicolon-gutter-boundary-") as directory:
        root = Path(directory)
        source = root / "lines.c"
        source.write_text("\n".join(f"int line_{number};" for number in range(1, 1000)))
        with tempfile.TemporaryDirectory(prefix="semicolon-gutter-state-") as state:
            HELPERS.STATE_DIR = state
            shell = HELPERS.Pty(
                24, 112,
                argv=[str(BINARY), "--root", str(root), "--file", str(source)],
                env={"SEMICOLON_STATE": state, "TERM": "xterm-256color",
                     "SEMICOLON_ICONS": "plain"},
            )
            try:
                # Keyword/identifier ink changes insert SGR between words. Wait
                # for the complete identifier tail, then assert the full source
                # after removing only styling, not text or cursor operations.
                if not shell.wait_for(b"line_1"):
                    raise AssertionError("the 999-line source did not render")
                plain = re.sub(rb"\x1b\[[0-9;]*m|\x1b\(B", b"", bytes(shell.captured))
                assert b"int line_1;" in plain, "the complete 999-line source was not painted"
                for _ in range(10):
                    shell.send(b"\x1b[B" * 100)
                    shell.drain(0.1, cap=4.0)
                shell.send(b"\r")
                shell.drain(0.5)
                frame = shell.repaint(24)
                if b"1000." not in frame:
                    raise AssertionError("the live gutter clipped the period when line 1000 was added")
            finally:
                status, _ = shell.finish()
            if status != 0:
                raise AssertionError(f"Semicolon exited with status {status}")
    print("PASS code gutter right-aligns to the largest line number")


if __name__ == "__main__":
    main()
