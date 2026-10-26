#!/usr/bin/env python3
"""Build an isolated presentation-shaped model and verify K=64 FP32 Spatz.

Never edits native/source or its install, cleanup sources, or previous results.
The native build is deliberately a separate stage from historical benchmarking.
"""
from collections import Counter
from pathlib import Path
import argparse
import difflib
import fcntl
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import spatz_macos_compat
import spatz_cycle_compat

HERE = Path(__file__).resolve().parent
ASSETS = HERE/'presentation_spatz'
KIT = HERE.parent
TAG = '[SOCDAML_PRESENTATION_SPATZ_V1]'
PROFILE = 'presentation-spatz-v1'
ARCH = Path('soft_hier/flex_cluster/flex_cluster_arch.py')
SDK = Path('soft_hier/flex_cluster_sdk')
BASE_ARCH_SHA = '21de1ba2cc27b061fc5fc16823d34ffbf596b582889e03b797ebfb306d94a5a2'
SOURCE_ITEMS = ('CMakeLists.txt', 'Makefile', 'bin', 'core', 'pulp', 'gapy', 'soft_hier')
IGNORED = {'.git', '__pycache__', '.DS_Store'}
FLAGS = ['-march=rv32imafdv_zfh', '-mabi=ilp32d', '-mcmodel=medlow', '-O3',
         '-g', '-fno-builtin', '-fno-tree-vectorize', '-fno-common',
         '-ffunction-sections', '-fno-lto']


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1024*1024), b''): h.update(data)
    return h.hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True)+'\n')


def architecture(original):
    if hashlib.sha256(original.encode()).hexdigest() != BASE_ARCH_SHA:
        raise ValueError('Native architecture changed; preserve it for review, do not reset it.')
    values = dict(cluster_tcdm_bank_width=64, cluster_tcdm_bank_nb=64,
                  spatz_attaced_core_list=[0], spatz_num_vlsu_port=4,
                  spatz_num_function_unit=4, hbm_chan_placement=[4, 4, 0, 0])
    for key, value in values.items():
        original, count = re.subn(r'(self\.'+key+r'\s*=)[^\n]+', r'\g<1> '+repr(value), original)
        if count != 1: raise ValueError('Unexpected architecture field: '+key)
    original += '\n        self.spatz_vlsu_port_width = 64\n'
    return original


def tree_manifest(root, items=SOURCE_ITEMS):
    """Hash source contents, executable bits and safe internal symlinks.

    No traversal through links and no Git mutation. Build/install directories
    are excluded only at the snapshot root; nested source directories survive.
    """
    root = root.resolve()
    result = {}
    def visit(path):
        if path.name in IGNORED or path.suffix in ('.pyc', '.pyo'): return
        rel = path.relative_to(root).as_posix()
        if path.is_symlink():
            target = os.readlink(path)
            if os.path.isabs(target) or not path.resolve().is_relative_to(root):
                raise ValueError('Snapshot needs a relative internal symlink: '+str(path))
            result[rel] = {'link': target}
        elif path.is_dir():
            for child in sorted(path.iterdir()): visit(child)
        elif path.is_file():
            result[rel] = {'sha256': sha(path), 'size': path.stat().st_size,
                           'mode': path.stat().st_mode & 0o777}
        else:
            raise ValueError('Missing or special source file: '+str(path))
    for item in items: visit(root/item)
    return result


def copy_snapshot(source, destination, manifest):
    for rel, meta in manifest.items():
        src, dst = source/rel, destination/rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if 'link' in meta: dst.symlink_to(meta['link'])
        else: shutil.copy2(src, dst)
    if tree_manifest(destination) != manifest:
        raise ValueError('Source snapshot verification failed.')


