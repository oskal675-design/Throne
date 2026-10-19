#!/usr/bin/env python3
"""Run share-link authentication regression tests against a built Linux CMake tree.

Reuses the real application's object files, replacing only its startup main.
No test copy of the production HTTP/SOCKS parser implementation is used.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(command, *, cwd, env=None):
    print("+", shlex.join(map(str, command)), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    source = Path(__file__).with_suffix(".cpp").resolve()
    cache = (build / "CMakeCache.txt").read_text().splitlines()
    cmake = next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_COMMAND:"))
    run([cmake, "--build", str(build), "--target", "Throne"], cwd=build)
    entries = json.loads((build / "compile_commands.json").read_text())
    entry = next(item for item in entries if Path(item["file"]).name == "main.cpp"
                 and "Throne.dir" in item.get("command", " ".join(item.get("arguments", []))))
    output = build / "share-link-auth-tests"
    output.mkdir(exist_ok=True)
    obj = output / "share_link_auth.o"
    binary = output / "share_link_auth"
    compile_args = entry.get("arguments") or shlex.split(entry["command"])
    main_object = compile_args[compile_args.index("-o") + 1]
    compile_args[compile_args.index("-o") + 1] = str(obj)
    compile_args[compile_args.index("-c") + 1] = str(source)
    run(compile_args, cwd=entry["directory"])

    link_file = build / "CMakeFiles/Throne.dir/link.txt"
    if link_file.exists():
        link = link_file.read_text().strip()
    else:
        ninja = next(line.split("=", 1)[1] for line in cache if line.startswith("CMAKE_MAKE_PROGRAM:"))
        commands = subprocess.check_output([ninja, "-t", "commands", "Throne"], cwd=build, text=True)
        link = commands.strip().splitlines()[-1]
    link_args = shlex.split(link)
    # Ninja wraps a normal linker invocation in ': && ... && :'.
    if link_args[:2] == [":", "&&"]:
        link_args = link_args[2:]
    if link_args[-2:] == ["&&", ":"]:
        link_args = link_args[:-2]
    if any(token in link_args for token in ("&&", ";", "|")):
        raise RuntimeError("Unexpected linker wrapper; use a native Linux CMake build")
    link_args[link_args.index(main_object)] = str(obj)
    link_args[link_args.index("-o") + 1] = str(binary)
    # Do not overwrite the app's linker dependency file while linking tests.
    link_args = [arg for arg in link_args if not arg.startswith("-Wl,--dependency-file=")]
    run(link_args, cwd=build)

    env = os.environ.copy()
    # No normal application startup, database, network, proxy core or widgets.
    with tempfile.TemporaryDirectory(prefix="data-", dir=output) as temporary:
        env["HOME"] = temporary
        env["XDG_CONFIG_HOME"] = temporary
        env["XDG_DATA_HOME"] = temporary
        env["XDG_CACHE_HOME"] = temporary
        result = subprocess.run([str(binary)], cwd=temporary, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    (output / "regression.log").write_text(result.stdout)
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    print(f"Log: {output / 'regression.log'}")



if __name__ == "__main__":
    main()
