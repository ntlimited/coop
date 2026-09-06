#!/usr/bin/env python3
"""Run actual coop GDB commands at a breakpoint, then against a generated core."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("fixture", type=Path)
parser.add_argument("--bare", action="store_true", help="test saved stacks without io_uring")
parser.add_argument("--gdb", default="gdb")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
fixture = args.fixture.resolve()


def load_python(path):
    # GDB's source command treats quotes as part of the filename. Python repr
    # safely preserves spaces and punctuation without relying on that parser.
    filename = repr(str(path))
    return "python exec(compile(open(%s).read(), %s, 'exec'))" % (filename, filename)


with tempfile.TemporaryDirectory(prefix="coop-gdb-") as directory:
    directory = Path(directory)
    core = directory / "fixture.core"
    for mode in ("live", "core"):
        commands = ["set pagination off", "set confirm off", "set debuginfod enabled off"]
        if mode == "live":
            commands += ["break CoopGdbReady", "run" + (" --bare" if args.bare else "")]
        commands += [load_python(root / "tools/coop_gdb.py"),
                     load_python(root / "tests/gdb_checks.py")]
        if mode == "live":
            commands += ["generate-core-file " + str(core)]
        commands += ["quit"]
        script = directory / (mode + ".gdb")
        script.write_text("\n".join(commands) + "\n")
        invocation = [args.gdb, "--nx", "--batch", "-x", str(script), str(fixture)]
        if mode == "core":
            invocation += [str(core)]
        result = subprocess.run(invocation, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        print(mode + ":\n" + result.stdout)
        if result.returncode != 0 or "COOP_GDB_CHECKS_PASSED" not in result.stdout:
            raise SystemExit("coop GDB " + mode + " checks failed")
    if not core.exists():
        raise SystemExit("GDB did not generate a core file")