def command(cmd, cwd, env, log, timeout=3600):
    print('Running: '+' '.join(map(str, cmd[:4])), flush=True)
    started = time.monotonic()
    with log.open('a') as stream:
        stream.write('\nCOMMAND '+repr(list(map(str, cmd)))+'\n'); stream.flush()
        process = subprocess.Popen(list(map(str, cmd)), cwd=cwd, env=env,
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while True:
                try:
                    code = process.wait(timeout=min(30, max(1, timeout-(time.monotonic()-started))))
                    break
                except subprocess.TimeoutExpired:
                    elapsed = time.monotonic()-started
                    print(f'  {elapsed:.0f}s host elapsed; log: {log}', flush=True)
                    if elapsed >= timeout: raise ValueError('Command timed out; partial log retained.')
                    if log.stat().st_size > 256*1024*1024: raise ValueError('Trace exceeded 256 MiB; stopped.')
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL); process.wait()
    if code:
        with log.open(errors='replace') as stream:
            from collections import deque
            print(''.join(deque(stream, maxlen=20)))
        raise ValueError(f'Command exited {code}: {log}')
    return {'exit_code': code, 'host_elapsed_seconds': time.monotonic()-started}


def profile_env(root, profile):
    source = profile/'source'
    env = dict(os.environ)
    # Source modules bootstrap the clean install. No old installed model path
    # remains on PATH/PYTHONPATH, even if env.sh selected the scalar workspace.
    path = [p for p in env['PATH'].split(os.pathsep)
            if not Path(p).is_relative_to(root/'source')]
    env['PATH'] = os.pathsep.join(map(str, [profile/'install/bin', source/'gapy/bin', *path]))
    env['PYTHONPATH'] = os.pathsep.join(map(str, [profile/'install/python', source/'core/models',
                                      source/'soft_hier/flex_cluster_utilities']))
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env.pop('SOCDAML_APP', None)
    return env


def create_profile(root, batch):
    source = root/'source'
    original = (source/ARCH).read_text()
    config = architecture(original)
    manifest = tree_manifest(source)
    identity = {'source_manifest': manifest,
                'config_sha256': hashlib.sha256(config.encode()).hexdigest()}
    profiles = root/'profiles'; profiles.mkdir(exist_ok=True)
    profile = profiles/PROFILE
    if profiles.is_symlink() or profile.is_symlink():
        raise ValueError('Profile directories must not be symlinks.')
    if profile.exists():
        record = profile/'identity.json'
        if not record.is_file() or json.loads(record.read_text()) != identity:
            raise ValueError('Existing Spatz profile has different inputs; preserve it for review: '+str(profile))
        print('Checking the existing isolated profile for safe reuse.', flush=True)
    else:
        needed = sum(x.get('size', 0) for x in manifest.values())
        if shutil.disk_usage(profiles).free < needed + 3*1024**3:
            raise ValueError('Need source snapshot space plus at least 3 GiB free for the isolated build.')
        temporary = Path(tempfile.mkdtemp(prefix=PROFILE+'-preparing-', dir=profiles))
        print(f'Snapshotting {len(manifest)} source entries ({needed/1024**2:.0f} MiB).', flush=True)
        copy_snapshot(source, temporary/'source', manifest)
        if tree_manifest(source) != manifest: raise ValueError('Native source changed while snapshotting.')
        (temporary/'source'/ARCH).write_text(config)
        write(temporary/'identity.json', identity)
        # Generate before renaming: config.py writes no absolute build paths.
        env = profile_env(root, temporary)
        command(['gmake', 'config'], temporary/'source', env, batch/'build.log', timeout=120)
        write(temporary/'configured-source.json', tree_manifest(temporary/'source'))
        temporary.rename(profile)
    spatz_cycle_compat.recover_interrupted(profile, tree_manifest)
    spatz_macos_compat.apply(profile, batch, tree_manifest)
    spatz_cycle_compat.apply(profile, batch, tree_manifest)
    shutil.copy2(profile/'identity.json', batch/'profile-identity.json')
    shutil.copy2(profile/'configured-source.json', batch/'configured-source.json')
    (batch/'architecture.py').write_text(config)
    (batch/'architecture.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True),
        config.splitlines(True), fromfile='scalar/flex_cluster_arch.py', tofile='spatz/flex_cluster_arch.py')))
    return profile, manifest


