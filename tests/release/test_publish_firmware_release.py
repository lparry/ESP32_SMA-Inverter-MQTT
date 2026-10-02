#!/usr/bin/env python3
"""Exercise the actual release publisher against temporary Git repos and fake gh."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PUBLISHER = ROOT / "scripts" / "publish_firmware_release.py"
REAL_GIT = shutil.which("git")
FAKE_GH = r'''#!/usr/bin/env python3
import json
import os
import subprocess
import sys
from pathlib import Path

args = sys.argv[1:]
log_path = Path(os.environ["FAKE_GH_LOG"])
with log_path.open("a", encoding="utf-8") as log:
    log.write(json.dumps(args) + "\n")

state_path = Path(os.environ["FAKE_GH_STATE"])
state = json.loads(state_path.read_text(encoding="utf-8"))

if args[:1] == ["api"]:
    if os.environ.get("FAKE_GH_API_FAIL"):
        print("HTTP 502: simulated API failure", file=sys.stderr)
        raise SystemExit(1)
    # gh api --paginate --slurp returns an outer array containing page arrays.
    print(json.dumps([state["releases"]]))
    raise SystemExit(0)

if args[:2] == ["release", "create"]:
    tag = args[2]
    if any(release["tag_name"] == tag for release in state["releases"]):
        print("release already exists", file=sys.stderr)
        raise SystemExit(1)
    target = args[args.index("--target") + 1]
    assets = [Path(arg).name for arg in args[3:args.index("--target")]]
    state["releases"].append({
        "tag_name": tag,
        "draft": False,
        "assets": [{"name": name, "state": "uploaded"} for name in assets],
    })
    state_path.write_text(json.dumps(state), encoding="utf-8")
    # A later checkout fetches the release tag created remotely.
    existing_tag = subprocess.run(
        ["git", "rev-parse", "--verify", f"refs/tags/{tag}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if existing_tag.returncode != 0:
        subprocess.run(["git", "tag", tag, target], check=True)
    raise SystemExit(0)

if args[:2] == ["release", "upload"]:
    tag = args[2]
    release = next((item for item in state["releases"] if item["tag_name"] == tag), None)
    if release is None:
        print("release not found", file=sys.stderr)
        raise SystemExit(1)
    for name in (Path(arg).name for arg in args[3:] if not arg.startswith("--")):
        release["assets"] = [asset for asset in release["assets"] if asset.get("name") != name]
        release["assets"].append({"name": name, "state": "uploaded"})
    state_path.write_text(json.dumps(state), encoding="utf-8")
    raise SystemExit(0)

if args[:2] == ["release", "edit"]:
    tag = args[2]
    release = next((item for item in state["releases"] if item["tag_name"] == tag), None)
    if release is None:
        print("release not found", file=sys.stderr)
        raise SystemExit(1)
    if "--draft=false" in args:
        release["draft"] = False
    state_path.write_text(json.dumps(state), encoding="utf-8")
    raise SystemExit(0)

print("unexpected gh invocation: " + " ".join(args), file=sys.stderr)
raise SystemExit(2)
'''


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=repo, check=True, text=True, capture_output=True
    )
    return result.stdout.strip()


class FirmwareReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.name", "Release Test")
        git(self.repo, "config", "user.email", "release-test@example.invalid")
        (self.repo / "README").write_text("commit one\n", encoding="utf-8")
        git(self.repo, "add", "README")
        git(self.repo, "commit", "-qm", "commit one")
        self.commit_one = git(self.repo, "rev-parse", "HEAD")

        self.firmware = self.repo / "firmware"
        self.firmware.mkdir()
        (self.firmware / "firmware.elf").write_bytes(b"test elf")
        (self.firmware / "firmware.bin").write_bytes(b"test bin")

        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        self.gh_log = self.root / "gh-calls.jsonl"
        self.gh_log.touch()
        self.gh_state = self.root / "gh-state.json"
        self.set_releases([])
        fake_gh = fake_bin / "gh"
        fake_gh.write_text(FAKE_GH, encoding="utf-8")
        fake_gh.chmod(0o755)

        fake_git = fake_bin / "git"
        fake_git.write_text(r'''#!/usr/bin/env python3
import os
import sys

args = sys.argv[1:]
if args[:2] == ["merge-base", "--is-ancestor"] and os.environ.get("FAKE_GIT_ANCESTOR_FAIL"):
    print("simulated merge-base failure", file=sys.stderr)
    raise SystemExit(2)
real_git = os.environ["REAL_GIT"]
os.execv(real_git, [real_git, *args])
''', encoding="utf-8")
        fake_git.chmod(0o755)

        self.base_env = os.environ.copy()
        self.base_env.update({
            "PATH": f"{fake_bin}{os.pathsep}{self.base_env.get('PATH', '')}",
            "GITHUB_REPOSITORY": "example/firmware",
            "FAKE_GH_LOG": str(self.gh_log),
            "FAKE_GH_STATE": str(self.gh_state),
            "REAL_GIT": REAL_GIT,
        })
        self.base_env.pop("FAKE_GH_API_FAIL", None)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def set_releases(self, releases: list[dict]) -> None:
        self.gh_state.write_text(json.dumps({"releases": releases}), encoding="utf-8")

    def releases(self) -> list[dict]:
        return json.loads(self.gh_state.read_text(encoding="utf-8"))["releases"]

    def gh_calls(self) -> list[list[str]]:
        return [json.loads(line) for line in self.gh_log.read_text(encoding="utf-8").splitlines()]

    def run_publisher(
        self,
        commit: str,
        *,
        api_failure: bool = False,
        ancestor_git_failure: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        env = self.base_env.copy()
        env["GITHUB_SHA"] = commit
        if api_failure:
            env["FAKE_GH_API_FAIL"] = "1"
        if ancestor_git_failure:
            env["FAKE_GIT_ANCESTOR_FAIL"] = "1"
        return subprocess.run(
            [os.environ.get("PYTHON", "python3"), str(PUBLISHER)],
            cwd=self.repo,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def commit_two(self) -> str:
        (self.repo / "README").write_text("commit two\n", encoding="utf-8")
        git(self.repo, "add", "README")
        git(self.repo, "commit", "-qm", "commit two")
        return git(self.repo, "rev-parse", "HEAD")

    def test_first_release_uses_next_minor_and_publishes_both_artifacts(self) -> None:
        result = self.run_publisher(self.commit_one)

        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.gh_calls()
        creates = [call for call in calls if call[:2] == ["release", "create"]]
        self.assertEqual(len(creates), 1)
        self.assertEqual(creates[0][2], "v1.1.0")
        self.assertIn(self.commit_one, creates[0])
        self.assertEqual(
            {asset["name"] for asset in self.releases()[0]["assets"]},
            {"firmware.elf", "firmware.bin"},
        )
        self.assertEqual(git(self.repo, "rev-parse", "v1.1.0^{}"), self.commit_one)

    def test_successful_rerun_of_same_commit_skips_publication(self) -> None:
        first = self.run_publisher(self.commit_one)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.gh_log.write_text("", encoding="utf-8")

        rerun = self.run_publisher(self.commit_one)

        self.assertEqual(rerun.returncode, 0, rerun.stderr)
        self.assertIn("already complete", rerun.stdout)
        calls = self.gh_calls()
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0], "api")

    def test_new_commit_gets_next_minor_after_prior_release(self) -> None:
        first = self.run_publisher(self.commit_one)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.gh_log.write_text("", encoding="utf-8")
        commit_two = self.commit_two()

        result = self.run_publisher(commit_two)

        self.assertEqual(result.returncode, 0, result.stderr)
        created = [call[2] for call in self.gh_calls() if call[:2] == ["release", "create"]]
        self.assertEqual(created, ["v1.2.0"])
        self.assertEqual(git(self.repo, "rev-parse", "v1.2.0^{}"), commit_two)

    def test_ancestor_build_skips_after_descendant_release_published_first(self) -> None:
        descendant = self.commit_two()
        newer = self.run_publisher(descendant)
        self.assertEqual(newer.returncode, 0, newer.stderr)
        self.assertEqual(git(self.repo, "rev-parse", "v1.1.0^{}"), descendant)
        self.gh_log.write_text("", encoding="utf-8")

        older = self.run_publisher(self.commit_one)

        self.assertEqual(older.returncode, 0, older.stderr)
        self.assertIn("ancestor of existing release tag v1.1.0", older.stdout)
        self.assertFalse(any(call[:2] == ["release", "create"] for call in self.gh_calls()))
        self.assertEqual([release["tag_name"] for release in self.releases()], ["v1.1.0"])

    def test_incomplete_annotated_descendant_tag_still_reserves_version(self) -> None:
        descendant = self.commit_two()
        git(self.repo, "tag", "-a", "v1.8.0", "-m", "reserved release", descendant)
        self.set_releases([{
            "tag_name": "v1.8.0",
            "draft": True,
            "assets": [{"name": "firmware.elf", "state": "uploaded"}],
        }])

        result = self.run_publisher(self.commit_one)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ancestor of existing release tag v1.8.0", result.stdout)
        self.assertEqual([release["tag_name"] for release in self.releases()], ["v1.8.0"])
        self.assertFalse(any(call[:2] == ["release", "create"] for call in self.gh_calls()))

    def test_divergent_release_tag_does_not_block_main_commit(self) -> None:
        main_branch = git(self.repo, "branch", "--show-current")
        git(self.repo, "switch", "-c", "divergent-release")
        (self.repo / "README").write_text("divergent commit\n", encoding="utf-8")
        git(self.repo, "add", "README")
        git(self.repo, "commit", "-qm", "divergent commit")
        divergent = git(self.repo, "rev-parse", "HEAD")
        git(self.repo, "tag", "v1.1.0", divergent)
        git(self.repo, "switch", main_branch)
        main_commit = self.commit_two()
        self.set_releases([{
            "tag_name": "v1.1.0",
            "draft": False,
            "assets": [
                {"name": "firmware.elf", "state": "uploaded"},
                {"name": "firmware.bin", "state": "uploaded"},
            ],
        }])

        result = self.run_publisher(main_commit)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            [call[2] for call in self.gh_calls() if call[:2] == ["release", "create"]],
            ["v1.2.0"],
        )
        self.assertEqual(git(self.repo, "rev-parse", "v1.2.0^{}"), main_commit)

    def test_git_ancestry_errors_are_not_treated_as_divergence(self) -> None:
        descendant = self.commit_two()
        git(self.repo, "tag", "v1.1.0", descendant)

        result = self.run_publisher(self.commit_one, ancestor_git_failure=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("git merge-base --is-ancestor failed (2)", result.stderr)
        self.assertIn("simulated merge-base failure", result.stderr)
        self.assertFalse(any(call[:2] == ["release", "create"] for call in self.gh_calls()))

    def test_existing_tag_without_release_is_reused_after_interrupted_run(self) -> None:
        git(self.repo, "tag", "-a", "v1.7.0", "-m", "release tag", self.commit_one)

        result = self.run_publisher(self.commit_one)

        self.assertEqual(result.returncode, 0, result.stderr)
        creates = [call for call in self.gh_calls() if call[:2] == ["release", "create"]]
        self.assertEqual(len(creates), 1)
        self.assertEqual(creates[0][2], "v1.7.0")
        self.assertEqual(len(self.releases()), 1)

    def test_incomplete_release_is_repaired_in_place(self) -> None:
        git(self.repo, "tag", "v1.4.0", self.commit_one)
        self.set_releases([{
            "tag_name": "v1.4.0",
            "draft": True,
            "assets": [{"name": "firmware.elf", "state": "starter"}],
        }])

        result = self.run_publisher(self.commit_one)

        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.gh_calls()
        self.assertFalse(any(call[:2] == ["release", "create"] for call in calls))
        uploads = [call for call in calls if call[:2] == ["release", "upload"]]
        self.assertEqual(uploads[0], ["release", "upload", "v1.4.0", "firmware/firmware.elf", "--clobber"])
        self.assertEqual(uploads[1], ["release", "upload", "v1.4.0", "firmware/firmware.bin"])
        self.assertTrue(any(call[:2] == ["release", "edit"] for call in calls))
        repaired = self.releases()[0]
        self.assertFalse(repaired["draft"])
        self.assertEqual(
            {asset["name"] for asset in repaired["assets"]},
            {"firmware.elf", "firmware.bin"},
        )

    def test_github_api_failure_is_not_treated_as_missing_release(self) -> None:
        git(self.repo, "tag", "v1.3.0", self.commit_one)

        result = self.run_publisher(self.commit_one, api_failure=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("simulated API failure", result.stderr)
        self.assertEqual([call[0] for call in self.gh_calls()], ["api"])
        self.assertEqual(self.releases(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
