#!/usr/bin/env python3
"""Bounded native trace of an existing failed grid ELF; never a performance run."""
from collections import Counter, deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import errno
import fcntl
import json
import os
import platform
import pty
import re
import selectors
import shutil
import signal
import subprocess
import tarfile
import tempfile
import time
import presentation_spatz as p

# The pinned engine uses regcomp(..., 0): POSIX basic regular expressions.
# Bare '+' is a literal there. Use [0-9][0-9]* to cover clusters 0..15.
# Instruction tracing filled the previous 64 MiB capture during normal A init.
# Keep event/decoder traces; the target ELF and model remain unchanged.
TRACES = (
    'cluster_0/pe[012]/decoder$',
    'cluster_[0-9][0-9]*/pe[012]/exception$',
    'cluster_[0-9][0-9]*/cluster_registers/trace$',
    'cluster_[0-9][0-9]*/idma/fe/trace$',
    'cluster_0/pe0/spatz/trace$',
    '/chip/ctrl_registers/trace$',
)


def safe_file(root, name):
    path = root / name
    if Path(name).is_absolute() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('Artifact path escapes its directory: ' + name)
    if not path.is_file():
        raise ValueError('Missing evidence file: ' + str(path))
    return path


def verify_hashes(root, hashes):
    for name, digest in hashes.items():
        if p.sha(safe_file(root, name)) != digest:
            raise ValueError('Evidence/model hash changed: ' + name)


def sample_launcher(group, destination):
    """Optional macOS stack evidence from our own simulator process group only."""
    if platform.system() != 'Darwin':
        return {'status': 'not_macos'}
    try:
        rows = subprocess.check_output(['/bin/ps', '-axo', 'pid=,pgid=,comm='],
                                       text=True, timeout=3).splitlines()
        for row in rows:
            fields = row.strip().split(None, 2)
            if len(fields) == 3 and fields[1] == str(group) and Path(fields[2]).name.startswith('gvsoc_launcher'):
                result = subprocess.run(['/usr/bin/sample', fields[0], '1', '-file', str(destination)],
                                        capture_output=True, text=True, timeout=5)
                return {'status': 'captured' if result.returncode == 0 else 'failed',
                        'pid': int(fields[0]), 'exit_code': result.returncode,
                        'message': (result.stdout + result.stderr)[-2000:]}
        return {'status': 'launcher_not_found_in_own_process_group'}
    except (OSError, subprocess.SubprocessError) as error:
        return {'status': 'unavailable', 'reason': str(error)}


def capture(command, cwd, env, log, seconds=60, byte_limit=64*1024*1024, sample_after=30):
    """PTY prevents libc stdout buffering from hiding the last few checkpoints.

    Trace volume and wall time are bounded independently. A deliberately
    stopped capture is evidence collection, never a passing benchmark. The
    process group is killed even if a wrapper exits before its children.
    """
    started = time.monotonic()
    master, slave = pty.openpty()
    process = None
    sampler = None; sample = None
    result = {'timing_accepted': False, 'byte_limit': byte_limit, 'seconds_limit': seconds}
    try:
        process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env,
            stdin=subprocess.DEVNULL, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave); slave = -1
        os.set_blocking(master, False)
        total = 0; heartbeat = started + 10
        with selectors.DefaultSelector() as selector, Path(log).open('wb') as output:
            selector.register(master, selectors.EVENT_READ)
            while True:
                now = time.monotonic()
                if now - started >= seconds:
                    result['stop_reason'] = 'time_limit'; break
                if total >= byte_limit:
                    result['stop_reason'] = 'trace_limit'; break
                if sample is None and now-started >= sample_after:
                    # Keep draining the PTY during sampling. The old sampler
                    # ran after draining stopped and observed our own output
                    # backpressure, not a useful simulator stall stack.
                    sampler = ThreadPoolExecutor(max_workers=1)
                    sample = sampler.submit(sample_launcher, process.pid,
                                            Path(log).with_name('launcher-sample.txt'))
                if now >= heartbeat:
                    print(f'  Diagnostic: {now-started:.0f}s, {total/1024**2:.1f} MiB captured.', flush=True)
                    heartbeat = now + 10
                events = selector.select(min(0.1, max(0, seconds-(now-started))))
                if not events:
                    continue
                try:
                    data = os.read(master, min(65536, byte_limit-total))
                except OSError as error:
                    if error.errno == errno.EIO: data = b''
                    elif error.errno == errno.EAGAIN: continue
                    else: raise
                if not data:
                    result['stop_reason'] = 'process_closed_output'; break
                output.write(data); total += len(data)
            output.flush()
        result['bytes_captured'] = total
    finally:
        if process is not None:
            # SIGKILL is intentional: this disposable process has only bounded
            # trace output and no user changes to save. Avoid orphan simulators.
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait(timeout=5)
        if slave != -1: os.close(slave)
        os.close(master)
        if sampler is not None: sampler.shutdown(wait=True)
    result['launcher_sample'] = sample.result() if sample is not None else {'status': 'capture_ended_before_sample'}
    result['exit_code'] = process.returncode
    result['host_elapsed_seconds'] = time.monotonic() - started
    return result


