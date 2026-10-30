"""Offline checks for restoring a manifest-only reference checkout."""
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import dump_aosp_headers as dump


class RestoreSnapshotTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.dest = self.root / 'api-23_android-6.0.0_r1/frameworks/native'
        self.dest.mkdir(parents=True)
        self.contents = {'A.h': b'first\n', 'a.h': b'second\n'}
        blob = io.BytesIO()
        with tarfile.open(fileobj=blob, mode='w:gz') as tar:
            for name, data in self.contents.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                tar.addfile(member, io.BytesIO(data))
        self.payload = blob.getvalue()
        self.manifest = {
            'status': 'complete', 'api': 23, 'tag': 'android-6.0.0_r1',
            'repository': 'platform/frameworks/native', 'commit': '1' * 40,
            'archives': [{'path': 'include', 'status': 'downloaded',
                          'archive_sha256': dump.digest(self.payload)}],
            'files': [
                {'path': 'include/' + name,
                 'stored_path': 'include/' + (name if name == 'A.h' else 'a.aosp-case-test.h'),
                 'bytes': len(data), 'sha256': dump.digest(data)}
                for name, data in self.contents.items()],
        }
        self.manifest_path = self.dest / 'SNAPSHOT.json'
        self.manifest_path.write_text(json.dumps(self.manifest))

    def fetch(self):
        return dump.fetch_repo(self.root, 23, '6.0.0', 'platform/frameworks/native', ['include'])

    def test_manifest_only_restore_and_cached_reuse(self):
        original = self.manifest_path.read_bytes()
        with patch.object(dump, 'get', return_value=self.payload) as get:
            self.assertEqual(self.fetch()[-1], 'restored')
            get.assert_called_once_with(dump.BASE + 'platform/frameworks/native/+archive/' +
                                        '1' * 40 + '/include.tar.gz')
        self.assertEqual(self.manifest_path.read_bytes(), original)
        for item in self.manifest['files']:
            self.assertEqual((self.dest / item['stored_path']).read_bytes(),
                             self.contents[Path(item['path']).name])
        with patch.object(dump, 'get', side_effect=AssertionError('unexpected network')):
            self.assertEqual(self.fetch()[-1], 'cached')

    def test_existing_corruption_is_not_overwritten(self):
        path = self.dest / 'include/A.h'
        path.parent.mkdir()
        path.write_bytes(b'corrupt')
        with patch.object(dump, 'get', side_effect=AssertionError('unexpected network')):
            with self.assertRaisesRegex(RuntimeError, 'checksum/size mismatch'):
                self.fetch()
        self.assertEqual(path.read_bytes(), b'corrupt')

    def test_archive_hash_failure(self):
        with patch.object(dump, 'get', return_value=b'invalid'):
            with self.assertRaisesRegex(RuntimeError, 'archive checksum'):
                self.fetch()
        self.assertFalse((self.dest / 'include/A.h').exists())

    def test_file_hash_failure(self):
        self.manifest['files'][0]['sha256'] = '0' * 64
        with patch.object(dump, 'get', return_value=self.payload):
            with self.assertRaisesRegex(RuntimeError, 'checksum/size mismatch'):
                dump.restore_snapshot(self.dest, self.manifest)
        self.assertFalse((self.dest / 'include/A.h').exists())

    def test_missing_member_is_reported(self):
        self.manifest['files'][0]['path'] = 'include/missing.h'
        with patch.object(dump, 'get', return_value=self.payload):
            with self.assertRaisesRegex(RuntimeError, 'files absent'):
                dump.restore_snapshot(self.dest, self.manifest)

    def test_reject_escaping_storage_path(self):
        self.manifest['files'][0]['stored_path'] = '../escape.h'
        with self.assertRaisesRegex(RuntimeError, 'unsafe archive path'):
            dump.restore_snapshot(self.dest, self.manifest)

    def test_changed_policy_requires_new_directory(self):
        with patch.object(dump, 'get', side_effect=AssertionError('unexpected network')):
            with self.assertRaisesRegex(RuntimeError, 'snapshot policy differs'):
                dump.fetch_repo(self.root, 23, '6.0.0', 'platform/frameworks/native', ['other'])


if __name__ == '__main__':
    unittest.main()
