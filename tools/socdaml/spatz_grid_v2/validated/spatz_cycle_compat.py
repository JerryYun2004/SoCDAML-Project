"""Make the pinned cycle CSR read the existing simulator clock.

The original declares a read-only cycle register without a read callback.
Its default storage stays zero. This adds one callback registration using the
already-existing Csr::mcycle_access implementation (clock.get_cycles()).
It changes no instruction handler, latency, scoreboard or target kernel.
"""
import difflib
import json
from pathlib import Path
from spatz_macos_compat import atomic_write, digest

HERE = Path(__file__).resolve().parent
REL = 'core/models/cpu/iss/src/csr.cpp'
BEFORE = '73d0cc960c85a41b24e537e95915412cabad1ea19de9a5d019451ff712a32c6a'
AFTER = '065c1fa055af48608ff1694dddbadd4576bb661e646ff39bf055315ff52807c8'
ANCHOR = '    this->declare_csr(&this->cycle,  "cycle",   0xC00); this->cycle.write_illegal = true;\n'
ADDITION = ('    // SoCDAML timing probe: expose the same clock used by Spatz timing_insn.\n'
            '    this->cycle.register_callback(std::bind(&Csr::mcycle_access, this, std::placeholders::_1, std::placeholders::_2));\n')


def patch(data):
    if digest(data) == AFTER: return data
    if digest(data) != BEFORE or data.count(ANCHOR.encode()) != 1:
        raise ValueError('Unrecognized cycle CSR source; preserve it for review.')
    result = data.replace(ANCHOR.encode(), (ANCHOR+ADDITION).encode())
    if digest(result) != AFTER: raise ValueError('Cycle CSR patch differs from the reviewed change.')
    return result


def paths(profile):
    source = profile/'source'; target = source/REL
    if any(p.is_symlink() for p in (target, *target.parents)):
        raise ValueError('Cycle CSR patch target must not pass through symlinks.')
    backups = profile/'compat-backups'
    if backups.is_symlink(): raise ValueError('Compatibility backups must not be symlinked.')
    return source, target, profile/'configured-source.json', backups/'csr.before-cycle-clock.cpp'


def recover_interrupted(profile, inventory):
    """Finish our exact interrupted rename before the older uint64_t guard.

    A pending uint64_t repair remains the responsibility of that repair's own
    guard. Timer migration starts only after the uint64_t migration completes.
    """
    source, target, manifest_path, backup = paths(profile)
    data = target.read_bytes(); patch(data)
    manifest = json.loads(manifest_path.read_text())
    if manifest.get(REL, {}).get('sha256') not in (BEFORE, AFTER):
        raise ValueError('Unrecognized cycle CSR manifest entry.')
    if digest(data) == AFTER and manifest[REL]['sha256'] == BEFORE:
        updated = dict(manifest)
        updated[REL] = {**manifest[REL], 'sha256': AFTER, 'size': len(data)}
        if inventory(source) != updated or not backup.is_file() or backup.is_symlink() or digest(backup.read_bytes()) != BEFORE:
            raise ValueError('Interrupted cycle patch contains unrecognized changes; preserve for review.')
        atomic_write(manifest_path, (json.dumps(updated, indent=2, sort_keys=True)+'\n').encode())


def apply(profile, batch, inventory):
    source, target, manifest_path, backup = paths(profile)
    manifest = json.loads(manifest_path.read_text())
    actual = inventory(source)
    if actual != manifest: raise ValueError('Profile edited before cycle repair; refusing to overwrite it.')
    current = target.read_bytes(); result = patch(current)
    backup.parent.mkdir(exist_ok=True)
    if backup.exists() or backup.is_symlink():
        if backup.is_symlink() or digest(backup.read_bytes()) != BEFORE:
            raise ValueError('Cycle CSR original backup is changed.')
    elif digest(current) == BEFORE:
        atomic_write(backup, current)
    else:
        raise ValueError('Patched cycle CSR has no verified original backup.')
    original = backup.read_bytes()
    if patch(original) != result: raise ValueError('Cycle CSR patch/backup mismatch.')
    delta = ''.join(difflib.unified_diff(original.decode().splitlines(True), result.decode().splitlines(True),
                                       fromfile='a/'+REL, tofile='b/'+REL))
    if delta != (HERE/'presentation_spatz/cycle_clock.diff').read_text():
        raise ValueError('Cycle CSR patch does not match the kit diff.')
    if current != result:
        atomic_write(target, result, actual[REL]['mode'])
        print('Connected the cycle CSR to the existing simulator clock.', flush=True)
    else:
        print('Verified the existing cycle CSR clock callback.', flush=True)
    updated = dict(manifest)
    updated[REL] = {**manifest[REL], 'sha256': AFTER, 'size': len(result)}
    if inventory(source) != updated: raise ValueError('Unexpected source change during cycle repair.')
    atomic_write(manifest_path, (json.dumps(updated, indent=2, sort_keys=True)+'\n').encode())
    record = {'name': 'spatz_cycle_clock_v1', 'path': REL, 'before_sha256': BEFORE,
              'after_sha256': AFTER, 'counter': 'cycle CSR 0xC00',
              'source': 'Csr::mcycle_access -> iss.top.clock.get_cycles()',
              'change': 'One callback registration; no instruction timing equations changed'}
    (batch/'csr.before-cycle-clock.cpp').write_bytes(original)
    (batch/'csr.after-cycle-clock.cpp').write_bytes(result)
    (batch/'cycle-clock.diff').write_text(delta)
    atomic_write(profile/'cycle-compatibility.json', (json.dumps(record, indent=2)+'\n').encode())
    (batch/'cycle-compatibility.json').write_text(json.dumps(record, indent=2)+'\n')
    return record