def summarize(log):
    """Last observed instructions are evidence, not proof of the stall cause."""
    counts = Counter(); last = {}; recent = deque(maxlen=80)
    markers = []; warnings = deque(maxlen=30); instructions = 0
    dma_counts = Counter(); dma_tail = {}; barrier_tail = deque(maxlen=40)
    init_rows = {'A': set(), 'B': set(), 'C': set()}; vector_count = 0
    ansi = re.compile(r'\x1b\[[0-9;]*m')
    insn = re.compile(r'(/chip/cluster_\d+/pe\d+/insn)\s*\]\s+.*?\b[MSU?]\s+([0-9a-fA-F]+)\s+(\S+)')
    with Path(log).open(errors='replace') as source:
        for raw in source:
            line = ansi.sub('', raw).strip()
            match = insn.search(line)
            if match:
                instructions += 1; counts[match[3]] += 1
                last[match[1]] = {'pc': '0x'+match[2], 'instruction': match[3], 'line': line}
                recent.append(line)
            dma = re.search(r'/chip/cluster_(\d+)/idma/fe/trace\s*\].*?\[iDMA\] Finished.*?Txn (\d+) = \{type: (\S+), src: (0x[0-9a-f]+), dst: (0x[0-9a-f]+), size: (0x[0-9a-f]+)', line)
            if dma:
                cid, txn, kind, src, dst, size = dma.groups()
                dma_counts[cid] += 1; dma_tail[cid] = line
                if cid == '0' and kind == '1D' and int(src, 16) == 0x20000:
                    for name, base, stride, rows in [('A',0xc0000400,256,256),('B',0xc0010400,1024,64),('C',0xc0020400,1024,256)]:
                        offset = int(dst,16)-base
                        if 0 <= offset < stride*rows and offset % stride == 0 and int(size,16) == stride:
                            init_rows[name].add(offset//stride)
            if re.search(r'/chip/(?:cluster_\d+/cluster_registers|ctrl_registers)/trace\s*\]', line):
                barrier_tail.append(line)
            if '[Spatz] Finished' in line: vector_count += 1
            if '[SOCDAML_SPATZ_GRID_V1]' in line:
                if len(markers) < 50: markers.append(line)
            if re.search(r'\b(illegal|exception|fatal|error|invalid)\b', line, re.I): warnings.append(line)
    return {'timing_accepted': False, 'instruction_records': instructions,
            'instruction_counts': dict(counts), 'last_observed_per_core': last,
            'last_80_instructions': list(recent), 'protocol_markers_first_50': markers,
            'warning_lines_last_30': list(warnings),
            'dma_completion_counts_by_cluster': dict(dma_counts),
            'last_dma_completion_by_cluster': dma_tail,
            'initialization_rows_completed': {name:len(rows) for name,rows in init_rows.items()},
            'expected_initialization_rows': {'A':256, 'B':64, 'C':256},
            'last_40_barrier_events': list(barrier_tail),
            'recorded_vector_completions': vector_count,
            'interpretation': 'Partial diagnostic trace only. Last recorded PC need not be the stalled instruction; blocked instructions may not retire.'}


def run(root, prior):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or not (root/'.socdaml-native-v1').is_file():
        raise ValueError('Use the existing native Apple Silicon workspace.')
    if prior == root or not prior.is_relative_to(root/'logs'):
        raise ValueError('Pass the existing failed presentation-grid evidence directory under native/logs.')
    report = json.loads(safe_file(prior, 'report.json').read_text())
    if report.get('protocol') != 'source_scheduled_spatz_grid_v1' or report.get('status') != 'failed':
        raise ValueError('Expected a failed Spatz grid report.')
    verify_hashes(prior, report['artifacts'])
    profile = root/'profiles'/p.PROFILE
    configured = json.loads(safe_file(prior, 'configured-source.json').read_text())
    installed = json.loads(safe_file(prior, 'installed-artifacts.json').read_text())
    dependencies = json.loads(safe_file(prior, 'dependencies.json').read_text())
    if p.tree_manifest(profile/'source') != configured:
        raise ValueError('Isolated model source changed since the failed run; preserve it for review.')
    verify_hashes(profile, installed)
    for path, digest in dependencies.items():
        if p.sha(path) != digest: raise ValueError('Native dependency changed: ' + path)
    original = p.tree_manifest(root/'source')
    batch = Path(tempfile.mkdtemp(prefix='presentation-grid-diagnose-', dir=root/'logs'))
    state = {'status': 'running', 'timing_accepted': False, 'prior_batch': str(prior),
             'prior_report_sha256': p.sha(prior/'report.json'), 'trace_patterns': TRACES,
             'diagnostic_version': 2, 'trace_mode': 'events_posix_basic'}
    print('Diagnostic evidence: ' + str(batch), flush=True)
    try:
        for name in ('grid_direct.elf', 'grid_direct.dump', 'grid_direct.map', 'grid.c', 'grid.ld',
                     'gemm_fp32.S', 'architecture.py', 'compiler.json', 'installed-artifacts.json',
                     'dependencies.json', 'configured-source.json'):
            shutil.copy2(safe_file(prior, name), batch/name)
        shutil.copy2(prior/'report.json', batch/'prior-report.json')
        shutil.copy2(__file__, batch/'grid_diagnose.py')
        state['elf_sha256'] = p.sha(batch/'grid_direct.elf')
        command = [str(profile/'install/bin/gvsoc'), '--target=pulp.chips.flex_cluster.flex_cluster',
                   '--binary', str(batch/'grid_direct.elf'), 'run', '--trace-level=debug',
                   *('--trace='+pattern for pattern in TRACES)]
        state['command'] = command
        print('Tracing the unchanged direct ELF for at most 60s / 64 MiB; no build.', flush=True)
        state['capture'] = capture(command, profile/'source', p.profile_env(root, profile), batch/'trace.log')
        state['trace_summary'] = summarize(batch/'trace.log')
        p.write(batch/'trace-summary.json', state['trace_summary'])
        state['status'] = 'captured'
        print('Capture complete: '+state['capture']['stop_reason']+'. This is not a benchmark pass.', flush=True)
    except BaseException as error:
        state['status'] = 'failed'; state['error'] = str(error); raise
    finally:
        try:
            state['original_native_source_unchanged'] = p.tree_manifest(root/'source') == original
            state['isolated_source_unchanged'] = p.tree_manifest(profile/'source') == configured
            verify_hashes(profile, installed)
            verify_hashes(prior, report['artifacts'])
            if p.sha(prior/'report.json') != state['prior_report_sha256']:
                raise ValueError('Prior report changed during diagnostic.')
            for path, digest in dependencies.items():
                if p.sha(path) != digest: raise ValueError('Native dependency changed during diagnostic: ' + path)
            state['installed_model_and_prior_artifacts_unchanged'] = True
            if not state['original_native_source_unchanged'] or not state['isolated_source_unchanged']:
                raise ValueError('Source changed during diagnostic.')
        except (OSError, ValueError) as error:
            state['status'] = 'failed'; state['preservation_error'] = str(error)
        state['artifacts'] = {str(f.relative_to(batch)): p.sha(f) for f in sorted(batch.rglob('*')) if f.is_file()}
        p.write(batch/'report.json', state)
        archive = batch.with_suffix('.tar.gz')
        with tarfile.open(archive, 'w:gz') as output: output.add(batch, arcname=batch.name)
        print('Evidence archive: ' + str(archive), flush=True)
    if state['status'] != 'captured': raise ValueError(state.get('error', state.get('preservation_error')))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native_directory', type=Path)
    parser.add_argument('failed_evidence_directory', type=Path)
    args = parser.parse_args()
    root = args.native_directory.expanduser().resolve()
    prior = args.failed_evidence_directory.expanduser().resolve()
    try:
        with (root/'logs/row-reproduction.lock').open('a') as lock:
            try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError: raise ValueError('Another reproduction runner is active.')
            run(root, prior)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit('STOP: ' + str(error))
