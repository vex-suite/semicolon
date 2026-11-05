#!/usr/bin/env python3
"""Owner: Semicolon's standalone supported-icon catalog command."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "suite/projects/semicolon"


def main():
    with tempfile.TemporaryDirectory(prefix="semicolon-icon-test-") as directory:
        binary = Path(directory) / "semicolon-tui"
        build = subprocess.run(["python3", str(PROJECT / "build.py"), "--output", str(binary)],
                               capture_output=True, text=True, timeout=180)
        if build.returncode:
            raise SystemExit(build.stderr or "Semicolon build failed")
        result = subprocess.run([str(binary), "icons"], capture_output=True, text=True,
                                env={**os.environ, "LC_ALL": "C.UTF-8"}, timeout=5)
    assert result.returncode == 0, result.stderr
    assert "Folder" in result.stdout and "Open folder" in result.stdout
    assert "#C58A52" in result.stdout and "#F08A32" in result.stdout
    for format_name in ("C++", "Java", "Java archive", "Python", "Rust", "Zig",
                        "Markdown", "Plain text", "Image", "Audio", "Video", "Archive",
                        "macOS application bundle", "Windows executable"):
        assert format_name in result.stdout, f"missing {format_name} catalog row"
    for special in ("LICENSE", "Dockerfile", "README.md", "settings.json", "Settings"):
        assert special in result.stdout, f"missing special/UI icon row {special}"
    assert "\x1b[38;2;" in result.stdout, "glyph colors were not emitted as ANSI true color"
    assert "Unknown extensions use the generic file icon" in result.stdout
    print("Semicolon icon catalog command passed")


if __name__ == "__main__":
    main()
