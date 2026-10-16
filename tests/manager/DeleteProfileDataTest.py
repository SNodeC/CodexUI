# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
import fcntl
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid
from unittest.mock import patch

module = importlib.util.spec_from_file_location("deletion", Path(__file__).resolve().parents[2] / "src/manager/DeleteProfileData.py")
deletion = importlib.util.module_from_spec(module)
module.loader.exec_module(deletion)


class ProfileDeletionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.identity = str(uuid.uuid4())
        self.home = self.root / (".codex-" + self.identity[:8])
        self.home.mkdir()
        (self.home / "data").write_text("profile history")
        self.spec = dict(path=str(self.home), profile=self.identity, kind="home", others=[])

    def confirmed(self):
        self.spec["expected"] = deletion.inspect(self.spec)["token"]
        return self.spec

    def test_delete_legacy_home_preserves_symlink_targets(self):
        external = self.root / "shared"
        external.mkdir()
        (external / "AGENTS.md").write_text("keep")
        (self.home / "instructions").symlink_to(external, target_is_directory=True)
        deletion.delete(self.confirmed())
        self.assertFalse(self.home.exists())
        self.assertEqual((external / "AGENTS.md").read_text(), "keep")

    def test_default_home_and_ancestors_are_protected(self):
        for path in (Path.home(), Path.home() / ".codex", Path.home().parent, Path("/")):
            with self.subTest(path=path), self.assertRaises(ValueError):
                deletion.inspect(dict(self.spec, path=str(path)))

    def test_shared_and_overlapping_homes_are_protected(self):
        for other in (self.home, self.home.parent, self.home / "child"):
            with self.subTest(other=other), self.assertRaises(ValueError):
                deletion.inspect(dict(self.spec, others=[str(other)]))

    def test_symlink_root_is_protected(self):
        link = self.root / "alias"
        link.symlink_to(self.home)
        with self.assertRaises(ValueError):
            deletion.inspect(dict(self.spec, path=str(link)))

    def test_unmarked_arbitrary_home_is_protected(self):
        arbitrary = self.root / "existing"
        self.home.rename(arbitrary)
        with self.assertRaises(ValueError):
            deletion.inspect(dict(self.spec, path=str(arbitrary)))
        (arbitrary / deletion.MARKER).write_text(self.identity)
        deletion.delete(dict(self.spec, path=str(arbitrary), expected=deletion.inspect(dict(self.spec, path=str(arbitrary)))["token"]))
        self.assertFalse(arbitrary.exists())

    def test_marker_identity_and_file_type(self):
        marker = self.home / deletion.MARKER
        marker.write_text(str(uuid.uuid4()))
        with self.assertRaises(ValueError):
            deletion.inspect(self.spec)
        marker.unlink()
        os.mkfifo(marker)
        with self.assertRaises(ValueError):
            deletion.inspect(self.spec)
        marker.unlink()
        shared = self.root / "owner"
        shared.write_text(self.identity)
        os.link(shared, marker)
        with self.assertRaises(ValueError):
            deletion.inspect(self.spec)
        self.assertEqual(shared.read_text(), self.identity)

    def test_replaced_directory_requires_new_confirmation(self):
        spec = self.confirmed()
        self.home.rename(self.root / "old")
        self.home.mkdir()
        with self.assertRaises(ValueError):
            deletion.delete(spec)
        self.assertTrue((self.root / "old/data").exists())

    def test_runtime_lock_blocks_deletion(self):
        with (self.home / deletion.LOCK).open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                deletion.delete(self.confirmed())
        self.assertTrue((self.home / "data").exists())

    def test_live_app_server_blocks_deletion(self):
        # Same process argv/environment boundary, without a real Codex service.
        process = subprocess.Popen(["codex", "30"], executable="/bin/sleep", env=dict(os.environ, CODEX_HOME=str(self.home)))
        try:
            with self.assertRaises(ValueError):
                deletion.delete(self.confirmed())
            self.assertTrue((self.home / "data").exists())
        finally:
            process.terminate()
            process.wait()

    def test_partial_failure_retains_recoverable_marker(self):
        (self.home / "nested").mkdir()
        with patch.object(deletion.shutil, "rmtree", side_effect=OSError("fixture failure")) as mocked:
            mocked.avoids_symlink_attacks = True
            with self.assertRaises(OSError):
                deletion.delete(self.confirmed())
        self.assertEqual((self.home / deletion.MARKER).read_text().strip(), "deleting:" + self.identity)
        deletion.delete(self.confirmed())
        self.assertFalse(self.home.exists())

    def test_missing_home_is_idempotent(self):
        deletion.delete(self.confirmed())
        deletion.delete(self.confirmed())
        self.assertFalse(self.home.exists())

    def test_settings_scope(self):
        with self.assertRaises(ValueError):
            deletion.inspect(dict(self.spec, kind="settings"))
        settings = self.root / "profiles" / self.identity
        settings.mkdir(parents=True)
        spec = dict(self.spec, kind="settings", path=str(settings))
        spec["expected"] = deletion.inspect(spec)["token"]
        deletion.delete(spec)
        self.assertFalse(settings.exists())
        self.assertTrue(self.home.exists())


if __name__ == "__main__":
    unittest.main()
