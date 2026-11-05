#!/usr/bin/env python3
"""PTY regression for delayed SGR suffixes and event-batch boundaries."""
import importlib.util
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_MODULE = ROOT / "semicolon/test/tui_main_test.py"
SPEC = importlib.util.spec_from_file_location("tui_main_test_helpers", TEST_MODULE)
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)


def main():
    HELPERS.ensure_built()
    with tempfile.TemporaryDirectory(prefix="semicolon-mouse-capture-") as directory:
        project = Path(directory)
        source = project / "hello.c"
        original = b"int main(void) { return 0; }\n"
        source.write_bytes(original)
        shell = HELPERS.Pty(
            22, 112,
            argv=[str(HELPERS.BINARY), "--root", str(project), "--file", str(source)],
            env={"SEMICOLON_ICONS": "plain"},
        )
        try:
            if not shell.wait_for(b"hello.c", 3.0):
                raise AssertionError("Semicolon did not open the disposable mouse-test file")
            # Reproduce a slow terminal transport split after the ncurses-bound prefix.
            shell.send(b"\x1b[<64;78;")
            shell.drain(0.35)
            shell.send(b"20M")
            shell.drain(0.3)
            # Cross multiple bounded event batches in one write, as a fast wheel scroll
            # does. The batch limit must not consume and drop the following report prefix.
            shell.send(b"".join(b"\x1b[<65;50;5M" for _ in range(64)))
            shell.drain(0.8)
            shell.send(b"\x13")  # Ctrl-S: any leaked suffix becomes persistent corruption.
            shell.drain(0.2)
        finally:
            status, _ = shell.finish()
        if status != 0:
            raise AssertionError(f"Semicolon exited with status {status}")
        if source.read_bytes() != original:
            raise AssertionError(
                f"mouse report suffix leaked into the editor: {source.read_bytes()!r}"
            )
    print("PASS delayed SGR capture and 64-report frame-boundary PTY regression")


if __name__ == "__main__":
    main()
