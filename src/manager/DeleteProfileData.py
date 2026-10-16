# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
"""Guarded Linux profile cleanup, shared by local, SSH and manual execution."""
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys

MARKER = ".codexmanager-owner"
LOCK = ".codexmanager-runtime.lock"


def inspect(spec):
    path = Path(os.path.normpath(spec["path"]))
    profile = spec["profile"]
    if not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", profile):
        raise ValueError("Invalid profile identity")
    if not path.is_absolute() or str(path) != os.path.realpath(path):
        raise ValueError("Refusing a relative path or a symlink in the directory path")
    home = Path.home().resolve()
    if path in (home, home / ".codex") or path in home.parents or len(path.parts) < 3:
        raise ValueError("Protected directory: only registration removal is allowed")
    for other in spec.get("others", []):
        other = Path(os.path.realpath(other))
        if path == other or path in other.parents or other in path.parents:
            raise ValueError("Another registered profile shares or overlaps this home")
    kind = spec["kind"]
    if kind not in ("home", "settings"):
        raise ValueError("Invalid deletion target")
    if kind == "settings" and (path.name != profile or path.parent.name != "profiles"):
        raise ValueError("Invalid manager settings directory")
    token = "missing"
    ownership = "Absent"
    if path.exists():
        info = path.stat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            raise ValueError("Target must be a directory owned by the current user")
        if not shutil.rmtree.avoids_symlink_attacks:
            raise ValueError("This Python platform lacks safe directory-relative deletion")
        for line in Path("/proc/self/mountinfo").read_text().splitlines():
            mount = Path(re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), line.split()[4]))
            if path == mount or path in mount.parents:
                raise ValueError("Refusing a mounted directory or a tree containing mounts")
        if kind == "home":
            marker = path / MARKER
            if marker.is_symlink():
                raise ValueError("Ownership marker must not be a symlink")
            if marker.exists():
                info_marker = marker.stat()
                if not stat.S_ISREG(info_marker.st_mode) or info_marker.st_nlink != 1 or info_marker.st_uid != os.getuid() or info_marker.st_size > 128:
                    raise ValueError("Invalid ownership marker")
                if marker.read_text().strip() not in (profile, "deleting:" + profile):
                    raise ValueError("This home belongs to a different profile")
                ownership = "Manager-owned home"
            elif path.name == ".codex-" + profile[:8]:
                ownership = "Legacy generated home — confirm exclusive use"
            else:
                raise ValueError("Existing unmarked home: establish exclusive ownership manually, or remove only the registration")
        else:
            ownership = "Profile-specific manager settings"
        token = str(info.st_dev) + ":" + str(info.st_ino)
    return {"path": str(path), "token": token, "ownership": ownership}


def check_processes(spec):
    target = os.path.realpath(spec["path"])
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit() and int(entry.name) != os.getpid():
            try:
                if entry.stat().st_uid == os.getuid():
                    args = (entry / "cmdline").read_bytes().split(b"\0")
                    relevant = b"app-server" in args or any(os.path.basename(a).startswith(b"codex") for a in args[:2])
                    if relevant:
                        env = dict(item.split(b"=", 1) for item in (entry / "environ").read_bytes().split(b"\0") if b"=" in item)
                        home = os.fsdecode(env.get(b"CODEX_HOME", os.fsencode(Path.home() / ".codex")))
                        same_home = spec["kind"] == "home" and os.path.realpath(home) == target
                        same_ui = env.get(b"CODEXUI_PROFILE_ID", b"").decode() == spec["profile"]
                        if same_home or same_ui:
                            raise ValueError("Profile still in use by process " + entry.name + "; stop it before deleting")
            except (FileNotFoundError, ProcessLookupError):
                pass


def write_marker(root, value):
    fd = os.open(MARKER, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=root)
    with os.fdopen(fd, "w") as stream:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
            raise ValueError("Invalid ownership marker")
        os.ftruncate(fd, 0)
        stream.write(value + "\n")


def delete(spec):
    preview = inspect(spec)
    if preview["token"] != spec["expected"]:
        raise ValueError("Directory changed since the preview; inspect and confirm it again")
    if preview["token"] != "missing":
        path = Path(preview["path"])
        parent = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        root = -1
        lock = -1
        try:
            root = os.open(path.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            info = os.fstat(root)
            if str(info.st_dev) + ":" + str(info.st_ino) != spec["expected"]:
                raise ValueError("Directory identity changed; nothing deleted")
            lock = os.open(LOCK, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600, dir_fd=root)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            check_processes(spec)
            write_marker(root, "deleting:" + spec["profile"])
            # Directory-relative operations never follow links to external data.
            for name in os.listdir(root):
                if name not in (MARKER, LOCK):
                    item = os.stat(name, dir_fd=root, follow_symlinks=False)
                    if stat.S_ISDIR(item.st_mode):
                        shutil.rmtree(name, dir_fd=root)
                    else:
                        os.unlink(name, dir_fd=root)
            current = os.stat(path.name, dir_fd=parent, follow_symlinks=False)
            if (current.st_dev, current.st_ino) != (info.st_dev, info.st_ino):
                raise ValueError("Directory moved during deletion; registration retained")
            os.unlink(LOCK, dir_fd=root)
            os.unlink(MARKER, dir_fd=root)
            try:
                os.rmdir(path.name, dir_fd=parent)
            except OSError:
                write_marker(root, "deleting:" + spec["profile"])
                raise
        finally:
            if lock >= 0:
                os.close(lock)
            if root >= 0:
                os.close(root)
            os.close(parent)
    return {"deleted": preview["path"]}


def main():
    result = 0
    try:
        spec = json.loads(sys.argv[1])
        if spec["action"] == "manual":
            preview = inspect(spec)
            print(json.dumps(preview, indent=2))
            if input("Permanently delete all data? Type the exact directory path: ") != preview["path"]:
                raise ValueError("Confirmation did not match; nothing deleted")
            spec["expected"] = preview["token"]
            spec["action"] = "delete"
        response = inspect(spec) if spec["action"] == "inspect" else delete(spec)
        print(json.dumps(response))
    except (OSError, ValueError, KeyError) as error:
        print(json.dumps({"error": str(error), "note": "Registration retained. If deletion had started, some files may already be removed; retry after resolving the error."}))
        result = 1
    return result


if __name__ == "__main__":
    sys.exit(main())
