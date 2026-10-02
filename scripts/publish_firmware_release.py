#!/usr/bin/env python3
"""Publish one minor firmware release for a main-branch commit, idempotently."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path


RELEASE_TAG = re.compile(r"^v1\.([0-9]+)\.0$")
COMMIT_SHA = re.compile(r"^[0-9a-fA-F]{40}(?:[0-9a-fA-F]{24})?$")
ASSETS = (Path("firmware/firmware.elf"), Path("firmware/firmware.bin"))


class CommandError(RuntimeError):
    pass


def run(command: list[str]) -> str:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no details"
        raise CommandError(f"{command[0]} command failed ({result.returncode}): {detail}")
    return result.stdout


def tags_pointing_at(commit: str) -> list[tuple[int, str]]:
    output = run(["git", "tag", "--list", "--points-at", commit, "v1.*.0"])
    matches = []
    for tag in output.splitlines():
        match = RELEASE_TAG.fullmatch(tag)
        if match:
            matches.append((int(match.group(1)), tag))
    return sorted(matches, reverse=True)


def all_release_tags() -> list[tuple[int, str]]:
    output = run(["git", "tag", "--list", "v1.*.0"])
    matches = []
    for tag in output.splitlines():
        match = RELEASE_TAG.fullmatch(tag)
        if match:
            matches.append((int(match.group(1)), tag))
    return matches


def is_ancestor(ancestor: str, descendant: str) -> bool:
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ancestor, descendant],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode == 0:
        return True
    if result.returncode == 1:
        return False
    detail = result.stderr.strip() or result.stdout.strip() or "no details"
    raise CommandError(
        f"git merge-base --is-ancestor failed ({result.returncode}): {detail}"
    )


def descendant_release_tag(commit: str, release_tags: list[tuple[int, str]]) -> str | None:
    for _, tag in release_tags:
        # Peel annotated tags explicitly and constrain the name to the tag namespace.
        target = run([
            "git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}",
        ]).strip()
        if target.lower() == commit.lower():
            continue
        if is_ancestor(commit, target):
            return tag
    return None


def releases_for_repository(repository: str) -> list[dict]:
    # Paginate the API rather than relying on a bounded `gh release list` result.
    # A failed request raises; it must never be treated as an empty release list.
    output = run(["gh", "api", f"repos/{repository}/releases", "--paginate", "--slurp"])
    try:
        pages = json.loads(output)
    except json.JSONDecodeError as error:
        raise CommandError(f"gh api returned invalid release JSON: {error}") from error

    if not isinstance(pages, list):
        raise CommandError("gh api returned an unexpected release response")
    if not pages:
        return []

    # --slurp wraps each paginated response page in an outer array.
    if all(isinstance(item, dict) for item in pages):
        releases = pages
    elif all(isinstance(page, list) for page in pages):
        releases = [release for page in pages for release in page]
    else:
        raise CommandError("gh api returned an unexpected paginated release response")

    if any(
        not isinstance(release, dict)
        or not isinstance(release.get("tag_name"), str)
        or not isinstance(release.get("draft"), bool)
        or not isinstance(release.get("assets"), list)
        for release in releases
    ):
        raise CommandError("gh api release response is missing required release fields")
    for release in releases:
        if any(
            not isinstance(asset, dict)
            or not isinstance(asset.get("name"), str)
            or not isinstance(asset.get("state"), str)
            for asset in release["assets"]
        ):
            raise CommandError("gh api release response contains an invalid asset")
    return releases


def release_is_complete(release: dict) -> bool:
    names = {
        asset.get("name")
        for asset in release.get("assets", [])
        if isinstance(asset, dict)
        and isinstance(asset.get("name"), str)
        and asset.get("state") == "uploaded"
    }
    return not release.get("draft", False) and {path.name for path in ASSETS} <= names


def publish(commit: str, repository: str) -> None:
    resolved = run(["git", "rev-parse", "--verify", f"{commit}^{{commit}}"]).strip()
    if resolved.lower() != commit.lower():
        raise CommandError("GITHUB_SHA does not identify a full commit in this checkout")

    for asset in ASSETS:
        if not asset.is_file():
            raise CommandError(f"firmware artifact is missing: {asset}")

    same_commit_tags = tags_pointing_at(commit)
    if same_commit_tags:
        releases = releases_for_repository(repository)
        by_tag = {release["tag_name"]: release for release in releases}

        # Any completed release already associated with this commit makes a rerun a no-op.
        for _, tag in same_commit_tags:
            release = by_tag.get(tag)
            if release is not None and release_is_complete(release):
                print(f"Release {tag} is already complete for {commit}; skipping.")
                return

        # Reuse an interrupted release (or an existing tag) rather than allocating
        # another minor version for this commit.
        tag = same_commit_tags[0][1]
        release = by_tag.get(tag)
        if release is None:
            create_release(tag, commit)
            return

        asset_states = {
            asset["name"]: asset["state"]
            for asset in release.get("assets", [])
        }
        incomplete_assets = [
            asset for asset in ASSETS
            if asset.name in asset_states and asset_states[asset.name] != "uploaded"
        ]
        missing_assets = [asset for asset in ASSETS if asset.name not in asset_states]
        for asset in incomplete_assets:
            run(["gh", "release", "upload", tag, str(asset), "--clobber"])
        if missing_assets:
            run(["gh", "release", "upload", tag, *(str(asset) for asset in missing_assets)])
        if release.get("draft", False):
            run(["gh", "release", "edit", tag, "--draft=false"])
        print(f"Completed existing release {tag} for {commit}.")
        return

    release_tags = all_release_tags()
    superseding_tag = descendant_release_tag(commit, release_tags)
    if superseding_tag is not None:
        # A tag reserves its version as soon as a release attempt starts. An
        # interrupted/draft release still reserves source order: allocating a
        # later version for an ancestor could make its retry look like a downgrade.
        print(
            f"Commit {commit} is an ancestor of existing release tag "
            f"{superseding_tag}; skipping new version allocation."
        )
        return

    latest_minor = max((minor for minor, _ in release_tags), default=0)
    create_release(f"v1.{latest_minor + 1}.0", commit)


def create_release(tag: str, commit: str) -> None:
    run([
        "gh", "release", "create", tag,
        *(str(asset) for asset in ASSETS),
        "--target", commit,
        "--title", f"Release {tag}",
        "--generate-notes",
    ])
    print(f"Published release {tag} for {commit}.")


def main() -> int:
    commit = os.environ.get("GITHUB_SHA", "")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    if not COMMIT_SHA.fullmatch(commit):
        print("GITHUB_SHA must be a full Git commit SHA.", file=sys.stderr)
        return 2
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        print("GITHUB_REPOSITORY must have owner/repository form.", file=sys.stderr)
        return 2

    try:
        publish(commit, repository)
    except CommandError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