def build_simulator(root, profile, batch, env):
    source = profile/'source'; build = profile/'build/gvsoc'; install = profile/'install'
    snd = Path(subprocess.check_output(['brew', '--prefix', 'libsndfile'], text=True).strip())
    rate = Path(subprocess.check_output(['brew', '--prefix', 'libsamplerate'], text=True).strip())
    systemc = Path(env['SYSTEMC_HOME'])
    for p in (snd/'include/sndfile.hh', snd/'lib/libsndfile.dylib', rate/'include/samplerate.h',
              rate/'lib/libsamplerate.dylib', systemc/'lib64/libsystemc.dylib', Path(env['SOCDAML_DRAMSYS_LIBRARY'])):
        if not p.is_file(): raise ValueError('Missing existing native dependency: '+str(p))
    flags = f'-L{snd}/lib -L{rate}/lib'
    cmd = ['cmake', '-S', source, '-B', build, '-G', 'Ninja',
           '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_CXX_STANDARD=17',
           '-DCMAKE_OSX_SYSROOT='+env['SDKROOT'], '-DCMAKE_OSX_ARCHITECTURES=arm64',
           f'-DCMAKE_CXX_FLAGS=-DSC_USE_PTHREADS -isystem {snd}/include -isystem {rate}/include',
           '-DCMAKE_SHARED_LINKER_FLAGS='+flags, '-DCMAKE_MODULE_LINKER_FLAGS='+flags,
           '-DCMAKE_EXE_LINKER_FLAGS='+flags, f'-DCMAKE_PREFIX_PATH={systemc};{snd};{rate}',
           f'-DSNDFILE_LIB:FILEPATH={snd}/lib/libsndfile.dylib',
           f'-DSAMPLERATE_LIB:FILEPATH={rate}/lib/libsamplerate.dylib',
           '-DCMAKE_INSTALL_PREFIX='+str(install),
           f'-DCMAKE_INSTALL_RPATH={install}/lib;{systemc}/lib64', '-DCMAKE_SKIP_INSTALL_RPATH=FALSE',
           f'-DGVSOC_MODULES={source}/core/models;{source}/pulp',
           '-DGVSOC_TARGETS=pulp.chips.flex_cluster.flex_cluster', '-DBUILD_OPTIMIZED=ON', '-DBUILD_DEBUG=ON']
    command(cmd, source, env, batch/'build.log')
    configs = sorted((build/'gvsoc/configs').glob('*.config'))
    config_text = '\n'.join(path.read_text() for path in configs)
    write(batch/'model-parameter-audit.json', audit_model_config(config_text))
    for path in configs:
        dst = batch/'model-configs'/path.name; dst.parent.mkdir(exist_ok=True)
        shutil.copy2(path, dst)
    command(['cmake', '--build', build, '--parallel', env.get('SOCDAML_JOBS', '2')], source, env, batch/'build.log')
    command(['cmake', '--install', build], source, env, batch/'build.log')
    for path in (source/ARCH, source/'pulp/pulp/chips/flex_cluster/flex_cluster_arch.py',
                 install/'generators/pulp/chips/flex_cluster/flex_cluster_arch.py'):
        if not path.is_file() or sha(path) != sha(batch/'architecture.py'):
            raise ValueError('Source/copied/installed Spatz architecture mismatch: '+str(path))
    artifacts = [p for p in install.rglob('*') if p.is_file() and '__pycache__' not in p.parts
                 and p.suffix not in ('.pyc', '.pyo')]
    write(batch/'installed-artifacts.json', {str(p.relative_to(profile)): sha(p) for p in sorted(artifacts)})
    write(batch/'dependencies.json', {str(p): sha(p) for p in
          (systemc/'lib64/libsystemc.dylib', Path(env['SOCDAML_DRAMSYS_LIBRARY']))})


