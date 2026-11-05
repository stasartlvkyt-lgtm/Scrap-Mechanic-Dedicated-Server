"""Inspect plaintext initial-sync packets without running or modifying the game.

Supported framing is corroborated by original NetObjUser/Receiver code and
the captured initial-world packet. Entity payload semantics are not inferred
from counts alone. No packet is transmitted by this tool.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sqlite3
import struct

LIMIT = 64 * 1024 * 1024

def aggregate_children(packet: bytes):
    if not packet or packet[0] != 29 or len(packet) > LIMIT:
        raise ValueError('invalid aggregate')
    offset = 1
    children = []
    while offset < len(packet):
        if len(packet) - offset < 4: raise ValueError('truncated aggregate length')
        size = int.from_bytes(packet[offset:offset+4], 'big')
        offset += 4
        if not size or size > len(packet) - offset: raise ValueError('invalid aggregate child size')
        children.append((offset, packet[offset:offset+size]))
        offset += size
        if len(children) > 1000000: raise ValueError('too many aggregate children')
    return children

def net_objects(packet: bytes):
    if len(packet) < 5 or len(packet) > LIMIT or packet[0] not in (21, 22):
        raise ValueError('expected ID21/22 NetObj packet')
    offset, previous, records = 5, b'', []
    while offset < len(packet):
        start = offset
        delta = bool(packet[offset] & 128)
        if delta:
            if not previous or len(previous) > 63: raise ValueError('delta without compatible previous record')
            mask_size = (len(previous) + 8) // 8
            if mask_size > len(packet) - offset: raise ValueError('truncated delta mask')
            mask = packet[offset:offset+mask_size]
            offset += mask_size
            data = bytearray(previous)
            for i in range(len(data)):
                if not mask[mask_size-1-i//8] & (1 << (i % 8)):
                    if offset >= len(packet): raise ValueError('truncated delta bytes')
                    data[i] = packet[offset]
                    offset += 1
            data = bytes(data)
        else:
            if len(packet) - offset < 2: raise ValueError('truncated record length')
            size = int.from_bytes(packet[offset:offset+2], 'big')
            if not 7 <= size <= 32767 or size > len(packet) - offset: raise ValueError('invalid NetObj length')
            data = packet[offset+2:offset+size]
            offset += size
        operation, kind = data[0] >> 5, data[0] & 31
        if operation not in (1, 2, 3, 5): raise ValueError('invalid NetObj operation')
        id_offset = 2 if operation == 1 else 1
        if len(data) < id_offset + 4: raise ValueError('truncated NetObj header')
        records.append({'operation': operation, 'type': kind,
                        'subtype': data[1] if operation == 1 else None,
                        'id': int.from_bytes(data[id_offset:id_offset+4], 'big'),
                        'payload': data[id_offset+4:], 'record_data': data,
                        'wire_offset': start, 'wire_size': offset-start,
                        'used_byte_delta': delta})
        previous = data if len(data) <= 63 else b''
        if len(records) > 1000000: raise ValueError('too many NetObj records')
    return int.from_bytes(packet[1:5], 'big'), records

def inspect_packet(packet, samples=5, records_out=None, path='packet', depth=0):
    if depth > 8: raise ValueError('aggregate nesting exceeds 8 levels')
    result = {'message_id': packet[0] if packet else None, 'bytes': len(packet),
              'sha256': hashlib.sha256(packet).hexdigest()}
    if packet and packet[0] == 29:
        children = aggregate_children(packet)
        result['children'] = [dict(offset=offset, **inspect_packet(child, samples, records_out, f'{path}.{i}', depth+1))
                              for i, (offset, child) in enumerate(children)]
    elif packet and packet[0] in (21, 22):
        tick, records = net_objects(packet)
        counts = Counter((x['operation'], x['type'], x['subtype']) for x in records)
        result.update(tick=tick, record_count=len(records), delta_record_count=sum(x['used_byte_delta'] for x in records),
                      record_types=[{'operation': key[0], 'type': key[1], 'subtype': key[2], 'count': value}
                                    for key, value in sorted(counts.items(), key=lambda item: str(item[0]))])
        shown = Counter()
        result['samples'] = []
        for record in records:
            key = (record['operation'], record['type'])
            if records_out is not None: records_out.append(dict(packet_path=path, **record))
            if shown[key] >= samples: continue
            shown[key] += 1
            result['samples'].append({key: value for key, value in record.items() if key not in ('payload', 'record_data')} |
                                     {'payload_bytes': len(record['payload']), 'payload_prefix_hex': record['payload'][:64].hex()})
    return result

def compare_save(records, path):
    tables = {0: 'RigidBody', 1: 'ChildShape', 2: 'Joint', 3: 'Controller'}
    db = sqlite3.connect(Path(path).resolve().as_uri() + '?mode=ro', uri=True)
    try:
        result = {}
        for kind, table in tables.items():
            saved = {row[0]: row[1] for row in db.execute(f'SELECT id,data FROM "{table}"')}
            creates = [x for x in records if x['operation'] == 1 and x['type'] == kind]
            ids = {x['id'] for x in creates}
            result[table] = {'save_count': len(saved), 'create_count': len(creates),
                             'create_ids_equal_save_ids': ids == set(saved),
                             'header_type_subtype_matches': sum(saved[x['id']][:2] == bytes((kind, x['subtype'])) for x in creates if x['id'] in saved)}
        return result
    finally: db.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packet', type=Path)
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--extract', type=Path, help='New directory for decoded record payloads and index')
    parser.add_argument('--save', type=Path, help='Read-only comparison with a candidate source save')
    args = parser.parse_args()
    if not 0 <= args.samples <= 100: parser.error('--samples must be 0..100')
    if args.packet.stat().st_size > LIMIT: raise ValueError('packet exceeds 64 MiB')
    records = []
    report = inspect_packet(args.packet.read_bytes(), args.samples, records)
    report['scope'] = 'Plain packet framing and NetObj record headers decoded; most entity payload fields remain opaque.'
    if args.save: report['source_save_comparison'] = compare_save(records, args.save)
    if args.extract:
        args.extract.mkdir(parents=True, exist_ok=False)
        index = []
        for i, record in enumerate(records):
            filename = f'{i:06d}-op{record["operation"]}-type{record["type"]}-id{record["id"]}.bin'
            (args.extract / filename).write_bytes(record['payload'])
            index.append({key: value for key, value in record.items() if key not in ('payload', 'record_data')} |
                         {'file': filename, 'payload_bytes': len(record['payload']), 'payload_sha256': hashlib.sha256(record['payload']).hexdigest()})
        (args.extract / 'index.json').write_text(json.dumps(index, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))

if __name__ == '__main__': main()
