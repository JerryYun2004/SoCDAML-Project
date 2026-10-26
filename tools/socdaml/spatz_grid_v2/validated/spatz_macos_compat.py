"""Narrow host-type repair for the pinned Spatz model, with resumable migration.

Apple arm64 defines uint64_t as unsigned long long; unsigned long has equal
width but is a different C++ pointer type. Use uint64_t for the 70 unsigned
bit-container declarations passed to uint64_t* helpers. No casts, arithmetic,
instruction latency, architecture parameters or target GEMM code are changed.
"""
from pathlib import Path
import difflib
import hashlib
import json
import os
import re
import tempfile

REL = 'core/models/cpu/iss/include/isa_lib/vint.h'
BEFORE = '96a0484fb2771ffea1c85eda6e4513e18f7241676f3b1b7fc1c87952ac36063a'
AFTER = 'be1756b7dccd5498c89a4d71f45269a965592f13fe5076082e3858701ca02f01'
COUNT = 70
HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def patch(data):
    if digest(data) == AFTER: return data
    if digest(data) != BEFORE:
        raise ValueError('Unrecognized Spatz vint.h; preserve edits for review.')
    text, count = re.subn(r'\bunsigned long int\b', 'uint64_t', data.decode())
    result = text.encode()
    if count != COUNT or digest(result) != AFTER:
        raise ValueError('Spatz host-type repair did not match the reviewed patch.')
    return result


def atomic_write(path, data, mode=0o644):
    if path.is_symlink(): raise ValueError('Refusing to replace a symlink: '+str(path))
    descriptor, temporary = tempfile.mkstemp(prefix=path.name+'.pending-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def apply(profile, batch, inventory):
    """Only accept an unchanged profile, or our exact interrupted migration.

    Original native/source is never a patch target. The profile's original
    source identity remains intact; configured-source.json tracks this one
    reviewed compatibility change. Arbitrary edits anywhere still reject.
    """
    source = profile/'source'
    path = source/REL
    manifest_path = profile/'configured-source.json'
    expected = json.loads(manifest_path.read_text())
    actual = inventory(source)
    if REL not in expected or expected[REL].get('sha256') not in (BEFORE, AFTER):
        raise ValueError('Unrecognized configured Spatz source manifest.')
    if path.is_symlink() or any(parent.is_symlink() for parent in path.parents if parent != profile.parent):
        raise ValueError('Spatz compatibility target must not pass through symlinks.')
    current = path.read_bytes()
    patched = patch(current)
    if actual != expected:
        # A kill after the header rename but before the manifest rename is
        # recoverable only when every other path and file mode is unchanged.
        resumed = dict(expected)
        resumed[REL] = {**expected[REL], 'sha256': AFTER, 'size': len(patched)}
        if expected[REL]['sha256'] != BEFORE or actual != resumed:
            raise ValueError('Isolated Spatz source was edited; refusing to overwrite it.')
    backups = profile/'compat-backups'
    if backups.is_symlink(): raise ValueError('Compatibility backup directory must not be a symlink.')
    backups.mkdir(exist_ok=True)
    backup = backups/'vint.before-macos-u64.h'
    if backup.exists() or backup.is_symlink():
        if backup.is_symlink() or digest(backup.read_bytes()) != BEFORE:
            raise ValueError('Compatibility backup differs from the original model header.')
    elif digest(current) == BEFORE:
        atomic_write(backup, current)
    else:
        raise ValueError('Patched header has no verified original backup; preserve for review.')
    original = backup.read_bytes()
    if patch(original) != patched: raise ValueError('Compatibility patch/backup mismatch.')
    delta = ''.join(difflib.unified_diff(original.decode().splitlines(True), patched.decode().splitlines(True),
                     fromfile='a/'+REL, tofile='b/'+REL))
    if delta != (HERE/'presentation_spatz/vint_macos.diff').read_text():
        raise ValueError('Compatibility diff does not match this kit.')
    if current != patched:
        atomic_write(path, patched, actual[REL]['mode'])
        print('Applied Spatz macOS repair: 70 unsigned bit-container declarations now use uint64_t.', flush=True)
    else:
        print('Verified existing Spatz macOS uint64_t repair.', flush=True)
    updated = dict(expected)
    updated[REL] = {**expected[REL], 'sha256': AFTER, 'size': len(patched)}
    if inventory(source) != updated:
        raise ValueError('Unexpected source change during compatibility repair.')
    atomic_write(manifest_path, (json.dumps(updated, indent=2, sort_keys=True)+'\n').encode())
    record = {'name': 'spatz_macos_u64_v1', 'path': REL, 'declarations_changed': COUNT,
              'before_sha256': BEFORE, 'after_sha256': AFTER,
              'scope': 'C++ host bit-container types only; arithmetic, latency, target kernel and hardware config unchanged'}
    atomic_write(profile/'macos-compatibility.json', (json.dumps(record, indent=2)+'\n').encode())
    (batch/'vint.before-macos-u64.h').write_bytes(original)
    (batch/'vint.after-macos-u64.h').write_bytes(patched)
    (batch/'vint-macos.diff').write_text(delta)
    (batch/'macos-compatibility.json').write_text(json.dumps(record, indent=2)+'\n')
    return record
