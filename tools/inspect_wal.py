"""Read-only, independent Python validator for MiniDB WAL v1 and v2.

Inspect a stopped database or a consistent copy. Output metadata by default;
--show-values explicitly includes base64-encoded keys/values. Never repairs files.
Exit codes: 0 clean, 1 corruption/I/O error, 2 incomplete final frame.
"""
import argparse
import base64
import json
import pathlib
import struct
import zlib

MAX_FRAME = 16 * 1024 * 1024
MAX_FIELD = 1024 * 1024


class CorruptionError(ValueError):
    pass


def decode(payload, show_values=False):
    at = 0

    def number():
        nonlocal at
        if len(payload) - at < 4:
            raise CorruptionError('truncated operation length')
        value, = struct.unpack_from('<I', payload, at)
        at += 4
        return value

    def field(length):
        nonlocal at
        if length > MAX_FIELD or length > len(payload) - at:
            raise CorruptionError('invalid operation field length')
        start = at
        at += length
        return base64.b64encode(payload[start:at]).decode('ascii') if show_values else None

    count = number()
    if not 1 <= count <= 100000 or count > len(payload) - at:
        raise CorruptionError('invalid operation count')
    counts = {'set': 0, 'erase': 0, 'clear': 0}
    operations = []
    for _ in range(count):
        if at == len(payload):
            raise CorruptionError('missing operation tag')
        tag = payload[at]
        at += 1
        if tag == 1:
            key_length, value_length = number(), number()
            key, value = field(key_length), field(value_length)
            counts['set'] += 1
            if show_values:
                operations.append({'kind': 'set', 'key_base64': key, 'value_base64': value})
        elif tag == 2:
            key = field(number())
            counts['erase'] += 1
            if show_values:
                operations.append({'kind': 'erase', 'key_base64': key})
        elif tag == 3:
            counts['clear'] += 1
            if show_values:
                operations.append({'kind': 'clear'})
        else:
            raise CorruptionError('unknown operation tag')
    if at != len(payload):
        raise CorruptionError('trailing operation bytes')
    result = {'operations': count, 'counts': counts}
    if show_values:
        result['values'] = operations
    return result


def inspect(path, show_values=False):
    with pathlib.Path(path).open('rb') as stream:
        header = stream.read(8)
        if header not in (b'MDBWAL1\n', b'MDBWAL2\n'):
            raise CorruptionError('invalid or incomplete database header')
        version = int(chr(header[6]))
        prefix_size = 8 if version == 1 else 12
        report = {'status': 'clean', 'format_version': version, 'frames': [],
                  'valid_bytes': 8, 'tail_bytes': 0}
        while True:
            offset = stream.tell()
            prefix = stream.read(prefix_size)
            if not prefix:
                return report
            if len(prefix) != prefix_size:
                report.update(status='incomplete_tail', tail_bytes=len(prefix))
                return report
            if version == 2 and zlib.crc32(prefix[:8]) != struct.unpack_from('<I', prefix, 8)[0]:
                raise CorruptionError(f'frame header checksum mismatch at byte {offset}')
            length, checksum = struct.unpack_from('<II', prefix)
            if not 5 <= length <= MAX_FRAME:
                raise CorruptionError(f'invalid frame length at byte {offset}')
            payload = stream.read(length)
            if len(payload) != length:
                report.update(status='incomplete_tail', tail_bytes=prefix_size + len(payload))
                return report
            if zlib.crc32(payload) != checksum:
                raise CorruptionError(f'payload checksum mismatch at byte {offset}')
            frame = {'offset': offset, 'payload_bytes': length, **decode(payload, show_values)}
            report['frames'].append(frame)
            report['valid_bytes'] = stream.tell()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=pathlib.Path)
    parser.add_argument('--show-values', action='store_true')
    args = parser.parse_args()
    try:
        report = inspect(args.log, args.show_values)
    except (OSError, CorruptionError) as error:
        print(json.dumps({'status': 'error', 'error': str(error)}))
        return 1
    print(json.dumps(report, indent=2))
    return 2 if report['status'] == 'incomplete_tail' else 0


if __name__ == '__main__':
    raise SystemExit(main())
