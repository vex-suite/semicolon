#!/usr/bin/env python3
"""Launcher owner: Language's source-root override participates in rebuild checks.

A disposable fake builder/artifact proves invalidation, not C compilation or tty
appearance; the editor owner separately compiles/links the real dependency.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "suite/projects/semicolon"


def main():
    with tempfile.TemporaryDirectory(prefix="semicolon-launcher-language-") as directory:
        root = Path(directory)
        launcher = root / "semicolon"
        shutil.copy2(APP / "semicolon", launcher)
        (root / "src").mkdir()
        dependency_roots = {}
        for name in ("DETERMINANT", "VEXSPOKE", "ENGINE", "LANGUAGE"):
            path = root / name.lower()
            path.mkdir()
            dependency_roots[f"SEMICOLON_{name}_SOURCE_DIR"] = str(path)
        language_header = root / "language" / "lexer.h"
        language_header.write_text("/* initial dependency */\n")
        (root / "build.py").write_text(
            "from pathlib import Path\n"
            "root = Path(__file__).resolve().parent\n"
            "counter = root / 'count'\n"
            "generation = int(counter.read_text()) + 1 if counter.exists() else 1\n"
            "counter.write_text(str(generation))\n"
            "binary = root / '.build' / 'semicolon-tui'\n"
            "binary.parent.mkdir(exist_ok=True)\n"
            "binary.write_text('#!/bin/sh\\nprintf \\\"generation-' + str(generation) + '\\\\n\\\"\\n')\n"
            "binary.chmod(0o700)\n"
        )

        def launch():
            result = subprocess.run(["sh", str(launcher)], capture_output=True, text=True,
                                    env={**os.environ, **dependency_roots}, timeout=10)
            assert result.returncode == 0, result.stderr
            return result

        assert launch().stdout == "generation-1\n"
        unchanged = launch()
        assert unchanged.stdout == "generation-1\n" and not unchanged.stderr
        binary = root / ".build/semicolon-tui"
        language_header.write_text("/* changed dependency */\n")
        changed_time = max(language_header.stat().st_mtime_ns, binary.stat().st_mtime_ns + 1)
        os.utime(language_header, ns=(changed_time, changed_time))
        changed = launch()
        assert changed.stdout == "generation-2\n", "Language header did not invalidate the binary"
        assert "rebuilding" in changed.stderr
        assert launch().stdout == "generation-2\n", "unchanged Language rebuilt again"
    print("PASS Language source-root launcher invalidation, no-change reuse and rebuild")


if __name__ == "__main__":
    main()
