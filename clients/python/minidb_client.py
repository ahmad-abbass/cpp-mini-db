"""Dependency-free synchronous client. One connection per thread; Python 3.9+."""
import json
import socket
from contextlib import contextmanager


class MiniDBError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def quote(value):
    """UTF-8 encode str inputs; preserve bytes. Responses always return bytes."""
    if isinstance(value, str):
        value = value.encode("utf-8")
    if not isinstance(value, bytes):
        raise TypeError("keys and values must be str or bytes")
    if len(value) > 1024 * 1024:
        raise ValueError("keys and values are limited to 1 MiB each")
    return '"' + ''.join(chr(byte) if 32 <= byte < 127 and byte not in (34, 92)
                         else f'\\x{byte:02x}' for byte in value) + '"'


class Client:
    def __init__(self, host="127.0.0.1", port=7070, timeout=10):
        self._socket = socket.create_connection((host, port), timeout=timeout)
        self._stream = self._socket.makefile("rb")
        self._batch = False
        self._batch_failed = False

    def close(self):
        self._stream.close()
        self._socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *ignored):
        self.close()

    def execute(self, command):
        if self._batch and self._batch_failed and command != "ROLLBACK":
            raise RuntimeError("batch was aborted; leave the batch context before continuing")
        if not isinstance(command, str) or '\n' in command or '\r' in command or '\0' in command:
            raise ValueError("execute accepts exactly one command line")
        encoded = command.encode("ascii")
        if len(encoded) > 9 * 1024 * 1024:
            raise ValueError("command exceeds 9 MiB")
        try:
            self._socket.sendall(encoded + b"\n")
            response = self._stream.readline(16 * 1024 * 1024 + 2)
            if not response.endswith(b"\n") or len(response) > 16 * 1024 * 1024 + 1:
                raise ConnectionError("connection closed or response exceeds limit")
            reply = json.loads(response)
        except (OSError, ValueError, ConnectionError):
            # After transport failure, request outcome may be unknown. Reconnect
            # explicitly; never silently replay a mutation or COMMIT.
            self.close()
            raise
        if not reply.get("ok"):
            if self._batch:
                self._batch_failed = True
            raise MiniDBError(reply.get("code", "unknown"), reply.get("error", "request failed"))
        return reply

    def ping(self):
        return self.execute("PING")["value"]

    def set(self, key, value):
        return self.execute(f"SET {quote(key)} {quote(value)}")

    def get(self, key):
        value = self.execute(f"GET {quote(key)}")["value"]
        return None if value is None else value.encode("latin1")

    def delete(self, key):
        return self.execute(f"DEL {quote(key)}")

    def scan(self, prefix=b"", limit=100):
        if isinstance(limit, bool) or not isinstance(limit, int) or not 0 <= limit <= 1000:
            raise ValueError("limit must be an integer in 0..1000")
        return [(key.encode("latin1"), value.encode("latin1"))
                for key, value in self.execute(f"SCAN {quote(prefix)} {limit}")["items"]]

    def stats(self):
        return self.execute("STATS")

    @contextmanager
    def batch(self):
        if self._batch:
            raise RuntimeError("nested batches are not supported")
        self.execute("BEGIN")
        self._batch = True
        self._batch_failed = False
        try:
            yield self
        except BaseException:
            try:
                self.execute("ROLLBACK")
            except (MiniDBError, OSError, ValueError):
                pass
            raise
        else:
            self.execute("COMMIT")
        finally:
            self._batch = False
            self._batch_failed = False
