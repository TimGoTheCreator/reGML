#!/usr/bin/env python3
"""List package function calls without a matching reGML built-in handler."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def find_runner(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).expanduser().resolve()

    names = ("regml.exe", "regml") if sys.platform == "win32" else ("regml", "regml.exe")
    locations = [
        ROOT / "build" / "win-run",
        ROOT / "build",
        ROOT / "build" / "Release",
        ROOT / "build" / "Debug",
    ]
    for directory in locations:
        for name in names:
            candidate = directory / name
            if candidate.is_file():
                return candidate
    raise FileNotFoundError("could not find reGML; pass the built executable with --runner")


def read_function_rows(runner: Path, package: Path) -> list[tuple[int, str, bool, str]]:
    result = subprocess.run(
        [str(runner), "audit", str(package), "--functions"],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode:
        if result.stderr:
            sys.stderr.write(result.stderr)
        if result.stdout:
            sys.stderr.write(result.stdout)
        raise RuntimeError(f"reGML audit failed with exit code {result.returncode}")

    rows = []
    for line in result.stdout.splitlines():
        if not line.startswith("FUNCTION\t") or line.startswith("FUNCTION\toccurrences"):
            continue
        fields = line.split("\t", 4)
        if len(fields) != 5:
            raise RuntimeError(f"malformed function record from reGML: {line!r}")
        _, occurrences, first_address, defined, name = fields
        rows.append((int(occurrences), first_address, defined == "1", name))
    return rows


def implemented_names(source_path: Path) -> tuple[set[str], list[str]]:
    source = source_path.read_text(encoding="utf-8")
    start = source.find("Value call(std::string_view name,")
    if start < 0:
        raise RuntimeError(f"could not find built-in dispatcher in {source_path}")
    pending = source.find('throw Error(ErrorKind::unsupported, "engine built-in pending:', start)
    if pending < 0:
        raise RuntimeError(f"could not find the pending built-in fallback in {source_path}")
    dispatcher = source[start:pending]

    exact = set(re.findall(r'\bname\s*==\s*"([^"\n]+)"', dispatcher))
    prefixes = []
    for match in re.finditer(r'\bname\.starts_with\("([^"\n]+)"\)', dispatcher):
        line_start = dispatcher.rfind("\n", 0, match.start()) + 1
        line_end = dispatcher.find("\n", match.end())
        line = dispatcher[line_start : line_end if line_end >= 0 else len(dispatcher)]
        # This branch is only used by the separate GMRT host path.
        if "gmrt_project" in line:
            continue
        prefixes.append(match.group(1))
    return exact, prefixes


def make_report(package: Path, source: Path, runner: Path) -> str:
    exact, prefixes = implemented_names(source)
    rows = read_function_rows(runner, package)

    # Duplicate symbol records can refer to the same function; combine their
    # counts so the report ranks names rather than symbol-table entries.
    combined: dict[str, tuple[int, int, bool]] = {}
    for occurrences, first_address, defined, name in rows:
        address = int(first_address, 0)
        old = combined.get(name)
        if old:
            combined[name] = (old[0] + occurrences, min(old[1], address), old[2] or defined)
        else:
            combined[name] = (occurrences, address, defined)

    candidates = []
    internal_names = []
    for name, (occurrences, first_address, defined) in combined.items():
        if defined or name in exact or any(name.startswith(prefix) for prefix in prefixes):
            continue
        if name.startswith("@@") and name.endswith("@@"):
            internal_names.append((occurrences, first_address, name))
        else:
            candidates.append((occurrences, first_address, name))
    candidates.sort(key=lambda row: (-row[0], row[2]))
    internal_names.sort(key=lambda row: (-row[0], row[2]))

    lines = [
        f"Static built-in audit: {package}",
        f"Function names checked: {len(combined)}",
        f"Likely unimplemented built-in names: {len(candidates)}",
        "",
        "Candidates are called by the game, are not defined in CODE/SCPT, and have no",
        "matching name check in GameSession::Impl::call. This is a source-based guess;",
        "dynamic dispatch, aliases, or incomplete handlers can change the result.",
        "First address is the package bytecode address reported by the linker.",
        "",
        "CALLS  FIRST ADDRESS  NAME",
    ]
    lines.extend(f"{count:5}  {address:#13x}  {name}" for count, address, name in candidates)

    if internal_names:
        lines.extend(["", "Internal-looking names need separate review:"])
        lines.extend(f"{count:5}  {address:#13x}  {name}" for count, address, name in internal_names)

    camera = combined.get("view_get_camera")
    if camera:
        count, address, defined = camera
        state = "candidate" if not defined and "view_get_camera" not in exact else "not listed as missing"
        lines.extend(["", f"view_get_camera: {state} ({count} calls, first at {address:#x})"])
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="Game package, usually data.win")
    parser.add_argument("--runner", help="path to a reGML executable with audit --functions support")
    parser.add_argument("--source", type=Path, default=ROOT / "src" / "game.cpp")
    parser.add_argument("--output", type=Path, help="also save the report to this file")
    args = parser.parse_args()

    package = args.package.expanduser().resolve()
    source = args.source.expanduser().resolve()
    runner = find_runner(args.runner)
    report = make_report(package, source, runner)
    print(report, end="")
    if args.output:
        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(report, encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
