#!/usr/bin/env python3
"""Validate release metadata and prepare GitHub Release notes.

The project intentionally exposes its version in several places because the
Windows build script needs a plain batch variable.  This helper makes that
constraint safe by requiring every declaration to match ``pyproject.toml``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path

SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
CHANGELOG_HEADING_RE = re.compile(
    r"^## \[(?P<version>[^\]]+)](?:\s+[—-]\s+.*)?\s*$",
    re.MULTILINE,
)


class ReleaseMetadataError(RuntimeError):
    """Raised when version declarations or release notes are inconsistent."""


@dataclass(frozen=True)
class ReleaseMetadata:
    version: str
    tag: str
    release_name: str
    prerelease: bool
    changelog_notes: str


def _match_one(path: Path, pattern: str, label: str) -> str:
    text = path.read_text(encoding="utf-8")
    match = re.search(pattern, text, flags=re.MULTILINE)
    if not match:
        raise ReleaseMetadataError(f"Cannot find {label} in {path}")
    return match.group(1)


def _read_pyproject_version(root: Path) -> str:
    with (root / "pyproject.toml").open("rb") as handle:
        data = tomllib.load(handle)
    try:
        return str(data["project"]["version"])
    except KeyError as exc:
        raise ReleaseMetadataError("pyproject.toml has no [project].version") from exc


def _extract_changelog_notes(changelog: Path, version: str) -> str:
    text = changelog.read_text(encoding="utf-8")
    headings = list(CHANGELOG_HEADING_RE.finditer(text))
    for index, heading in enumerate(headings):
        if heading.group("version") != version:
            continue
        start = heading.end()
        end = headings[index + 1].start() if index + 1 < len(headings) else len(text)
        notes = text[start:end].strip()
        if not notes:
            raise ReleaseMetadataError(
                f"CHANGELOG.md section [{version}] exists but contains no release notes"
            )
        return notes
    raise ReleaseMetadataError(
        f"CHANGELOG.md must contain a '## [{version}] — YYYY-MM-DD' section"
    )


def collect_release_metadata(root: Path) -> ReleaseMetadata:
    """Read and validate all release-facing metadata in *root*."""

    root = root.resolve()
    versions = {
        "pyproject.toml": _read_pyproject_version(root),
        "usb_monitor/__init__.py": _match_one(
            root / "usb_monitor" / "__init__.py",
            r'^__version__\s*=\s*["\']([^"\']+)["\']',
            "__version__",
        ),
        "usb_monitor/app.py": _match_one(
            root / "usb_monitor" / "app.py",
            r'^APP_VERSION\s*=\s*["\']([^"\']+)["\']',
            "APP_VERSION",
        ),
        "build/windows_nuitka.bat": _match_one(
            root / "build" / "windows_nuitka.bat",
            r'^set\s+"APP_VERSION=([^"]+)"',
            "APP_VERSION",
        ),
    }

    canonical = versions["pyproject.toml"]
    if not SEMVER_RE.fullmatch(canonical):
        raise ReleaseMetadataError(
            f"Version {canonical!r} must use numeric MAJOR.MINOR.PATCH format"
        )

    mismatches = {path: value for path, value in versions.items() if value != canonical}
    if mismatches:
        details = ", ".join(f"{path}={value}" for path, value in mismatches.items())
        raise ReleaseMetadataError(
            f"Version mismatch: pyproject.toml={canonical}; inconsistent declarations: {details}"
        )

    notes = _extract_changelog_notes(root / "CHANGELOG.md", canonical)
    return ReleaseMetadata(
        version=canonical,
        tag=f"v{canonical}",
        release_name=f"USB Monitor v{canonical}",
        prerelease=False,
        changelog_notes=notes,
    )


def write_release_files(metadata: ReleaseMetadata, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    notes = (
        f"<!-- Generated from CHANGELOG.md [{metadata.version}] by scripts/release_meta.py -->\n"
        f"{metadata.changelog_notes.rstrip()}\n"
    )
    (output_dir / "release-notes.md").write_text(notes, encoding="utf-8")
    payload = asdict(metadata)
    payload.pop("changelog_notes")
    (output_dir / "release-metadata.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def append_github_outputs(metadata: ReleaseMetadata, output_path: Path) -> None:
    values = {
        "version": metadata.version,
        "tag": metadata.tag,
        "release_name": metadata.release_name,
        "prerelease": str(metadata.prerelease).lower(),
    }
    with output_path.open("a", encoding="utf-8") as handle:
        for key, value in values.items():
            handle.write(f"{key}={value}\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command",
        choices=("check", "prepare"),
        help="check validates only; prepare also writes metadata and release notes",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(".release"),
        help="output directory used by the prepare command",
    )
    parser.add_argument(
        "--github-output",
        type=Path,
        default=None,
        help="optional GitHub Actions GITHUB_OUTPUT file",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        metadata = collect_release_metadata(args.root)
        if args.command == "prepare":
            output_dir = args.output_dir
            if not output_dir.is_absolute():
                output_dir = args.root / output_dir
            write_release_files(metadata, output_dir)
        github_output = args.github_output
        if github_output is None and os.environ.get("GITHUB_OUTPUT"):
            github_output = Path(os.environ["GITHUB_OUTPUT"])
        if github_output is not None:
            append_github_outputs(metadata, github_output)
    except (OSError, ReleaseMetadataError, tomllib.TOMLDecodeError) as exc:
        print(f"release metadata error: {exc}", file=sys.stderr)
        return 1

    print(
        json.dumps(
            {
                "version": metadata.version,
                "tag": metadata.tag,
                "release_name": metadata.release_name,
                "prerelease": metadata.prerelease,
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
