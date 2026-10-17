"""Cross-check C++ log output with Python struct/zlib, including v1 upgrade."""
import base64
import json
import pathlib
import subprocess
import struct
import sys
import tempfile
import unittest
import zlib

CLI = pathlib.Path(sys.argv.pop(1)).resolve()
INSPECTOR = pathlib.Path(__file__).resolve().parents[1] / 'tools' / 'inspect_wal.py'


class InspectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='minidb-inspector-')
        self.directory = pathlib.Path(self.temp.name)
        self.log = self.directory / 'data.log'

    def tearDown(self):
        self.temp.cleanup()

    def cli(self, commands):
        result = subprocess.run([str(CLI), str(self.directory)], input=commands,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def inspect(self, *options, expected=0):
        before = self.log.read_bytes()
        result = subprocess.run([sys.executable, str(INSPECTOR), str(self.log), *options],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, expected, result.stderr + result.stdout)
        self.assertEqual(self.log.read_bytes(), before, 'inspector must never alter its input')
        return json.loads(result.stdout)

    def test_cpp_frames_and_private_default_output(self):
        self.cli('SET secret value\nBEGIN\nSET b two\nDEL secret\nCOMMIT\n')
        report = self.inspect()
        self.assertEqual(report['format_version'], 2)
        self.assertEqual([frame['operations'] for frame in report['frames']], [1, 2])
        self.assertNotIn('secret', json.dumps(report))
        shown = self.inspect('--show-values')
        self.assertEqual(base64.b64decode(shown['frames'][0]['values'][0]['key_base64']), b'secret')

    def test_incomplete_tail_is_reported_without_repair(self):
        self.cli('SET a one\nSET b two\n')
        self.log.write_bytes(self.log.read_bytes()[:-1])
        report = self.inspect(expected=2)
        self.assertEqual(report['status'], 'incomplete_tail')
        self.assertEqual(len(report['frames']), 1)

    def test_both_checksum_layers_are_checked(self):
        self.cli('SET a one\n')
        original = self.log.read_bytes()
        for offset in [9, len(original) - 1]:
            damaged = bytearray(original)
            damaged[offset] ^= 1
            self.log.write_bytes(damaged)
            self.assertIn('checksum', self.inspect(expected=1)['error'])

    def test_python_generated_legacy_log_opens_and_upgrades_in_cpp(self):
        payload = struct.pack('<IBII', 1, 1, 3, 5) + b'keyvalue'
        self.log.write_bytes(b'MDBWAL1\n' + struct.pack('<II', len(payload), zlib.crc32(payload)) + payload)
        self.assertEqual(self.inspect()['format_version'], 1)
        replies = self.cli('GET key\nCOMPACT\n')
        self.assertEqual(replies[0]['value'], 'value')
        self.assertEqual(self.inspect()['format_version'], 2)

    def test_malformed_operation_with_valid_checksums_is_rejected(self):
        payload = struct.pack('<IB', 1, 99)
        header = struct.pack('<II', len(payload), zlib.crc32(payload))
        self.log.write_bytes(b'MDBWAL2\n' + header + struct.pack('<I', zlib.crc32(header)) + payload)
        self.assertIn('operation tag', self.inspect(expected=1)['error'])

    def test_partial_file_header_is_an_error(self):
        self.log.write_bytes(b'MDB')
        self.assertIn('header', self.inspect(expected=1)['error'])


if __name__ == '__main__':
    unittest.main()
