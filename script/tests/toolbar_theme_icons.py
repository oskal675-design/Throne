#!/usr/bin/env python3
"""Run toolbar theme icon regression tests against a built Linux CMake tree.

Reuses the real application's object files, replacing only its startup main.
No test copy of the production repository/window implementation is used.
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
    output = build / "toolbar-theme-icons-tests"
    output.mkdir(exist_ok=True)
    obj = output / "toolbar_theme_icons.o"
    binary = output / "toolbar_theme_icons"
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
    for key in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(key, None)
    env["QT_QPA_PLATFORM"] = "offscreen"
    # No normal application bootstrap or EngineHost::init. All DB state is temporary.
    for label, arguments in (("default", []), ("enabled", ["--startup-enabled"])):
        with tempfile.TemporaryDirectory(prefix="data-", dir=output) as temporary:
            env["HOME"] = temporary
            env["XDG_CONFIG_HOME"] = temporary
            env["XDG_DATA_HOME"] = temporary
            env["XDG_CACHE_HOME"] = temporary
            result = subprocess.run([str(binary), *arguments], cwd=temporary, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        (output / f"gui-{label}.log").write_text(result.stdout)
        print(result.stdout, end="", flush=True)
        result.check_returncode()
    print(f"Logs: {output}")


if __name__ == "__main__":
    main()
