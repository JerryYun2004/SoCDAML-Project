"""One direct-V2 verification: fixed 300-second / 64-MiB limits; no build/retry."""
from pathlib import Path
from collections import Counter, defaultdict
import fcntl
import hashlib
import json
import os
import re
import resource
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = Path('/Users/jerryyun/projects/SoCDAML-native')
KIT = ROOT/'kits/grid-export-v2/SoCDAML_MacNative/reproduction'
BUILD = ROOT/'logs/presentation-grid-export-v2-build-n3h9jxb7'
PRIOR = ROOT/'logs/presentation-grid-v6nsjj3k'
PROFILE = ROOT/'profiles/presentation-spatz-v1'
SECONDS = 300
BYTE_LIMIT = 64*1024*1024
sys.path.insert(0, str(KIT))
import presentation_grid as grid
import presentation_spatz as p
from word_export import RECORD


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify_files(root, manifest):
    for name, digest in manifest.items():
        path = root/name
        require(path.resolve().is_relative_to(ROOT), 'Path outside native workspace: '+str(path))
        require(p.sha(path) == digest, 'Artifact changed: '+str(path))


def limit_file_size():
    resource.setrlimit(resource.RLIMIT_FSIZE, (BYTE_LIMIT, BYTE_LIMIT))


def inspect_trace(text):
    """Progress counts are useful even when the strict complete-run check fails."""
    text = re.sub(r'\x1b\[[0-9;]*m', '', text)
    vectors = defaultdict(Counter)
    vector_ends = defaultdict(list)
    dma = []
    word_counts = Counter()
    last_record = None
    boundaries = {}
    times = {}
    computes = {}
    for line_no, line in enumerate(text.splitlines(), 1):
        match = re.search(r'/cluster_(\d+)/pe0/spatz/trace.*\[Spatz\] Finished\s*:\s*(\d+) ns ---> (\d+) ns.*Instruction = (\S+)', line)
        if match:
            cid, start, end, op = match.groups()
            vectors[cid][op] += 1
            vector_ends[cid].append(int(end))
        match = re.search(r'/cluster_(\d+)/idma/fe/trace.*\[iDMA\] Finished\s*:\s*(\d+) ns ---> (\d+) ns.*Txn (\d+) = \{(.*)\}', line)
        if match:
            cid, start, end, txn, fields = match.groups()
            values = dict(re.findall(r'(\w+): (0x[0-9a-f]+|[12]D)', fields))
            dma.append(dict(cid=int(cid), start=int(start), end=int(end), txn=int(txn), line=line_no,
                            **{k:(v if k=='type' else int(v,16)) for k,v in values.items()}))
        match = RECORD.fullmatch(line)
        if match:
            cid, kind, index, count, _ = match.groups()
            word_counts[cid+':'+kind] += int(count)
            last_record = dict(cid=int(cid),kind=kind,index=int(index),count=int(count),line=line_no)
        for token in ('BEGIN broadcast=', 'INTERVAL_END', grid.TAG+' END'):
            if token in line:
                boundaries[token] = line_no
        match = re.fullmatch(re.escape(grid.TAG)+r' TIMES cid=(\d+) ((?:\d+ ){7}\d+)', line)
        if match:
            times[int(match[1])] = list(map(int,match[2].split()))
        match = re.fullmatch(re.escape(grid.TAG)+r' COMPUTE cid=(\d+) begin=(\d+) end=(\d+) joined=(\d+) status=(\d+) vl=(\d+) vtype=([0-9a-f]{8})',line)
        if match:
            computes[int(match[1])] = dict(zip(('begin','end','joined','status','vl'),map(int,match.groups()[1:6])))
    progress = dict(protocol_boundary_lines=boundaries,vector_counts_by_cluster=dict(vectors),
                    complete_record_words_by_tile=dict(word_counts),last_complete_export_record=last_record,
                    complete_export_record_words=sum(word_counts.values()),dma_completions=len(dma),
                    times_records=len(times),compute_records=len(computes))
    return text, progress, dma, times, computes, vector_ends


