"""Own a temporary server and measure verified SET/GET pairs over TCP."""
import argparse
import concurrent.futures
import json
import pathlib
import platform
import select
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "clients" / "python"))
from minidb_client import Client  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=pathlib.Path, required=True)
    parser.add_argument('--clients', type=int, default=4, choices=range(1, 33))
    parser.add_argument('--operations-per-client', type=int, default=250)
    parser.add_argument('--value-bytes', type=int, default=128)
    parser.add_argument('--buffered', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.operations_per_client <= 1000000 or not 1 <= args.value_bytes <= 1024 * 1024:
        parser.error('operations must be 1..1000000 and value bytes 1..1048576')
    with tempfile.TemporaryDirectory(prefix='minidb-network-bench-') as directory:
        command = [str(args.server.resolve()), directory, '--port', '0', '--workers', str(args.clients)]
        if args.buffered:
            command.append('--buffered')
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            if not select.select([process.stdout], [], [], 10)[0]:
                raise RuntimeError('server startup timed out')
            ready = process.stdout.readline()
            if not ready:
                raise RuntimeError('server startup failed')
            port = json.loads(ready)['port']
            value = b'v' * args.value_bytes

            def worker(worker_id):
                samples = []
                with Client(port=port, timeout=30) as client:
                    for index in range(args.operations_per_client):
                        key = f'worker:{worker_id}:{index}'
                        start = time.perf_counter_ns()
                        client.set(key, value)
                        if client.get(key) != value:
                            raise RuntimeError('value verification failed')
                        samples.append((time.perf_counter_ns() - start) / 1000)
                return samples

            start = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as executor:
                samples = [sample for group in executor.map(worker, range(args.clients)) for sample in group]
            elapsed = time.perf_counter() - start
            samples.sort()
            with Client(port=port) as client:
                if client.stats()['keys'] != len(samples):
                    raise RuntimeError('key count verification failed')
                client.execute('SYNC')
            print(json.dumps({
                'workload': 'tcp_set_get_pair', 'python': platform.python_version(),
                'clients': args.clients, 'pairs': len(samples), 'value_bytes': args.value_bytes,
                'sync_on_write': not args.buffered, 'wall_seconds': elapsed,
                'pairs_per_second': len(samples) / elapsed,
                'p50_us': statistics.median(samples),
                'p95_us': samples[int(0.95 * (len(samples) - 1))],
                'p99_us': samples[int(0.99 * (len(samples) - 1))], 'verified': True,
            }, indent=2))
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                _, errors = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise RuntimeError('server shutdown timed out')
            if process.returncode != 0:
                raise RuntimeError(f'server failed: {errors}')


if __name__ == '__main__':
    main()
