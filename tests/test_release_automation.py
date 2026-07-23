from __future__ import annotations

import json
import subprocess
import sys
import tomllib
from pathlib import Path


from scripts.release_meta import collect_release_metadata, write_release_files

ROOT = Path(__file__).resolve().parents[1]


def test_release_metadata_matches_project_and_changelog(tmp_path: Path) -> None:
    with (ROOT / "pyproject.toml").open("rb") as handle:
        expected = tomllib.load(handle)["project"]["version"]

    metadata = collect_release_metadata(ROOT)
    assert metadata.version == expected
    assert metadata.tag == f"v{expected}"
    assert metadata.release_name == f"USB Monitor v{expected}"
    assert "### Security" in metadata.changelog_notes

    write_release_files(metadata, tmp_path)
    payload = json.loads((tmp_path / "release-metadata.json").read_text(encoding="utf-8"))
    assert payload["version"] == metadata.version
    assert payload["tag"] == metadata.tag
    assert "Generated from CHANGELOG.md" in (tmp_path / "release-notes.md").read_text(
        encoding="utf-8"
    )


def test_release_metadata_cli_prepare(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "release_meta.py"),
            "prepare",
            "--root",
            str(ROOT),
            "--output-dir",
            str(tmp_path),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "release-notes.md").is_file()
    with (ROOT / "pyproject.toml").open("rb") as handle:
        expected = tomllib.load(handle)["project"]["version"]
    assert json.loads(result.stdout)["tag"] == f"v{expected}"


def test_release_workflow_is_valid_and_has_required_guards() -> None:
    workflow_path = ROOT / ".github" / "workflows" / "release.yml"
    source = workflow_path.read_text(encoding="utf-8")
    assert source.startswith("name: Build, tag and release\n")
    assert "\non:\n  push:" in source
    assert "  workflow_dispatch:" in source
    assert "\n  preflight:" in source
    assert "\n  build-windows:" in source
    assert "\n  publish:" in source
    assert "gh release create" in source
    assert "--target \"${GITHUB_SHA}\"" in source
    assert "contents: write" in source
    assert "build\\windows_nuitka.bat onefile" in source
    assert "SHA256SUMS.txt" in source
    assert "USBMONITOR_NO_UPX" in source
    assert "actions/checkout@v7" in source
    assert "actions/setup-python@v7" in source
    assert "actions/upload-artifact@v7" in source
    assert "actions/download-artifact@v7" in source
