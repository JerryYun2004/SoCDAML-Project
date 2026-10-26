"""Strict decoder for V2 lossless validation blocks, not a pass criterion.

Expand blocks into the legacy eight-word representation, then apply every
existing grid numerical, transport, interval and vector-completion check.
No partial, interleaved, mixed-format or noncanonical record is repaired.
"""
import base64
import binascii
import re
import struct

TAG = '[SOCDAML_SPATZ_GRID_V2]'
BLOCK_WORDS = 192
TILE_WORDS = 4096
RECORD = re.compile(re.escape(TAG) +
    r' WORDS64 cid=(\d+) kind=(A|B|C|HBM_C) index=(\d+) count=(\d+) ([A-Za-z0-9+/=]+)')


def expand_word_blocks(text):
    text = re.sub(r'\x1b\[[0-9;]*m', '', text)
    if '[SOCDAML_SPATZ_GRID_V1]' in text:
        raise ValueError('V1 evidence is not a V2 export.')
    result = []
    expected_index = {}
    for line in text.splitlines():
        if TAG + ' WORDS' not in line:
            result.append(line)
            continue
        match = RECORD.fullmatch(line)
        if not match:
            raise ValueError('Malformed or interleaved WORDS64 record.')
        cid, kind, index, count, payload = match.groups()
        cid, index, count = int(cid), int(index), int(count)
        key = (cid, kind)
        if not 0 <= cid < 16 or not 0 <= index < TILE_WORDS:
            raise ValueError('Invalid WORDS64 worker/index.')
        if index != expected_index.get(key, 0):
            raise ValueError('Missing/reordered/repeated WORDS64 block.')
        if count != min(BLOCK_WORDS, TILE_WORDS-index):
            raise ValueError('Invalid WORDS64 block length.')
        # Check text length first, bounding the decode allocation as well.
        if len(payload) != 4*((count*4+2)//3):
            raise ValueError('Truncated/oversized WORDS64 payload.')
        try:
            data = base64.b64decode(payload, validate=True)
        except binascii.Error as error:
            raise ValueError('Invalid WORDS64 Base64.') from error
        if len(data) != count*4 or base64.b64encode(data).decode('ascii') != payload:
            raise ValueError('Noncanonical WORDS64 encoding.')
        values = struct.unpack('<' + str(count) + 'I', data)
        for at in range(0, count, 8):
            result.append(f'{TAG} WORDS cid={cid} kind={kind} index={index+at} ' +
                          ' '.join(f'{word:08x}' for word in values[at:at+8]))
        expected_index[key] = index+count
    # A complete stream must contain all 64 complete tiles. Require this here
    # as well as in the numerical checker; a truncated final block cannot pass.
    wanted = {(cid, kind) for cid in range(16) for kind in ('A', 'B', 'C', 'HBM_C')}
    if set(expected_index) != wanted or any(n != TILE_WORDS for n in expected_index.values()):
        raise ValueError('Missing/incomplete WORDS64 tile export.')
    return '\n'.join(result)+'\n'
