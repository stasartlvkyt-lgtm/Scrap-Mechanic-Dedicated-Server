"""Verify a synthetic original-game codec fixture using independent python-lz4."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import lz4.block

def verify(path):
    raw = Path(path).read_bytes()
    if len(raw) < 16 or raw[:8] != b'SDSCOD01': raise ValueError('invalid fixture header')
    source_size, compressed_size = struct.unpack_from('<II', raw, 8)
    if not 1 <= source_size <= 64 * 1024 * 1024 or not 1 <= compressed_size <= 64 * 1024 * 1024:
        raise ValueError('fixture sizes out of bounds')
    if len(raw) != 16 + source_size + compressed_size: raise ValueError('truncated fixture or trailing bytes')
    source = raw[16:16+source_size]
    compressed = raw[16+source_size:]
    if source[0] != compressed[0]: raise ValueError('first message byte changed')
    decoded = b'' if source_size == 1 else lz4.block.decompress(compressed[1:], uncompressed_size=source_size - 1)
    if decoded != source[1:]: raise ValueError('reference decode differs from original source')
    encoded = source[:1] if source_size == 1 else source[:1] + lz4.block.compress(source[1:], store_size=False)
    return {'source_size': source_size, 'compressed_size': compressed_size, 'reference_decode_matches': True,
            'reference_encode_matches': encoded == compressed, 'fixture_sha256': hashlib.sha256(raw).hexdigest(),
            'scope': 'synthetic codec fixture; no handshake or gameplay compatibility claim'}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    print(json.dumps(verify(parser.parse_args().fixture), indent=2))