def audit_model_config(text):
    result = {}
    for name, expected in [('INC_SPATZ', 1), ('SPATZ_FPU', 4), ('SPATZ_VLSU', 4), ('SPATZ_VLSU_BW', 64)]:
        values = re.findall(r'CONFIG_GVSOC_ISS_'+name+r'=(\d+)\b', text)
        if not values or {int(x) for x in values} != {expected}:
            raise ValueError('Generated model compile definitions mismatch: '+name)
        result[name] = expected
    return {'status': 'passed', 'compile_definitions': result}


def build_probe(profile, batch, env):
    source = profile/'source'; sdk = source/SDK
    compiler = shutil.which('riscv32-unknown-elf-gcc', path=env['PATH'])
    objdump = shutil.which('riscv32-unknown-elf-objdump', path=env['PATH'])
    if not compiler or not objdump: raise ValueError('Existing native RISC-V GCC/objdump required.')
    for p in ASSETS.iterdir():
        if p.is_file(): shutil.copy2(p, batch/p.name)
    obj = batch/'gemm_fp32.o'; elf = batch/'spatz-probe.elf'
    command([compiler, *FLAGS, '-c', batch/'gemm_fp32.S', '-o', obj], source, env, batch/'build.log', 120)
    command([compiler, *FLAGS, '-I'+str(sdk/'runtime/include'), '-I'+str(sdk/'app_example/include'),
             '-nostartfiles', '-Wl,--gc-sections', '-Wl,-Map='+str(batch/'spatz-probe.map'),
             '-T'+str(sdk/'runtime/flex_memory.ld'), '-o', elf,
             sdk/'runtime/flex_start.s', batch/'probe.c', obj], source, env, batch/'build.log', 120)
    dump = subprocess.check_output([objdump, '-d', str(elf)], text=True, env=env, timeout=60)
    (batch/'spatz-probe.dump').write_text(dump)
    body = re.search(r'<socdaml_spatz_gemm>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)', dump, re.S)
    if not body or any(op not in body[1] for op in ('vsetvli', 'vle32.v', 'vfmul.vf', 'vfmacc.vf', 'vse32.v', 'fence')):
        raise ValueError('Linked vector kernel missing expected instructions.')
    write(batch/'compiler.json', {'path': compiler, 'sha256': sha(compiler), 'flags': FLAGS,
          'version': subprocess.check_output([compiler, '--version'], text=True, timeout=30),
          'elf_sha256': sha(elf), 'dump_sha256': sha(batch/'spatz-probe.dump'), 'object_sha256': sha(obj)})
    return elf


def oracle():
    a = [[1+(r*3+k*5+(r^k)) % 16 for k in range(64)] for r in range(64)]
    b = [[1+(k*7+c*11+(k^c)) % 16 for c in range(64)] for k in range(64)]
    # Every product/sum is an exactly representable integer in binary32.
    return [struct.unpack('<I', struct.pack('<f', sum(a[r][k]*b[k][c] for k in range(64))))[0]
            for r in range(64) for c in range(64)]


