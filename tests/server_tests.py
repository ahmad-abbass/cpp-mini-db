"""Real-process TCP, crash/restart and Python-client integration tests."""
import concurrent.futures
import json
import pathlib
import select
import socket
import subprocess
import sys
import tempfile
import unittest

SERVER = pathlib.Path(sys.argv.pop(1)).resolve()
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "clients" / "python"))
from minidb_client import Client, MiniDBError  # noqa: E402


class RunningServer:
    def __init__(self, directory, *options):
        self.errors = tempfile.TemporaryFile(mode="w+")
        self.process = subprocess.Popen(
            [str(SERVER), str(directory), '--port', '0', *options],
            stdout=subprocess.PIPE, stderr=self.errors, text=True,
        )
        if not select.select([self.process.stdout], [], [], 10)[0]:
            self.stop(kill=True)
            raise RuntimeError("server startup timed out")
        line = self.process.stdout.readline()
        if not line:
            self.process.wait(timeout=5)
            self.errors.seek(0)
            error = self.errors.read()
            self.process.stdout.close()
            self.errors.close()
            raise RuntimeError("server failed to start: " + error)
        self.port = json.loads(line)["port"]

    def client(self):
        return Client(port=self.port, timeout=10)

    def stop(self, kill=False):
        if self.process.poll() is None:
            self.process.kill() if kill else self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise AssertionError("server did not stop gracefully")
        finally:
            self.process.stdout.close()
            self.errors.seek(0)
            error = self.errors.read()
            self.errors.close()
        if not kill and self.process.returncode != 0:
            raise AssertionError(f"server exited {self.process.returncode}: {error}")


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="minidb-server-")
        self.directory = pathlib.Path(self.temporary.name)
        self.server = RunningServer(self.directory)

    def tearDown(self):
        if self.server:
            self.server.stop()
        self.temporary.cleanup()

    def test_python_client_binary_unicode_and_batches(self):
        with self.server.client() as client:
            self.assertEqual(client.ping(), "PONG")
            binary = bytes(range(256))
            client.set(binary, binary)
            self.assertEqual(client.get(binary), binary)
            client.set("greeting", "مرحبا")
            self.assertEqual(client.get("greeting").decode("utf-8"), "مرحبا")
            with client.batch():
                client.set("user:2", "two")
                client.set("user:1", "one")
            self.assertEqual(client.scan("user:"), [(b"user:1", b"one"), (b"user:2", b"two")])
            self.assertTrue(client.stats()["sync_on_write"])

    def test_client_rolls_back_on_exception(self):
        with self.server.client() as client:
            with self.assertRaisesRegex(RuntimeError, "cancel"):
                with client.batch():
                    client.set("cancelled", "value")
                    raise RuntimeError("cancel")
            self.assertIsNone(client.get("cancelled"))

    def test_error_caught_inside_batch_cannot_commit_later_writes(self):
        with self.server.client() as client:
            with self.assertRaises(RuntimeError):
                with client.batch():
                    client.set("first", "queued")
                    try:
                        client.execute("UNKNOWN")
                    except MiniDBError:
                        pass
                    client.set("second", "must not be committed")
            self.assertIsNone(client.get("first"))
            self.assertIsNone(client.get("second"))

    def test_fragmented_and_pipelined_frames(self):
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=5) as connection:
            with connection.makefile("rb") as stream:
                connection.sendall(b'SET "frag')
                connection.sendall(b'mented" "value"\nGET fragmented\nPING\n')
                self.assertTrue(json.loads(stream.readline())["ok"])
                self.assertEqual(json.loads(stream.readline())["value"], "value")
                self.assertEqual(json.loads(stream.readline())["value"], "PONG")

    def test_concurrent_independent_clients(self):
        def writer(worker):
            with self.server.client() as client:
                for index in range(25):
                    key = f"worker:{worker}:{index:03}"
                    client.set(key, str(index))
                    self.assertEqual(client.get(key), str(index).encode())
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            list(executor.map(writer, range(8)))
        with self.server.client() as client:
            self.assertEqual(client.stats()["keys"], 200)

    def test_acknowledged_write_survives_sigkill_pending_batch_does_not(self):
        with self.server.client() as client:
            client.set("committed", "yes")
            client.execute("BEGIN")
            client.set("uncommitted", "no")
            self.server.stop(kill=True)
            self.server = RunningServer(self.directory)
        with self.server.client() as client:
            self.assertEqual(client.get("committed"), b"yes")
            self.assertIsNone(client.get("uncommitted"))

    def test_disconnect_discards_batch(self):
        with self.server.client() as client:
            client.execute("BEGIN")
            client.set("pending", "value")
        with self.server.client() as client:
            self.assertIsNone(client.get("pending"))

    def test_graceful_shutdown_with_idle_and_queued_clients(self):
        connections = [socket.create_connection(("127.0.0.1", self.server.port), timeout=5) for _ in range(12)]
        try:
            self.server.stop()
            self.server = None
        finally:
            for connection in connections:
                connection.close()

    def test_idle_timeout(self):
        self.server.stop()
        self.server = RunningServer(self.directory, '--idle-timeout', '1')
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=4) as connection:
            self.assertEqual(connection.recv(1), b"")

    def test_second_process_cannot_open_same_directory(self):
        process = subprocess.run([str(SERVER), str(self.directory), '--port', '0'],
                                 capture_output=True, text=True, timeout=10)
        self.assertEqual(process.returncode, 1)
        self.assertIn("locked", process.stderr)
        with self.server.client() as client:
            self.assertEqual(client.ping(), "PONG")

    def test_oversized_request_is_rejected_and_connection_closed(self):
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=10) as connection:
            with connection.makefile("rb") as stream:
                connection.sendall(b"x" * (9 * 1024 * 1024 + 1))
                reply = json.loads(stream.readline())
                self.assertEqual(reply["code"], "request_too_large")
                self.assertEqual(stream.read(1), b"")

    def test_bounded_connection_queue(self):
        self.server.stop()
        self.server = RunningServer(self.directory, '--workers', '1')
        connections = [socket.create_connection(("127.0.0.1", self.server.port), timeout=5) for _ in range(9)]
        try:
            readable = select.select(connections, [], [], 5)[0]
            self.assertTrue(readable, "excess clients should get a busy response")
            reply = json.loads(readable[0].recv(4096))
            self.assertEqual(reply["code"], "busy")
        finally:
            for connection in connections:
                connection.close()

    def test_json_response_budget_leaves_connection_usable(self):
        with self.server.client() as client:
            for i in range(3):
                client.set(f'large:{i}', b'\xff' * (1024 * 1024))
            with self.assertRaises(MiniDBError) as error:
                client.scan('large:', 3)
            self.assertEqual(error.exception.code, 'invalid_command')
            self.assertEqual(client.ping(), 'PONG')


if __name__ == '__main__':
    unittest.main()
