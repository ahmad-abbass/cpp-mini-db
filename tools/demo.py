"""Run the storage/server/client/restart/inspection demo in a temporary database."""
import argparse
from contextlib import contextmanager
import json
import pathlib
import select
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'clients' / 'python'))
from minidb_client import Client  # noqa: E402
from inspect_wal import inspect  # noqa: E402


@contextmanager
def running_server(executable, directory):
    process = subprocess.Popen([str(executable), str(directory), '--port', '0'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        if not select.select([process.stdout], [], [], 10)[0]:
            raise RuntimeError('server readiness timeout')
        line = process.stdout.readline()
        if not line:
            raise RuntimeError('server did not become ready')
        yield json.loads(line)['port']
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            _, errors = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise RuntimeError('server shutdown timeout')
        if process.returncode != 0:
            raise RuntimeError('server failed: ' + errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=pathlib.Path, required=True)
    args = parser.parse_args()
    executable = args.server.resolve()
    with tempfile.TemporaryDirectory(prefix='minidb-demo-') as directory:
        with running_server(executable, directory) as port:
            with Client(port=port) as client:
                with client.batch():
                    client.set('project', 'MiniDB')
                    client.set('greeting', 'مرحبا')
                assert client.get('project') == b'MiniDB'
                assert client.get('greeting').decode('utf-8') == 'مرحبا'
                print(json.dumps({'phase': 'committed_batch', 'keys': client.stats()['keys']}))
                for _ in range(3):
                    client.set('project', 'MiniDB')
                before = client.stats()['log_bytes']
                client.execute('COMPACT')
                after = client.stats()['log_bytes']
                assert after < before
                print(json.dumps({'phase': 'compacted', 'before_bytes': before, 'after_bytes': after}))
        with running_server(executable, directory) as port:
            with Client(port=port) as client:
                assert client.get('project') == b'MiniDB'
                assert client.get('greeting').decode('utf-8') == 'مرحبا'
                assert client.stats()['keys'] == 2
                print(json.dumps({'phase': 'restarted', 'project': 'MiniDB', 'greeting': 'مرحبا'}))
        report = inspect(pathlib.Path(directory) / 'data.log')
        assert report['status'] == 'clean' and report['format_version'] == 2
        assert sum(frame['operations'] for frame in report['frames']) == 2
        print(json.dumps({'phase': 'independent_inspection', 'status': report['status'],
                          'format_version': report['format_version'], 'frames': len(report['frames'])}))


if __name__ == '__main__':
    main()
