#!/usr/bin/env python3
"""Registered strict/sanitized Language-to-Semicolon consumer owner.

Injects bridge allocation failures only, not Language's own allocator. Exact
stderr counts are checked; this proves offline byte/pen behavior, not appearance.
"""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "suite/projects/semicolon"
LANGUAGE = ROOT / "suite/ecosystem/language"
DETERMINANT = ROOT / "suite/ecosystem/determinant/src"
VEXSPOKE = ROOT / "suite/ecosystem/vexspoke/src"
ENGINE = ROOT / "suite/ecosystem/relational-engine/src"


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n{result.stdout}{result.stderr}")
    return result


def main():
    spec = importlib.util.spec_from_file_location("semicolon_build", APP / "build.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    compiler = os.environ.get("CC", "clang")
    std_flag = build.c23_std_flag(compiler)
    flags = [std_flag, "-Wall", "-Wextra", "-Werror", "-O2", "-UNDEBUG"]
    if sys.platform == "darwin":
        flags += ["-arch", "arm64", "-mcpu=apple-m1", "-mmacosx-version-min=14.0"]
    includes = [f"-I{path}" for path in (APP, APP / "src", DETERMINANT, VEXSPOKE, ENGINE, LANGUAGE)]
    dependencies = [LANGUAGE / name for name in (
        "adapters/c/c23_highlights.c", "adapters/c/c23_lexer.c",
        "adapters/c/c23_lex_step.c", "syntax/role.c",
        "analysis/syntax_span.c", "document/source_range.c", "text/bounded_text.c")]
    dependencies += [DETERMINANT / "ui/surface/terminal_canvas.c"]
    with tempfile.TemporaryDirectory(prefix="semicolon-language-") as directory:
        directory = Path(directory)
        source = directory / "main.c"
        source.write_bytes(("int main() { return 42; } /*" + " " * 160 + "*/\n"
                            "/* start\nstill */ return 42;\n\"界\" name\n").encode()
                           + b"x\xff int k;\n")
        plain = directory / "notes.txt"
        plain.write_text("int is plain text here;\n")
        for mode, sanitizer in (("strict", []), ("asan-ubsan", ["-fsanitize=address,undefined",
                                                               "-fno-omit-frame-pointer"])):
            obj = directory / f"{mode}.o"
            run([compiler, *flags, *sanitizer, *includes,
                 "-Dmalloc=syntax_test_malloc", "-Dcalloc=syntax_test_calloc",
                 "-Drealloc=syntax_test_realloc", "-Dfree=syntax_test_free", "-c",
                 str(APP / "src/editor/semicolon_syntax.c"), "-o", str(obj)])
            binary = directory / mode
            run([compiler, *flags, *sanitizer, *includes,
                 str(ROOT / "semicolon/test/editor/semicolon_syntax_test.c"),
                 str(obj), *(str(path) for path in dependencies), "-o", str(binary)])
            result = run([str(binary)])
            # 3 row-admission + 3 palette refusals + 2 bridge allocation stages
            # (source and line directory) + one failed owner construction.
            # Language's own allocations are exercised by its owner, not this
            # bridge's allocator hooks.
            messages = result.stderr.splitlines()
            assert all(line.startswith("[vex] semicolon_syntax.c:") for line in messages), messages
            assert sum("rejected missing" in line for line in messages) == 3, messages
            assert sum("rejected palette" in line for line in messages) == 3, messages
            assert sum("storage refused" in line for line in messages) == 2, messages
            assert sum("owner allocation refused" in line for line in messages) == 1, messages
            assert len(messages) == 9, messages
            assert not result.stdout, result.stdout
            print(f"PASS {mode} SemicolonSyntax: 2560 spans, staged failures, recovery, leak accounting")
            host = directory / f"{mode}-host"
            curses = "-lncurses" if sys.platform == "darwin" else "-lncursesw"
            extra_platform = ["-fobjc-arc", "-framework", "AppKit"] if sys.platform == "darwin" else []
            run([compiler, *flags, *sanitizer, *includes,
                 str(ROOT / "semicolon/test/editor/language_bridge_test.c"),
                 *(str(path) for path in build.sources(DETERMINANT, VEXSPOKE, ENGINE, LANGUAGE)),
                 curses, *extra_platform, "-lm", "-o", str(host)])
            result = run([str(host), str(source), str(plain)])
            messages = result.stderr.splitlines()
            assert len(messages) == 1 and "SemicolonSyntax storage refused" in messages[0], messages
            assert not result.stdout, result.stdout
            print(f"PASS {mode} live editor code-path: ink/weight, wide cells, controls, caret, edits, scroll")
        # Real consumer build, not just a fixture that happens to include headers.
        output = directory / "semicolon"
        build.build(output, compiler=compiler)
        print("PASS strict Semicolon production link with real Language sources")
        original = output.read_bytes()
        previous = os.environ.get("SEMICOLON_LANGUAGE_SOURCE_DIR")
        try:
            os.environ["SEMICOLON_LANGUAGE_SOURCE_DIR"] = str(directory / "missing-language")
            try:
                build.build(output, compiler=compiler)
            except ValueError as error:
                assert "c23_lexer" in str(error) or "role.h" in str(error), error
            else:
                raise AssertionError("missing Language source root was accepted")
            assert output.read_bytes() == original, "failed link replaced the previous binary"
        finally:
            if previous is None:
                del os.environ["SEMICOLON_LANGUAGE_SOURCE_DIR"]
            else:
                os.environ["SEMICOLON_LANGUAGE_SOURCE_DIR"] = previous
        negative = subprocess.run(
            [compiler, *flags, *includes, "-x", "c", "-fsyntax-only", "-"],
            input='#include "editor/semicolon_syntax.h"\nfn f() { () SemicolonSyntax(1u); }\n',
            capture_output=True, text=True, timeout=30)
        assert negative.returncode != 0 and "too many arguments" in negative.stderr, negative.stderr
        print("PASS missing Language dependency preserves artifact; unsupported constructor arity rejects")
        # Scripted PTY output exercises the production loop and actual ncurses
        # quantization, rather than treating a canvas-only oracle as device proof.
        spec = importlib.util.spec_from_file_location("semicolon_pty_helpers",
                                                       ROOT / "semicolon/test/tui_main_test.py")
        helpers = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helpers)
        helpers.STATE_DIR = str(directory / "state")
        editable = directory / "color.c"
        editable.write_text("/** doc */\nint value;\n")
        shell = helpers.Pty(22, 112,
                            argv=[str(output), "--root", str(directory), "--file", str(editable),
                                  "--icons", "plain"],
                            env={"TERM": "xterm-256color", "SEMICOLON_BG": "#202020"})
        closed = False
        try:
            if not shell.wait_for(b"color.c", seconds=10.0):
                closed = True
                code, captured = shell.finish()
                raise AssertionError(f"C23 file did not open. Exit: {code}, captured: {captured!r}")
            payload = shell.repaint()
            text = re.sub(rb"\x1b\[[0-9;]*m|\x1b\(B", b"", payload)
            assert b"38;5;34m" in payload and b"int" in payload, payload
            assert b"38;5;108m" in payload and b"/** doc */" in text, payload
            shell.send(b"\x1b[B")
            shell.send(b"\x1b[H")
            shell.send(b"//")
            payload = shell.repaint()
            # The real caret reverses one glyph, inserting SGR/charset changes
            # inside the source run. Inspect text without those attributes while
            # independently asserting the emitted regular-comment foreground.
            text = re.sub(rb"\x1b\[[0-9;]*m|\x1b\(B", b"", payload)
            assert b"38;5;145m" in payload and b"//int value;" in text, payload
            deadline = time.monotonic() + 5.0
            while (editable.read_text() != "/** doc */\n//int value;\n"
                   and time.monotonic() < deadline):
                shell.drain(0.1)
            assert editable.read_text() == "/** doc */\n//int value;\n", "coloring changed saved source bytes"
        finally:
            if not closed:
                code, captured = shell.finish()
                assert code == 0, f"shell exited with {code}; captured: {captured!r}"
        print("PASS scripted PTY C23 keyword/doc/regular-comment foreground and edit recoloring; exact autosaved bytes")

        # The snapshot history owns and frees whole line arrays, so exercise the real
        # shell under ASan/UBSan rather than trusting the strict build alone. A multibyte
        # scalar is one transaction: undoing half of it would leave invalid UTF-8 and
        # show the grouping bug the PTY owner caught.
        sanitized = directory / "semicolon-asan"
        run([compiler, *flags, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             *includes, str(APP / "src/main/tui-main.c"),
             *(str(path) for path in build.sources(DETERMINANT, VEXSPOKE, ENGINE, LANGUAGE)),
             curses, *extra_platform, "-lm", "-o", str(sanitized)])
        history = directory / "history.c"
        history.write_bytes(b"int value;\n")
        history_shell = helpers.Pty(
            22, 112, argv=[str(sanitized), "--root", str(directory), "--file", str(history),
                           "--icons", "plain"],
            env={"TERM": "xterm-256color", "SEMICOLON_BG": "#202020",
                 "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"})

        def wait_bytes(expected, seconds=6.0):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if history.read_bytes() == expected:
                    return True
                history_shell.drain(0.1)
            return False

        try:
            assert history_shell.wait_for(b"history.c", seconds=10.0), "history file did not open"
            history_shell.send(b"AB")
            assert wait_bytes(b"ABint value;\n"), history.read_bytes()
            history_shell.send("界".encode("utf-8"))
            assert wait_bytes("AB界int value;\n".encode("utf-8")), history.read_bytes()
            history_shell.send(b"\x1a")
            assert wait_bytes(b"ABint value;\n"), history.read_bytes()
            history_shell.send(b"\x19")
            assert wait_bytes("AB界int value;\n".encode("utf-8")), history.read_bytes()
        finally:
            exit_code, captured = history_shell.finish()
            assert exit_code == 0, (exit_code, captured[-2000:])
            assert b"AddressSanitizer" not in captured and b"runtime error" not in captured, captured[-2000:]
        print("PASS asan-ubsan TUI snapshot history: multibyte undo/redo, exact autosaved bytes")


if __name__ == "__main__":
    main()