def verify_log(text):
    clean = re.sub(r'\x1b\[[0-9;]*m', '', text)
    if clean.count(TAG+' BEGIN M=64 K=64 N=64 dtype=fp32 cluster=0 core=0') != 1 or clean.count(TAG+' END') != 1:
        raise ValueError('Missing or repeated presentation probe boundaries.')
    cycles = re.findall(re.escape(TAG)+r' CYCLES start=(\d+) stop=(\d+) status=(\d+) vl=(\d+) vtype=([0-9a-fA-F]+)', clean)
    if len(cycles) != 1: raise ValueError('Exactly one cycle record required.')
    begin_pos, cycle_pos, end_pos = (clean.index(TAG+' BEGIN'), clean.index(TAG+' CYCLES'), clean.index(TAG+' END'))
    if not begin_pos < cycle_pos < end_pos:
        raise ValueError('Probe records are out of order.')
    start, stop, status, vl = map(int, cycles[0][:4]); vtype = int(cycles[0][4], 16)
    if status or vl not in (1, 2, 4, 8, 16, 32, 64) or vtype != 0xd2:
        raise ValueError('Invalid kernel status or e32/m4 vector configuration.')
    # Still verify numerical and execution evidence when an unreadable cycle
    # counter invalidates timing. Never substitute trace time for a bad probe.
    cycle_valid = 0 < start < stop < 2**32
    rows = re.findall(re.escape(TAG)+r' C index=(\d+) ((?:[0-9a-fA-F]{8}(?: |\r?$)){8})', clean, re.M)
    actual = []
    if len(rows) != 512: raise ValueError('Expected 512 complete output rows, covering every C word.')
    for match in re.finditer(re.escape(TAG)+r' C index=', clean):
        if not cycle_pos < match.start() < end_pos:
            raise ValueError('Output validation was printed inside/outside the wrong probe boundary.')
    for index, words in rows:
        if int(index) != len(actual): raise ValueError('Missing/reordered/duplicated output index.')
        actual.extend(int(x, 16) for x in words.split())
    expected = oracle()
    mismatch = next((i for i, (x, y) in enumerate(zip(actual, expected)) if x != y), None)
    if mismatch is not None:
        raise ValueError(f'FP32 output mismatch at {mismatch}: {actual[mismatch]:08x} != {expected[mismatch]:08x}')
    # Static disassembly is insufficient: require the running model to have
    # issued the exact expected number of vector operations for this workload.
    operations = []
    for line in clean.splitlines():
        if '[Spatz] Finished' not in line: continue
        match = re.search(r'\[Spatz\] Finished\s*:\s*(\d+) ns ---> (\d+) ns.*Instruction = (\S+)', line)
        if not match: raise ValueError('Unrecognized vector timing trace.')
        if not re.search(r'/cluster_0/(?:pe0|fp_ss0)/spatz/trace', line):
            raise ValueError('Vector work executed on an unexpected cluster/core.')
        operations.append((int(match[1]), int(match[2]), match[3]))
    outside = clean[:begin_pos]+clean[cycle_pos:]
    if '[Spatz] Finished' in outside:
        raise ValueError('Vector execution appears outside the kernel trace window.')
    chunks = 64//vl
    counts = Counter(op for _, _, op in operations)
    expected_counts = {'vle32.v': 16*chunks*64, 'vfmul.vf': 64*chunks,
                       'vfmacc.vf': 64*chunks*63, 'vse32.v': 64*chunks}
    if counts != expected_counts: raise ValueError(f'Runtime vector instruction count mismatch: {dict(counts)} vs {expected_counts}')
    if any((cycle_valid and a < start) or b < a or b >= 2**32 for a, b, _ in operations):
        raise ValueError('Vector trace lies outside the valid core-cycle window.')
    completion = max(b for _, b, _ in operations)
    timing_rejection = ('invalid_cycle_interval' if not cycle_valid else
                        'vector_completion_after_end_probe' if completion > stop else None)
    return {'execution_path_validated': True, 'all_4096_outputs_bit_exact': True,
            'output_sha256': hashlib.sha256(struct.pack('<4096I', *actual)).hexdigest(),
            'workload': {'M': 64, 'K': 64, 'N': 64, 'dtype': 'FP32', 'active_clusters': 1,
                         'compute_core': 0, 'mathematical_macs': 262144},
            'runtime_vector_instructions': dict(counts), 'vl': vl,
            'start_cycle': start, 'stop_cycle': stop, 'cycle_counter_valid': cycle_valid,
            'entry_to_return_cycles': stop-start if cycle_valid else None,
            'last_vector_completion_cycle': completion,
            'vector_trace_envelope_cycles': completion-min(a for a, _, _ in operations),
            'vector_trace_is_call_interval': False,
            'compute_timing_accepted': timing_rejection is None,
            'timing_rejection': timing_rejection,
            'timing_scope': 'Core 0 call/return including vector setup and fence; excludes input handoff, validation and final synchronization.',
            'historical_presentation_reproduction_verified': False,
            'application_speedup_claim': False}


