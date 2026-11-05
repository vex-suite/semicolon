#!/usr/bin/env python3
"""The rail starts with its project root and keeps deep folder labels readable."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "suite/projects/semicolon"
BINARY = APP / ".build/semicolon-tui"


def main():
    subprocess.run(["python3", str(APP / "build.py")], check=True, timeout=600)
    with tempfile.TemporaryDirectory(prefix="semicolon-rail-format-") as directory:
        root = Path(directory)
        nested = root / "one" / "two" / "three"
        nested.mkdir(parents=True)
        source = nested / "deep-file-name-that-should-stay-readable.c"
        source.write_text("int value;\n")
        result = subprocess.run(
            [str(BINARY), "--dump", "80x12", "--root", str(root), "--file", str(source),
             "--icons", "plain"],
            check=True, capture_output=True, text=True, timeout=60,
        )
        lines = result.stdout.splitlines()
        assert "semicolon-rail-for" in lines[0], "project root is missing from the top row"
        assert "semicolon-rail-for" not in lines[1], "root was duplicated in the child rows"
        assert not any(mark in result.stdout for mark in ("▣", "⌕", "⑂", "⚙")), \
            "an activity icon strip appeared in the file list"
        rail = "\n".join(line[:30] for line in result.stdout.splitlines()[1:8])
        for label in ("one", "two", "three"):
            assert label in rail, f"nested folder {label!r} disappeared from the rail: {rail!r}"
        for depth, label in enumerate(("one", "two", "three"), start=2):
            row = next(line for line in lines[1:] if f"[] {label}" in line[:50])
            assert len(row) - len(row.lstrip(" ")) == depth * 2, \
                f"tree level did not advance by two spaces: {row!r}"
        assert "deep-file" in rail, f"deep filename was lost behind indentation: {rail!r}"
        # Use a short root so its location can be read without label truncation.
        project = root / "proj"
        project.mkdir()
        source = project / "main.c"
        source.write_text("int value;\n")
        command = [str(BINARY), "--dump", "100x12", "--root", ".",
                   "--file", str(source), "--icons", "plain"]
        env = dict(os.environ, HOME=str(root.resolve()))
        relative = subprocess.run(command, cwd=project, env=env, check=True,
                                  capture_output=True, text=True, timeout=60)
        header = relative.stdout.splitlines()[0]
        assert "⌄ [] proj ~/proj" in header, header
        assert "[- main.c" in header[50:], "location displaced the file tabs"
        # A HOME prefix without a complete component must not abbreviate a sibling.
        env["HOME"] = str(root.resolve())[:-1]
        absolute = subprocess.run(command, cwd=project, env=env, check=True,
                                  capture_output=True, text=True, timeout=60)
        header = absolute.stdout.splitlines()[0]
        assert " /" in header[:50] and "~/" not in header[:50], header
        assert "…" in header[:50], "long location was silently clipped"
        # Inspect actual PTY foreground: the location is neutral gray, not the
        # purple-muted tab palette. No physical terminal appearance is claimed.
        import importlib.util
        spec = importlib.util.spec_from_file_location("rail_header_helpers",
                                                      ROOT / "semicolon/test/tui_main_test.py")
        helpers = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helpers)
        helpers.STATE_DIR = str(root / "state")
        shell = helpers.Pty(20, 100,
                            argv=[str(BINARY), "--root", str(project), "--file",
                                  str(source), "--icons", "plain"],
                            env={"HOME": str(root.resolve()), "TERM": "xterm-256color"})
        try:
            assert shell.wait_for(b"~/proj"), "project location absent from PTY header"
            assert re.search(rb"\x1b\[38;5;145m(?:\x1b\[[0-9;]*m)*~/proj",
                             bytes(shell.captured)), "location did not use neutral grayscale"
            shell.click(5, 0)
            folded = shell.repaint()
            assert "› [] proj".encode() in folded and b"~/proj" in folded, \
                "root did not fold from its pinned header"
            shell.click(5, 0)
            assert "⌄ [] proj".encode() in shell.repaint(), "root did not reopen"
        finally:
            assert shell.finish()[0] == 0
    print("PASS nested rail keeps folder and file names visible")


if __name__ == "__main__":
    main()
