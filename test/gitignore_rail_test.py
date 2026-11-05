#!/usr/bin/env python3
"""Owner: Semicolon dot-entry visibility and text-driven ignore rail tint."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "suite/projects/semicolon"
SPEC = importlib.util.spec_from_file_location(
    "semicolon_tui_helpers", ROOT / "semicolon/test/tui_main_test.py")
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)


def main():
    with tempfile.TemporaryDirectory(prefix="semicolon-ignore-rail-") as directory:
        root = Path(directory)
        (root / ".github").mkdir()
        (root / ".idea").mkdir()
        (root / ".vscode").mkdir()
        (root / "build").mkdir()
        (root / "build" / "generated.c").write_text("generated\n")
        (root / "ignored.tmp").write_text("ignored\n")
        (root / "kept.tmp").write_text("kept\n")
        (root / ".gitkeep").write_text("")
        (root / "main.c").write_text("main\n")
        ignore = root / ".gitignore"
        ignore.write_text("build/\n*.tmp\n!kept.tmp\n")

        build = subprocess.run(["python3", str(APP / "build.py")], capture_output=True,
                               text=True, timeout=180)
        assert build.returncode == 0, build.stderr
        with tempfile.TemporaryDirectory(prefix="semicolon-ignore-state-") as state:
            HELPERS.STATE_DIR = state
            shell = HELPERS.Pty(
                20, 100,
                argv=[str(APP / ".build/semicolon-tui"), "--root", str(root),
                      "--file", str(root / "main.c"), "--icons", "plain"])
            assert shell.wait_for(b"ignored.tmp"), "tree did not paint the test entries"
            screen = bytes(shell.captured)
            orange = b"\x1b[38;5;215m"
            assert orange + b"ignored.tmp" in screen, "ignored filename did not use orange"
            assert orange + b"kept.tmp" not in screen, "negated filename stayed orange"
            for visible in (b".github", b".gitignore", b".gitkeep", b"build",
                            b"ignored.tmp", b"kept.tmp"):
                assert visible in screen, f"missing expected tree entry {visible.decode()}"
            for hidden in (b".idea", b".vscode"):
                assert hidden not in screen, f"unexpected hidden dot-directory {hidden.decode()}"

            ignore.write_text("kept.tmp\n")
            before = len(shell.captured)
            shell.force_repaint(rows=20)
            changed = bytes(shell.captured[before:])
            assert orange + b"kept.tmp" in changed, ".gitignore update did not recolor immediately"
            assert orange + b"ignored.tmp" not in changed, "removed ignore rule stayed orange"
            assert shell.finish()[0] == 0, "Semicolon PTY did not exit cleanly"
        print("Semicolon dot-entry and .gitignore rail owner passed")


if __name__ == "__main__":
    main()
