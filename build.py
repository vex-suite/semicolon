#!/usr/bin/env python3
"""Explicit strict native build for the semicolon terminal shell.

MODULE: build.py
REGISTRY: sources(determinant, vexspoke, engine, language) lists the real toolkit,
consumer bridge, C23 lexical dependencies and borrowed R2 units, including the
engine's platform clipboard seam (NSPasteboard/AppKit on macOS, the in-process
stub elsewhere). run(argv, timeout)
executes a bounded compiler child. build(output, compiler) resolves explicit
dependency roots, compiles those sources, and publishes the binary
by atomic replacement. No fetch, no installation, no program run, no tty use.
Standalone consumers supply source roots through the SEMICOLON_*_SOURCE_DIR
environment variables; the workspace paths are discovery defaults, not an
ecosystem dependency. A missing header is a real build failure.
SEMICOLON_LANGUAGE_SOURCE_DIR names the Language repository root; unlike the
other source-root options, that owner has no src/ subdirectory.
The compiler and the output parent are trusted; a failed compile preserves any
existing output and reclaims its own temporary directory.
"""
import argparse
import os
import platform
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT.parents[1]


def run(argv, timeout=180):
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout)


def c23_std_flag(compiler):
    """Pick the newest C23 spelling this compiler accepts, probing once.

    Apple clang before 16 accepts `-std=gnu2x` but rejects `-std=gnu23`; newer
    LLVM and GCC accept both. The probe is cold (one trivial syntax-only
    compile) and the chosen spelling is stated when it is the fallback.
    """
    compiler_cmd = shlex.split(compiler) if isinstance(compiler, str) else list(compiler)
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as probe:
        probe.write("#include <stddef.h>\nint main(void) { void *p = nullptr; return p != 0; }\n")
        probe_path = probe.name
    try:
        for flag in ("-std=gnu23", "-std=gnu2x"):
            result = run([*compiler_cmd, flag, "-fsyntax-only", probe_path], timeout=60)
            if result.returncode == 0:
                if flag != "-std=gnu23":
                    print(f"semicolon build: compiler rejects -std=gnu23; "
                          f"using {flag}", file=sys.stderr)
                return flag
    finally:
        Path(probe_path).unlink(missing_ok=True)
    raise SystemExit("semicolon build: compiler accepts neither -std=gnu23 "
                     "nor -std=gnu2x")


def sources(determinant, vexspoke, engine, language):
    produced = sorted(determinant.glob("ui/*/*.c"))
    produced += sorted(determinant.glob("events/*.c"))
    produced += sorted(determinant.glob("format/*.c"))
    produced += [vexspoke / "primitive/vex-string.c", vexspoke / "atomic/spin.c",
                 vexspoke / "system/browser.c", engine / "nio/mem.c"]
    # The real system clipboard is an R2 engine seam: NSPasteboard on macOS, the
    # in-process stub elsewhere. A preview never reaches the pasteboard itself.
    produced += [engine / ("io/clipboard_mac.m" if sys.platform == "darwin"
                          else "io/clipboard_stub.c")]
    produced += [ROOT / "src/editor/semicolon_syntax.c"]
    produced += [language / name for name in (
        "adapters/c/c23_highlights.c", "adapters/c/c23_lexer.c",
        "adapters/c/c23_lex_step.c", "syntax/role.c",
        "analysis/syntax_span.c", "document/source_range.c", "text/bounded_text.c")]
    return produced


def build(output, compiler=None, entry=None, extra_sources=None):
    if compiler is None:
        compiler = os.environ.get("CC", "clang")
    determinant = Path(os.environ.get("SEMICOLON_DETERMINANT_SOURCE_DIR",
                                      WORKSPACE / "vexgraph-ecosystem/determinant/src"))
    vexspoke = Path(os.environ.get("SEMICOLON_VEXSPOKE_SOURCE_DIR",
                                   WORKSPACE / "vexgraph-ecosystem/vexspoke/src"))
    engine = Path(os.environ.get("SEMICOLON_ENGINE_SOURCE_DIR",
                                  WORKSPACE / "vexgraph-ecosystem/relational-engine/src"))
    language = Path(os.environ.get("SEMICOLON_LANGUAGE_SOURCE_DIR",
                                    WORKSPACE / "vexgraph-ecosystem/language"))
    # R0 owns the canonical platform identity header; every tier reaches it
    # downward (the One File, All Platforms Law, home-is-the-floor clause).
    lifetime = Path(os.environ.get("SEMICOLON_LIFETIME_SOURCE_DIR",
                                    WORKSPACE / "vexgraph-ecosystem/lifetime/src"))
    entry = Path(entry) if entry is not None else ROOT / "src/main/tui-main.c"
    if not entry.is_file():
        raise ValueError(f"missing entry point: {entry}")
    extra_sources = [Path(source) for source in (extra_sources or [])]
    for source in extra_sources:
        if not source.is_file():
            raise ValueError(f"missing additional source: {source}")
    compiler_cmd = shlex.split(compiler) if isinstance(compiler, str) else list(compiler)
    flags = [c23_std_flag(compiler), "-Wall", "-Wextra", "-Werror", "-O2"]
    curses_libraries = ["-lncurses"]
    platform_libraries = []
    if sys.platform == "darwin":
        has_arch = any(arg in ("-arch", "-target") or arg.startswith(("--target=", "-target=")) for arg in compiler_cmd)
        if not has_arch:
            if platform.machine() == "arm64":
                flags += ["-arch", "arm64", "-mcpu=apple-m1", "-mmacosx-version-min=14.0"]
            else:
                flags += ["-arch", "x86_64", "-mmacosx-version-min=13.0"]
        # The clipboard seam is Objective-C with ARC, and NSPasteboard lives in AppKit.
        flags += ["-fobjc-arc"]
        platform_libraries = ["-framework", "AppKit"]
    elif sys.platform == "win32":
        curses_libraries = []
        platform_libraries = ["-luser32", "-lkernel32"]
    else:
        # Linux commonly splits the wide-character ABI into ncursesw; Darwin's
        # system ncurses exports the same wide API from -lncurses.
        curses_libraries = ["-lncursesw"]
    output = Path(output).absolute()
    if output.is_symlink() or output.is_dir():
        raise ValueError("invalid build output")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".semicolon-build-", dir=output.parent) as temp:
        artifact = Path(temp) / ("semicolon-tui.exe" if sys.platform == "win32" else "semicolon-tui")
        argv = [*compiler_cmd, *flags, "-I", str(ROOT), "-I", str(ROOT / "src"),
                "-I", str(determinant), "-I", str(vexspoke), "-I", str(engine), "-I", str(language),
                "-I", str(lifetime),
                str(entry), *(str(source) for source in extra_sources),
                *(str(source) for source in sources(determinant, vexspoke, engine, language)),
                *curses_libraries, *platform_libraries, "-lm", "-o", str(artifact)]
        result = run(argv)
        if result.returncode:
            raise ValueError(result.stderr.strip() or "compile failed")
        os.replace(artifact, output)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".build/semicolon-tui")
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--entry", type=Path, help="alternate application entry source")
    parser.add_argument("--source", action="append", default=[],
                        help="additional application source; may be repeated")
    args = parser.parse_args()
    try:
        print(build(args.output, args.compiler, args.entry, args.source))
    except (OSError, ValueError) as error:
        print(f"[vex] semicolon build: {error}", file=sys.stderr)
        raise SystemExit(1)