def extra_checks(dma, times, computes, vector_ends, result):
    require(len(dma)==640, 'Expected 576 initialization, 48 application and 16 readback DMA completions.')
    spans={(item['cid'],item['kind']):(item['begin'],item['end']) for item in result['dma_spans']}
    app=[]; readbacks=[]
    for cid in range(16):
        events=[e for e in dma if e['cid']==cid]
        require(len(events)==(580 if cid==0 else 4), 'Missing/unexpected DMA events for worker '+str(cid))
        offset=576 if cid==0 else 0
        require([e['txn'] for e in events]==list(range(len(events))), 'Missing/repeated DMA transaction ID.')
        a,b,out,readback=events[offset:]
        y,x=divmod(cid,4)
        expected=[('A',a,'1D',0xc0000400+y*16384,0x10000,16384),
                  ('B',b,'2D',0xc0010400+x*256,0x14000,256),
                  ('OUT',out,'2D',0x18000,0xc0020400+(y*64*256+x*64)*4,256)]
        for kind,event,typ,src,dst,size in expected:
            require((event['type'],event['src'],event['dst'],event['size'])==(typ,src,dst,size), 'DMA address/type/size mismatch.')
            if typ=='2D':
                strides=(1024,256) if kind=='B' else (256,1024)
                require((event['src_stride'],event['dst_stride'],event['repeats'])==(*strides,64), 'DMA strides/repeats mismatch.')
            begin,end=spans[cid,kind]
            require(begin<=event['start']<=event['end']<=end, 'Actual DMA completion outside issue/wait probes.')
            event=dict(event,kind=kind)
            app.append(event)
        require((readback['type'],readback['src'],readback['dst'],readback['size'],readback['src_stride'],readback['dst_stride'],readback['repeats'])==
                ('2D',out['dst'],0x24000,256,1024,256,64), 'HBM validation readback mismatch.')
        require(readback['start']>max(t[7] for t in times.values()), 'Validation DMA inside measured application interval.')
        readbacks.append(readback)
        require(max(vector_ends[str(cid)])<=computes[cid]['end']<=computes[cid]['joined'], 'Late vector completion.')
    inputs=[e for e in app if e['kind'] in ('A','B')]
    outputs=[e for e in app if e['kind']=='OUT']
    require(max(e['end'] for e in inputs)<=times[0][4], 'Input DMA incomplete at input boundary.')
    require(max(e['end'] for e in outputs)<=times[0][7], 'Output DMA incomplete at final boundary.')
    return dict(application_dma=app,validation_readbacks=readbacks,
                data_in_actual_dma_envelope_ns=max(e['end'] for e in inputs)-min(e['start'] for e in inputs),
                data_in_first_actual_dma_start_ns=min(e['start'] for e in inputs),
                data_in_last_actual_dma_completion_ns=max(e['end'] for e in inputs),
                data_out_last_actual_dma_completion_ns=max(e['end'] for e in outputs),
                master_timestamps=times[0],compute_records=computes,
                last_vector_completion_ns={c:max(ends) for c,ends in vector_ends.items()})


