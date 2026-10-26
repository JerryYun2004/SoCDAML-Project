"""One authorized 60s/64-MiB startup capture of the unchanged broadcast ELF.

PTY/output draining plus control/completion events distinguish invisible
progress from pre-BEGIN blocking. Samples at 2/20/45 seconds are concurrent.
No timing acceptance, model mutation, compilation or automatic retry.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import errno
import ctypes
import fcntl
import json
import os
import pty
import selectors
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

sys.dont_write_bytecode=True
ROOT=Path('/Users/jerryyun/projects/SoCDAML-native')
KIT=ROOT/'kits/grid-export-v2/SoCDAML_MacNative/reproduction'
DIRECT=ROOT/'logs/presentation-grid-direct-v2-0puwdfmc'
FAILED=ROOT/'logs/presentation-grid-broadcast-v2-71uk1e55'
PROFILE=ROOT/'profiles/presentation-spatz-v1'
sys.path.insert(0,str(KIT))
import presentation_spatz as p
import grid_diagnose as diagnostic

TRACES=(
    '^/chip/cluster_[0-9][0-9]*/loader/trace$',
    '^/chip/cluster_[0-9][0-9]*/pe[012]/exception$',
    '^/chip/cluster_[0-9][0-9]*/cluster_registers/trace$',
    '^/chip/cluster_[0-9][0-9]*/idma/fe/trace$',
    '^/chip/cluster_[0-9][0-9]*/pe0/spatz/trace$',
)
ENV_KEYS=('PATH','PYTHONPATH','LD_LIBRARY_PATH','DYLD_LIBRARY_PATH',
          'DYLD_FALLBACK_LIBRARY_PATH','DYLD_INSERT_LIBRARIES','DYLD_FRAMEWORK_PATH',
          'LANG','LC_ALL','LC_NUMERIC','LC_TIME','TZ','OMP_NUM_THREADS',
          'SYSTEMC_HOME','SOCDAML_DRAMSYS_LIBRARY','PYTHONDONTWRITEBYTECODE')


def verify_regexes():
    # The pinned trace engine calls regcomp(regex, path, 0), i.e. POSIX BRE.
    # Use that same libc API and flags, not Python's different regex grammar.
    libc=ctypes.CDLL(None)
    libc.regcomp.argtypes=(ctypes.c_void_p,ctypes.c_char_p,ctypes.c_int)
    libc.regexec.argtypes=(ctypes.c_void_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_int)
    libc.regfree.argtypes=(ctypes.c_void_p,)
    paths=[]
    for cid in range(16):
        paths += [f'/chip/cluster_{cid}/pe{core}/{kind}' for core in range(3)
                  for kind in ('wrapper','exec','exception','insn','decoder','fetch','clock','memory')]
        paths += [f'/chip/cluster_{cid}/'+kind for kind in
                  ('loader/trace','cluster_registers/trace','idma/fe/trace','pe0/spatz/trace')]
    paths.append('/chip/ctrl_registers/trace')
    counts=[]
    for pattern in TRACES:
        # Opaque regex_t storage, conservatively oversized and long-aligned.
        regex=(ctypes.c_long*512)()
        if libc.regcomp(regex,pattern.encode(),0)!=0:raise ValueError('Invalid BRE: '+pattern)
        try:
            matches=[path for path in paths if libc.regexec(regex,path.encode(),0,None,0)==0]
            if not matches or any(path.endswith('/'+kind) for path in matches for kind in ('insn','decoder','exec','fetch','clock','memory','wrapper')):
                raise ValueError('Missing match or continuous instruction filter: '+pattern)
            expected=48 if '/pe[012]/' in pattern else 16
            if len(matches)!=expected:raise ValueError('Wrong BRE coverage: '+pattern)
            counts.append(dict(pattern=pattern,matched_paths=len(matches)))
        finally:libc.regfree(regex)
    return dict(api='libc regcomp/regexec',regcomp_flags=0,syntax='POSIX basic',
                coverage=counts,continuous_instruction_tracing=False)


def run():
    regex_verification=verify_regexes()
    roots=(DIRECT,FAILED)
    reports={root:json.loads((root/'report.json').read_text()) for root in roots}
    identities={root:(p.sha(root/'report.json'),p.sha(root.with_suffix('.tar.gz'))) for root in roots}
    original=p.tree_manifest(ROOT/'source')
    configured=json.loads((FAILED/'configured-source.json').read_text())
    installed=json.loads((FAILED/'installed-artifacts.json').read_text())
    dependencies=json.loads((FAILED/'dependencies.json').read_text())

    def preserve():
        for root in roots:
            diagnostic.verify_hashes(root,reports[root]['artifacts'])
            if identities[root]!=(p.sha(root/'report.json'),p.sha(root.with_suffix('.tar.gz'))):
                raise ValueError('Existing evidence changed: '+str(root))
        diagnostic.verify_hashes(PROFILE,installed)
        for name,digest in dependencies.items():
            if p.sha(name)!=digest:raise ValueError('Dependency changed: '+name)
        if p.tree_manifest(ROOT/'source')!=original or p.tree_manifest(PROFILE/'source')!=configured:
            raise ValueError('Source changed; do not launch.')

    preserve()
    batch=Path(tempfile.mkdtemp(prefix='broadcast-v2-startup-diagnose-',dir=ROOT/'logs'))
    print('Diagnostic evidence: '+str(batch),flush=True)
    for name in ('grid_broadcast.elf','grid_broadcast.dump','grid.c','installed-artifacts.json','dependencies.json'):
        shutil.copy2(FAILED/name,batch/name)
    shutil.copy2(__file__,batch/'diagnose.py')
    command=[str(PROFILE/'install/bin/gvsoc'),'--target=pulp.chips.flex_cluster.flex_cluster',
             '--binary',str(batch/'grid_broadcast.elf'),'run','--trace-level=debug',
             *('--trace='+pattern for pattern in TRACES)]
    env=p.profile_env(ROOT,PROFILE)
    state=dict(status='running',timing_accepted=False,command=command,cwd=str(PROFILE/'source'),
               environment={k:env.get(k) for k in ENV_KEYS},seconds_limit=60,byte_limit=64*1024*1024,
               trace_patterns=TRACES,elf_sha256=p.sha(batch/'grid_broadcast.elf'),
               hypothesis='Determine whether CPUs start, enter barriers/init DMA, or leave GVSoC without a next event; no root cause assumed.',
               stdout_stderr='merged PTY, continuously drained; no performance acceptance',simulations_launched=0)
    p.write(batch/'report.json',state)
    p.write(batch/'regex-verification.json',regex_verification)
    master,slave=pty.openpty()
    process=None
    samples=[];sample_schedule=[]
    try:
        started=time.monotonic()
        with ThreadPoolExecutor(max_workers=1) as pool:
            try:
                process=subprocess.Popen(command,cwd=PROFILE/'source',env=env,stdin=subprocess.DEVNULL,
                                         stdout=slave,stderr=slave,start_new_session=True)
                state['simulations_launched']=1
                os.close(slave);slave=-1
                os.set_blocking(master,False)
                total=0;offsets=iter((2,20,45));next_sample=next(offsets,None)
                with selectors.DefaultSelector() as selector,(batch/'trace.log').open('xb') as output:
                    selector.register(master,selectors.EVENT_READ)
                    while True:
                        elapsed=time.monotonic()-started
                        if elapsed>=60:state['stop_reason']='time_limit';break
                        if total>=64*1024*1024:state['stop_reason']='output_limit';break
                        if next_sample is not None and elapsed>=next_sample:
                            alive=process.poll() is None
                            sample_schedule.append(dict(offset_seconds=next_sample,submitted_at_seconds=elapsed,process_alive=alive))
                            if alive:
                                destination=batch/('sample-'+str(next_sample)+'s.txt')
                                samples.append((next_sample,pool.submit(diagnostic.sample_launcher,process.pid,destination)))
                            next_sample=next(offsets,None)
                        if not selector.select(min(.1,max(0,60-elapsed))):continue
                        try:data=os.read(master,min(65536,64*1024*1024-total))
                        except OSError as error:
                            if error.errno==errno.EIO:data=b''
                            elif error.errno==errno.EAGAIN:continue
                            else:raise
                        if not data:state['stop_reason']='output_closed';break
                        output.write(data);total+=len(data)
                state['bytes_captured']=total
            finally:
                if process is not None:
                    try:os.killpg(process.pid,signal.SIGKILL)
                    except ProcessLookupError:pass
                    process.wait(timeout=5)
                    state['exit_code']=process.returncode
                state['host_capture_elapsed_seconds']=time.monotonic()-started
            state['samples']={str(offset):future.result() for offset,future in samples}
            state['sample_schedule']=sample_schedule
            state['unscheduled_sample_offsets']=list(([] if next_sample is None else [next_sample]))+list(offsets)
        config=PROFILE/'source/gvsoc_config.json'
        if config.exists():shutil.copy2(config,batch/'runtime-config.json')
        # Do not reuse the older summary's 1D-only DMA accounting.
        text=(batch/'trace.log').read_text(errors='replace')
        state['recorded_dma_completions']=text.count('[iDMA] Finished')
        state['status']='captured'
    except BaseException as error:
        state.update(status='failed',error=repr(error))
    finally:
        if slave!=-1:os.close(slave)
        os.close(master)
        try:preserve();state['preservation_verified']=True
        except BaseException as error:state.update(status='failed',preservation_error=repr(error))
        state['artifacts']={str(f.relative_to(batch)):p.sha(f) for f in sorted(batch.rglob('*')) if f.is_file() and f!=batch/'report.json'}
        p.write(batch/'report.json',state)
        with tarfile.open(batch.with_suffix('.tar.gz'),'w:gz') as archive:archive.add(batch,arcname=batch.name)
        print(json.dumps({k:v for k,v in state.items() if k!='artifacts'},indent=2),flush=True)


if __name__=='__main__':
    with (ROOT/'logs/row-reproduction.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        run()
