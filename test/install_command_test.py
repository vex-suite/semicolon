#!/usr/bin/env python3
"""Owner: Semicolon's opt-in per-user command installer.

Disposable bin directories prove all command names, idempotence, and refusal
before mutation. This test does not edit a shell profile, build, or launch the UI.
"""
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "suite/projects/semicolon"
INSTALLER = APP / "install-command.sh"
LAUNCHER = APP / "semicolon"
PREVIEW_LAUNCHER = APP / "smc2"


def install(bin_dir):
    return subprocess.run(["sh", str(INSTALLER), str(bin_dir)],
                          capture_output=True, text=True, timeout=10)


def main():
    syntax = subprocess.run(["sh", "-n", str(INSTALLER)],
                            capture_output=True, text=True, timeout=10)
    assert syntax.returncode == 0, syntax.stderr
    with tempfile.TemporaryDirectory(prefix="semicolon-command-") as directory:
        scratch = Path(directory)
        bin_dir = scratch / "bin with spaces"
        first = install(bin_dir)
        assert first.returncode == 0, first.stderr
        for name in ("semicolon", "smc"):
            link = bin_dir / name
            assert link.is_symlink() and link.readlink() == LAUNCHER, name
        preview_link = bin_dir / "smc2"
        assert preview_link.is_symlink() and preview_link.readlink() == PREVIEW_LAUNCHER
        second = install(bin_dir)
        assert second.returncode == 0 and "Already installed" in second.stdout
        assert all((bin_dir / name).readlink() == LAUNCHER
                   for name in ("semicolon", "smc")), "idempotence lost a link"
        assert (bin_dir / "smc2").readlink() == PREVIEW_LAUNCHER

        foreign_dir = scratch / "foreign"
        foreign_dir.mkdir()
        foreign = foreign_dir / "smc"
        foreign.write_text("unrelated command\n")
        refused = install(foreign_dir)
        assert refused.returncode != 0 and "Refusing to replace" in refused.stderr
        assert foreign.read_text() == "unrelated command\n"
        assert not (foreign_dir / "semicolon").exists(), \
            "installer partially created semicolon before refusing smc"

        foreign_preview_dir = scratch / "foreign-preview"
        foreign_preview_dir.mkdir()
        foreign_preview = foreign_preview_dir / "smc2"
        foreign_preview.write_text("unrelated preview command\n")
        refused = install(foreign_preview_dir)
        assert refused.returncode != 0 and "Refusing to replace" in refused.stderr
        assert foreign_preview.read_text() == "unrelated preview command\n"
        assert not (foreign_preview_dir / "smc").exists(), \
            "installer partially created commands before refusing smc2"

        dangling_dir = scratch / "dangling"
        dangling_dir.mkdir()
        (dangling_dir / "smc2").symlink_to(scratch / "nonexistent")
        refused = install(dangling_dir)
        assert refused.returncode != 0 and not (dangling_dir / "semicolon").exists()
        assert (dangling_dir / "smc2").is_symlink(), "foreign symlink was replaced"

        legacy_dir = scratch / "existing"
        legacy_dir.mkdir()
        (legacy_dir / "semicolon").symlink_to(LAUNCHER)
        upgraded = install(legacy_dir)
        assert upgraded.returncode == 0 and (legacy_dir / "smc").readlink() == LAUNCHER
        assert (legacy_dir / "smc2").readlink() == PREVIEW_LAUNCHER

    print("Semicolon installer owner passed")


if __name__ == "__main__":
    main()
