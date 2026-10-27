#!/usr/bin/env python3
"""Snapshot superproject worktree files, not dirty submodule build products."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

repo = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()

def git(*args):
    return subprocess.check_output(["git", "-C", str(repo), *args])

head = git("rev-parse", "HEAD").decode().strip()
entries = git("ls-files", "--stage", "-z").split(b"\0")
paths = set()
gitlinks = {}
for entry in entries:
    if not entry:
        continue
    metadata, name = entry.split(b"\t", 1)
    mode, sha, stage = metadata.decode().split()
    name = os.fsdecode(name)
    if stage != "0":
        raise SystemExit("Resolve merge conflicts before creating a snapshot.")
    if mode == "160000":
        gitlinks[name] = sha
    else:
        paths.add(name)

# New checkpoint docs/scripts are included before their first commit. Other
# untracked source files must be staged explicitly to enter this snapshot.
for prefix in ("README.md", "docs/project", "tools/socdaml"):
    p = repo / prefix
    if p.is_file():
        paths.add(prefix)
    elif p.is_dir():
        paths.update(str(f.relative_to(repo)) for f in p.rglob("*") if f.is_file())

for name, sha in gitlinks.items():
    baseline = git("rev-parse", f"HEAD:{name}").decode().strip()
    if sha != baseline:
        raise SystemExit(f"Commit or resolve the staged submodule revision change: {name}")

manifest = {"head": head, "gitlinks": gitlinks, "files": [],
            "submodule_worktree_changes_included": False}
with tarfile.open(out / "source.tar.gz", "w:gz", dereference=False) as archive:
    for name in sorted(paths):
        p = repo / name
        if not p.exists() and not p.is_symlink():
            continue  # A worktree deletion must remain deleted in the copy.
        if p.is_dir():
            continue
        if not p.resolve().is_relative_to(repo):
            raise SystemExit(f"Source path escapes the repository: {name}")
        info = {"path": name, "mode": oct(p.lstat().st_mode & 0o777)}
        if p.is_symlink():
            info["symlink"] = os.readlink(p)
        else:
            info["sha256"] = hashlib.sha256(p.read_bytes()).hexdigest()
        archive.add(p, arcname=name, recursive=False)
        manifest["files"].append(info)

subprocess.run(["git", "-C", str(repo), "bundle", "create",
                str(out / "source.bundle"), "HEAD"], check=True)
(out / "source-head.txt").write_text(head + "\n")
(out / "source-status.txt").write_bytes(git("status", "--short"))
(out / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(out / "submodules.txt").write_bytes(git("submodule", "status", "--recursive"))
hashes = []
for name in ("source.tar.gz", "source.bundle", "source-manifest.json"):
    hashes.append(f"{hashlib.sha256((out/name).read_bytes()).hexdigest()}  {name}")
(out / "INPUT_SHA256SUMS").write_text("\n".join(hashes) + "\n")
print(f"Snapshotted {len(manifest['files'])} files at {head}.")
