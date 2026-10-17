"""Black-box CLI tests; no third-party Python dependencies."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

CLI = pathlib.Path(sys.argv.pop(1)).resolve()


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="minidb-cli-")
        self.directory = pathlib.Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_commands(self, commands, expected_code=0):
        process = subprocess.run(
            [str(CLI), str(self.directory)], input="\n".join(commands) + "\n",
            text=True, capture_output=True, timeout=20,
        )
        self.assertEqual(process.returncode, expected_code, process.stderr + process.stdout[-2000:])
        replies = [json.loads(line) for line in process.stdout.splitlines()]
        self.assertEqual(len(replies), len(commands))
        return replies

    def test_commands_and_reopen(self):
        replies = self.run_commands([
            'ping', 'SET "key with spaces" "hello \\"world\\""',
            'GET "key with spaces"', 'GET missing', 'EXISTS missing', 'SIZE',
            'SCAN "key" 1', 'STATS', 'SYNC', 'COMPACT',
        ])
        self.assertEqual(replies[0]["value"], "PONG")
        self.assertEqual(replies[2]["value"], 'hello "world"')
        self.assertIsNone(replies[3]["value"])
        self.assertFalse(replies[4]["value"])
        self.assertEqual(replies[5]["value"], 1)
        self.assertEqual(replies[6]["items"], [["key with spaces", 'hello "world"']])
        self.assertTrue(replies[7]["sync_on_write"])
        self.assertEqual(self.run_commands(['GET "key with spaces"'])[0]["value"], 'hello "world"')

    def test_all_byte_values_round_trip(self):
        escaped = ''.join(f'\\x{i:02x}' for i in range(256))
        replies = self.run_commands([f'SET "{escaped}" "{escaped}"', f'GET "{escaped}"'])
        self.assertEqual(replies[1]["value"].encode("latin1"), bytes(range(256)))

    def test_commit_rollback_and_disconnect(self):
        self.run_commands(['BEGIN', 'SET a one', 'SET b two', 'COMMIT'])
        self.run_commands(['BEGIN', 'DEL a', 'ROLLBACK'])
        self.run_commands(['BEGIN', 'SET ignored value'])
        replies = self.run_commands(['SIZE', 'GET a', 'GET ignored'])
        self.assertEqual(replies[0]["value"], 2)
        self.assertEqual(replies[1]["value"], "one")
        self.assertIsNone(replies[2]["value"])

    def test_errors_abort_pending_batch(self):
        replies = self.run_commands(['BEGIN', 'SET a one', 'GET a', 'COMMIT', 'SIZE'], 1)
        self.assertFalse(replies[2]["ok"])
        self.assertFalse(replies[3]["ok"])
        self.assertEqual(replies[4]["value"], 0)
        for command in ['SET only_key', 'GET a b', 'SCAN "" -1', 'SCAN "" 1001',
                        'SCAN "" 1x', '"PING"junk', 'SET a "unterminated',
                        'SET a \\q', 'SET a \\x0g', 'SET a \\x0', 'UNKNOWN']:
            with self.subTest(command=command):
                reply = self.run_commands([command], 1)[0]
                self.assertEqual(reply["code"], "invalid_command")

    def test_empty_values_delete_and_clear(self):
        replies = self.run_commands(['SET "" ""', 'GET ""', 'DEL ""', 'GET ""',
                                    'SET a one', 'CLEAR', 'SIZE'])
        self.assertEqual(replies[1]["value"], "")
        self.assertIsNone(replies[3]["value"])
        self.assertEqual(replies[-1]["value"], 0)

    def test_last_line_without_newline(self):
        process = subprocess.run([str(CLI), str(self.directory)], input='PING',
                                 text=True, capture_output=True, timeout=10)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(json.loads(process.stdout)["value"], "PONG")


if __name__ == '__main__':
    unittest.main()