def run(root):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise ValueError('Run through native.sh on the existing Apple Silicon Mac workspace.')
    if not (root/'.socdaml-native-v1').is_file(): raise ValueError('Unrecognized native workspace.')
    batch = Path(tempfile.mkdtemp(prefix='presentation-spatz-', dir=root/'logs'))
    print('Presentation execution evidence: '+str(batch), flush=True)
    state = {'status': 'running', 'protocol': PROFILE, 'historical_presentation_reproduction_verified': False}
    before = None
    try:
        for name in ('flex_cluster.py', 'cluster_unit.py', 'ctrl_registers.cpp'):
            reference = HERE/'evidence/compute-model-source'/name
            actual = root/'source/soft_hier/flex_cluster'/name
            if not actual.is_file() or sha(actual) != sha(reference):
                raise ValueError('Machine/clock/timer source changed; preserve for review: '+str(actual))
        profile, before = create_profile(root, batch)
        state['profile'] = str(profile)
        state['macos_compatibility'] = json.loads((profile/'macos-compatibility.json').read_text())
        state['cycle_compatibility'] = json.loads((profile/'cycle-compatibility.json').read_text())
        env = profile_env(root, profile)
        for rel in ('presentation_spatz.py', 'spatz_macos_compat.py', 'spatz_cycle_compat.py',
                    'presentation_spatz/gemm_fp32.S', 'presentation_spatz/probe.c',
                    'presentation_spatz/vint_macos.diff', 'presentation_spatz/cycle_clock.diff'):
            target = batch/'checking-tools'/rel; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(HERE/rel, target)
        # Check assembler support before spending time rebuilding the model.
        elf = build_probe(profile, batch, env)
        build_simulator(root, profile, batch, env)
        if tree_manifest(profile/'source') != json.loads((profile/'configured-source.json').read_text()):
            raise ValueError('Model build unexpectedly changed source files.')
        cmd = [str(profile/'install/bin/gvsoc'), '--target=pulp.chips.flex_cluster.flex_cluster',
               '--binary', str(elf), 'run', '--trace=spatz', '--trace-level=debug']
        state['simulation_command'] = cmd
        state['simulation'] = command(cmd, profile/'source', env, batch/'simulation.log', timeout=1800)
        state.update(verify_log((batch/'simulation.log').read_text(errors='replace')))
        if not state['compute_timing_accepted']:
            raise ValueError('Numerical/vector proof passed; timing rejected: '+state['timing_rejection']+'. Preserve the archive for review.')
        state['status'] = 'passed'
        print('PASS: K=64 FP32 Spatz executed; all 4096 outputs match; completion lies within the cycle interval.', flush=True)
    except BaseException as error:
        state['status'] = 'failed'; state['error'] = str(error)
        raise
    finally:
        if before is not None:
            try: state['original_native_source_unchanged'] = tree_manifest(root/'source') == before
            except (ValueError, OSError) as error:
                state['original_native_source_unchanged'] = False
                state['preservation_check_error'] = str(error)
            if not state['original_native_source_unchanged']:
                state['status'] = 'failed'; state['error'] = 'Original source changed during this run; review concurrent activity.'
        state['artifacts'] = {str(p.relative_to(batch)): sha(p) for p in sorted(batch.rglob('*'))
                              if p.is_file() and p.name != 'report.json'}
        write(batch/'report.json', state)
        archive = batch.with_suffix('.tar.gz')
        with tarfile.open(archive, 'w:gz') as output: output.add(batch, arcname=batch.name)
        print(f'Evidence archive ({state["status"]}): {archive}', flush=True)
    if state['status'] != 'passed': raise ValueError(state['error'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native_directory', type=Path)
    args = parser.parse_args()
    root = args.native_directory.expanduser().resolve()
    try:
        (root/'logs').mkdir(exist_ok=True)
        with (root/'logs/row-reproduction.lock').open('a') as lock:
            try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError: raise ValueError('Another reproduction runner is active.')
            run(root)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit('STOP: '+str(error))