def run():
    batch=Path(tempfile.mkdtemp(prefix='presentation-grid-direct-v2-',dir=ROOT/'logs'))
    print('Evidence: '+str(batch),flush=True)
    state=dict(status='preflight',timing_accepted=False,seconds_limit=SECONDS,byte_limit=BYTE_LIMIT,
               simulations_launched=0,build_batch=str(BUILD),protocol='source_scheduled_spatz_grid_v2',variant='grid_direct')
    process=None
    original=None
    configured=installed=dependencies=build_report=prior_report=None

    def preservation():
        verify_files(BUILD,build_report['artifacts'])
        verify_files(PRIOR,prior_report['artifacts'])
        verify_files(PROFILE,installed)
        for name,digest in dependencies.items():
            path=Path(name)
            require(path.is_relative_to(ROOT), 'Dependency outside workspace: '+name)
            require(p.sha(path)==digest, 'Dependency changed: '+name)
        require(p.tree_manifest(PROFILE/'source')==configured,'Profile source changed.')
        require(p.tree_manifest(ROOT/'source')==original,'Original native source changed.')

    try:
        build_report=json.loads((BUILD/'report.json').read_text())
        prior_report=json.loads((PRIOR/'report.json').read_text())
        configured=json.loads((PRIOR/'configured-source.json').read_text())
        installed=json.loads((PRIOR/'installed-artifacts.json').read_text())
        dependencies=json.loads((PRIOR/'dependencies.json').read_text())
        original=p.tree_manifest(ROOT/'source')
        preservation()
        # Use exactly the prepared checker, not an edited working version.
        for name in ('presentation_grid.py','word_export.py'):
            require(p.sha(KIT/name)==p.sha(BUILD/name),'Checker differs from prepared build: '+name)
        for name in ('grid_direct.elf','grid_direct.dump','grid_direct.map','grid.c','grid.ld','word_export.h','gemm_fp32.S','compiler.json'):
            shutil.copy2(BUILD/name,batch/name)
        shutil.copy2(BUILD/'report.json',batch/'build-report.json')
        shutil.copy2(PRIOR/'report.json',batch/'prior-report.json')
        for name in ('configured-source.json','installed-artifacts.json','dependencies.json'):
            shutil.copy2(PRIOR/name,batch/name)
        (batch/'checking-tools').mkdir()
        for name in ('presentation_grid.py','word_export.py','presentation_spatz.py','spatz_macos_compat.py','spatz_cycle_compat.py'):
            shutil.copy2(KIT/name,batch/'checking-tools'/name)
        shutil.copy2(__file__,batch/'verify_grid_direct_v2.py')
        baseline=grid.kernel_bytes((PRIOR/'grid_direct.elf').read_bytes())
        require(grid.kernel_bytes((batch/'grid_direct.elf').read_bytes())==baseline,'Shared GEMM kernel changed.')
        state.update(elf_sha256=p.sha(batch/'grid_direct.elf'),kernel_sha256=hashlib.sha256(baseline).hexdigest(),kernel_unchanged=True)
        command=[str(PROFILE/'install/bin/gvsoc'),'--target=pulp.chips.flex_cluster.flex_cluster',
                 '--binary',str(batch/'grid_direct.elf'),'run','--trace-level=debug',
                 '--trace=cluster_[0-9][0-9]*/pe0/spatz/trace$',
                 '--trace=cluster_[0-9][0-9]*/idma/fe/trace$']
        state.update(status='running',command=command,cwd=str(PROFILE/'source'),simulations_launched=1)
        p.write(batch/'report.json',state)
        print('Hypothesis: V2 preserves all words and completes within 300s; one direct-only launch, no retry.',flush=True)
        started=time.monotonic()
        checkpoints=[]
        with (batch/'trace.log').open('xb') as log:
            process=subprocess.Popen(command,cwd=PROFILE/'source',env=p.profile_env(ROOT,PROFILE),
                stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,
                start_new_session=True,preexec_fn=limit_file_size)
            next_checkpoint=30
            try:
                while True:
                    elapsed=time.monotonic()-started
                    size=(batch/'trace.log').stat().st_size
                    if elapsed>=next_checkpoint:
                        checkpoints.append(dict(host_seconds=elapsed,bytes=size))
                        print(f'{elapsed:.0f}s: {size/1024**2:.2f} MiB captured',flush=True)
                        next_checkpoint+=30
                    if size>=BYTE_LIMIT:
                        state['stop_reason']='output_limit';break
                    if elapsed>=SECONDS:
                        state['stop_reason']='time_limit';break
                    if process.poll() is not None:
                        state['stop_reason']='process_exit';break
                    time.sleep(min(.1,max(0,SECONDS-elapsed)))
            finally:
                # Reap only this launch's process group; no additional launch.
                try:os.killpg(process.pid,signal.SIGKILL)
                except ProcessLookupError:pass
                process.wait(timeout=5)
        state.update(exit_code=process.returncode,host_elapsed_seconds=time.monotonic()-started,
                     bytes_captured=(batch/'trace.log').stat().st_size,checkpoints=checkpoints)
        require(state['bytes_captured']<=BYTE_LIMIT,'Output exceeded capture limit.')
        config=PROFILE/'source/gvsoc_config.json'
        if config.is_file():shutil.copy2(config,batch/'runtime-config.json')
        text,progress,dma,times,computes,vector_ends=inspect_trace((batch/'trace.log').read_text(errors='replace'))
        p.write(batch/'progress.json',progress)
        require(state['stop_reason']=='process_exit' and state['exit_code']==0,'Run did not exit normally within limits.')
        result=grid.verify_log(text,'grid_direct',require_trace=True)
        require(result['checked_words']==262144,'Incomplete numerical validation.')
        details=extra_checks(dma,times,computes,vector_ends,result)
        require(sum(result['master_phase_cycles'].values())==result['application_cycles'],'Non-contiguous application accounting.')
        preservation()
        p.write(batch/'verification.json',dict(result=result,details=details))
        state.update(status='passed',timing_accepted=True,checked_words=result['checked_words'],preservation_verified=True)
    except BaseException as error:
        state.update(status='failed',timing_accepted=False,error=repr(error))
        print('Stopped: '+repr(error),flush=True)
    finally:
        if original is not None and all(x is not None for x in (configured,installed,dependencies,build_report,prior_report)):
            try:
                preservation()
                state['preservation_verified']=True
            except BaseException as error:
                state.update(status='failed',timing_accepted=False,preservation_verified=False,preservation_error=repr(error))
        state['artifacts']={str(f.relative_to(batch)):p.sha(f) for f in sorted(batch.rglob('*')) if f.is_file() and f!=batch/'report.json'}
        p.write(batch/'report.json',state)
        with tarfile.open(batch.with_suffix('.tar.gz'),'w:gz') as archive:
            archive.add(batch,arcname=batch.name)
        print(json.dumps({k:v for k,v in state.items() if k!='artifacts'},indent=2),flush=True)
        print('Archive: '+str(batch.with_suffix('.tar.gz')),flush=True)


if __name__=='__main__':
    with (ROOT/'logs/row-reproduction.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        run()
