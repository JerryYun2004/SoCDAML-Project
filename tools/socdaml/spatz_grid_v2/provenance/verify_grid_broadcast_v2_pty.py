"""One matching broadcast-V2 verification: fixed 300-second / 64-MiB limits; no build/retry."""
from pathlib import Path
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import errno
import pty
import selectors
import base64
import struct
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
DIRECT = ROOT/'logs/presentation-grid-direct-v2-0puwdfmc'
SECONDS = 300
BYTE_LIMIT = 64*1024*1024
sys.path.insert(0, str(KIT))
import presentation_grid as grid
import presentation_spatz as p
from word_export import RECORD
import diagnose_broadcast_v2_startup as controls


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


def input_fingerprint(text):
    values=defaultdict(bytearray)
    for raw in text.splitlines():
        line=re.sub(r'\x1b\[[0-9;]*m','',raw)
        m=RECORD.fullmatch(line)
        if m and m[2] in ('A','B'):
            values[int(m[1]),m[2]].extend(base64.b64decode(m[5],validate=True))
    require(set(values)=={(c,k) for c in range(16) for k in ('A','B')}, 'Missing delivered input tile.')
    require(all(len(v)==16384 for v in values.values()), 'Incomplete delivered input words.')
    return hashlib.sha256(b''.join(values[c,k] for c in range(16) for k in ('A','B'))).hexdigest()


def canonical_config(config, elf):
    def clean(value):
        if isinstance(value,dict):return {k:clean(v) for k,v in value.items()}
        if isinstance(value,list):return [clean(v) for v in value]
        if isinstance(value,str):
            return value.replace(str(elf),'<ELF>').replace('debug_binary_0_grid_direct.elf.debugInfo','<ELF_DEBUG>').replace('debug_binary_0_grid_broadcast.elf.debugInfo','<ELF_DEBUG>')
        return value
    result=clean(config)
    # Only observation selectors differ; all timing, topology, ISA and loader
    # settings remain subject to complete structural equality.
    result['target']['gvsoc']['traces']['include_regex']='<TRACE_ALLOWLIST>'
    return result


def capture_pty(command,env,log,state):
    started=time.monotonic();master,slave=pty.openpty();process=None
    samples=[];schedule=[];offsets=iter((2,20,45));next_sample=next(offsets,None)
    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            try:
                process=subprocess.Popen(command,cwd=PROFILE/'source',env=env,stdin=subprocess.DEVNULL,
                    stdout=slave,stderr=slave,start_new_session=True)
                state['simulations_launched']=1
                os.close(slave);slave=-1;os.set_blocking(master,False)
                total=0;heartbeat=30
                with selectors.DefaultSelector() as selector,log.open('xb') as output:
                    selector.register(master,selectors.EVENT_READ)
                    while True:
                        elapsed=time.monotonic()-started
                        if elapsed>=SECONDS:state['stop_reason']='time_limit';break
                        if total>=BYTE_LIMIT:state['stop_reason']='output_limit';break
                        if elapsed>=heartbeat:
                            print(f'{elapsed:.0f}s: {total/1048576:.2f} MiB',flush=True);heartbeat+=30
                        if next_sample is not None and elapsed>=next_sample:
                            alive=process.poll() is None
                            schedule.append(dict(offset=next_sample,submitted_at=elapsed,process_alive=alive))
                            if alive:samples.append((next_sample,pool.submit(controls.diagnostic.sample_launcher,process.pid,log.parent/f'sample-{next_sample}s.txt')))
                            next_sample=next(offsets,None)
                        if not selector.select(min(.1,max(0,SECONDS-elapsed))):continue
                        try:data=os.read(master,min(65536,BYTE_LIMIT-total))
                        except OSError as error:
                            if error.errno==errno.EIO:data=b''
                            elif error.errno==errno.EAGAIN:continue
                            else:raise
                        if not data:
                            try:process.wait(timeout=max(.001,min(1,SECONDS-(time.monotonic()-started))))
                            except subprocess.TimeoutExpired:pass
                            state['stop_reason']='process_exit' if process.poll() is not None else 'output_closed'
                            break
                        output.write(data);total+=len(data)
                state['bytes_captured']=total
            finally:
                if process is not None:
                    try:os.killpg(process.pid,signal.SIGKILL)
                    except ProcessLookupError:pass
                    process.wait(timeout=5);state['exit_code']=process.returncode
                state['host_elapsed_seconds']=time.monotonic()-started
            state['samples']={str(t):f.result() for t,f in samples}
            state['sample_schedule']=schedule
            state['unscheduled_sample_offsets']=([] if next_sample is None else [next_sample])+list(offsets)
    finally:
        if slave!=-1:os.close(slave)
        os.close(master)


def extra_checks(dma, times, computes, vector_ends, result):
    require(len(dma)==624, 'Expected 576 initialization, 32 application and 16 readback DMA completions.')
    spans={(e['cid'],e['kind']):(e['begin'],e['end']) for e in result['dma_spans']}
    app=[];readbacks=[]
    for cid in range(16):
        y,x=divmod(cid,4)
        events=[e for e in dma if e['cid']==cid]
        require([e['txn'] for e in events]==list(range(len(events))), 'Missing/repeated DMA transaction ID.')
        events=events[576:] if cid==0 else events
        out_address=0xc0020400+(y*64*256+x*64)*4
        expected=[]
        if x==0:expected.append(('A','1D',0xc0000400+y*16384,0x10000,16384))
        if y==0:expected.append(('B','2D',0xc0010400+x*256,0x14000,256))
        if x==0:expected.append(('BA','1D',0x10000,0x30010000+cid*0x100000,16384))
        if y==0:expected.append(('BB','1D',0x14000,0x30014000+cid*0x100000,16384))
        expected.append(('OUT','2D',0x18000,out_address,256))
        require(len(events)==len(expected)+1, 'Wrong application/readback DMA count for '+str(cid))
        for event,(kind,typ,s,d,n) in zip(events,expected):
            require((event['type'],event['src'],event['dst'],event['size'])==(typ,s,d,n), 'DMA address/type/length mismatch: '+str((cid,kind,event)))
            if typ=='2D':
                strides=(1024,256) if kind=='B' else (256,1024)
                require((event['src_stride'],event['dst_stride'],event['repeats'])==(*strides,64), 'DMA strides/repeats mismatch.')
            begin,end=spans[cid,kind]
            require(begin<=event['start']<=event['end']<=end, 'Actual completion outside DMA issue/wait probes.')
            app.append(dict(event,kind=kind))
        readback=events[-1]
        require((readback['type'],readback['src'],readback['dst'],readback['size'],readback['src_stride'],readback['dst_stride'],readback['repeats'])==('2D',out_address,0x24000,256,1024,256,64), 'HBM readback mismatch.')
        require(readback['start']>max(t[7] for t in times.values()), 'Validation readback within measured interval.')
        readbacks.append(readback)
        require(computes[cid]['begin']>=times[cid][4], 'Compute precedes local input-ready boundary.')
        require(max(vector_ends[str(cid)])<=computes[cid]['end']<=computes[cid]['joined'], 'Late compute/vector tail.')
    inputs=[e for e in app if e['kind']!='OUT']
    hbm=[e for e in inputs if e['kind'] in ('A','B')]
    distribution=[e for e in inputs if e['kind'] in ('BA','BB')]
    outputs=[e for e in app if e['kind']=='OUT']
    require(max(e['end'] for e in hbm)<=times[0][2], 'HBM fetch incomplete at leader completion join.')
    require(max(e['end'] for e in inputs)<=times[0][4], 'Input distribution incomplete at readiness boundary.')
    require(max(e['end'] for e in outputs)<=times[0][7], 'Output DMA incomplete at final boundary.')
    return dict(application_dma=app,validation_readbacks=readbacks,
                data_in_actual_dma_envelope_ns=max(e['end'] for e in inputs)-min(e['start'] for e in inputs),
                data_in_first_actual_dma_start_ns=min(e['start'] for e in inputs),
                data_in_last_actual_dma_completion_ns=max(e['end'] for e in inputs),
                hbm_last_actual_dma_completion_ns=max(e['end'] for e in hbm),
                distribution_first_actual_start_ns=min(e['start'] for e in distribution),
                distribution_last_actual_completion_ns=max(e['end'] for e in distribution),
                data_out_last_actual_dma_completion_ns=max(e['end'] for e in outputs),
                output_dma_overlap_observed=any(a['start']<b['end'] and b['start']<a['end'] for i,a in enumerate(outputs) for b in outputs[i+1:]),
                master_timestamps=times[0],compute_records=computes,
                last_vector_completion_ns={c:max(ends) for c,ends in vector_ends.items()})


def run():
    batch=Path(tempfile.mkdtemp(prefix='presentation-grid-broadcast-v2-pty-',dir=ROOT/'logs'))
    print('Evidence: '+str(batch),flush=True)
    state=dict(status='preflight',timing_accepted=False,seconds_limit=SECONDS,byte_limit=BYTE_LIMIT,
               simulations_launched=0,build_batch=str(BUILD),protocol='source_scheduled_spatz_grid_v2',variant='grid_broadcast')
    process=None
    direct_report=json.loads((DIRECT/'report.json').read_text())
    direct_report_sha=p.sha(DIRECT/'report.json')
    direct_archive_sha=p.sha(DIRECT.with_suffix('.tar.gz'))
    original=None
    configured=installed=dependencies=build_report=prior_report=None

    def preservation():
        verify_files(DIRECT,direct_report['artifacts'])
        require(p.sha(DIRECT/'report.json')==direct_report_sha,'Accepted direct report changed.')
        require(p.sha(DIRECT.with_suffix('.tar.gz'))==direct_archive_sha,'Accepted direct archive changed.')
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
        require(direct_report['status']=='passed' and direct_report['timing_accepted'], 'Direct evidence not accepted.')
        require((BUILD/'grid.c').read_bytes()==(DIRECT/'grid.c').read_bytes(), 'Workload/input/measurement source differs.')
        for name in ('configured-source.json','installed-artifacts.json','dependencies.json','compiler.json','gemm_fp32.S','grid.ld'):
            expected=(BUILD/name) if (BUILD/name).exists() else (PRIOR/name)
            require(expected.read_bytes()==(DIRECT/name).read_bytes(), 'Direct provenance differs: '+name)
        direct_config=json.loads((DIRECT/'runtime-config.json').read_text())
        current_config=json.loads((PROFILE/'source/gvsoc_config.json').read_text())
        current_elf=current_config['target']['chip']['cluster_0']['loader']['binary'][0]
        require(canonical_config(current_config,current_elf)==canonical_config(direct_config,DIRECT/'grid_direct.elf'), 'Current hardware/model config differs before launch.')
        require(direct_config['target']['clock']['frequency']==1000000000, 'Clock not 1 GHz.')
        chip=direct_config['target']['chip']
        require({k for k in chip if re.fullmatch(r'cluster_\d+',k)}=={'cluster_'+str(c) for c in range(16)}, 'Not 16 clusters.')
        require(all(all('pe'+str(c) in chip['cluster_'+str(i)] for c in range(3)) for i in range(16)), 'Three-core cluster configuration differs.')
        direct_input_sha=input_fingerprint((DIRECT/'trace.log').read_text())
        reference=grid.reference()
        expected_input_sha=hashlib.sha256(b''.join(struct.pack('<4096I',*reference[c,k]) for c in range(16) for k in ('A','B'))).hexdigest()
        require(direct_input_sha==expected_input_sha,'Accepted direct inputs differ from V2 pattern.')
        require(grid.kernel_bytes((BUILD/'grid_broadcast.elf').read_bytes())==grid.kernel_bytes((DIRECT/'grid_direct.elf').read_bytes()),'Broadcast/direct kernels differ.')
        require(grid.kernel_bytes((DIRECT/'grid_direct.elf').read_bytes())==grid.kernel_bytes((PRIOR/'grid_direct.elf').read_bytes()),'Prior kernel differs.')
        preflight=dict(workload='A[256][64] x B[64][256] -> C[256][256]',precision='FP32',grid='4x4',cores_per_cluster=3,
                       clock_hz=1000000000,source_matches_direct=True,kernel_matches_direct=True,model_matches_direct=True,
                       input_words_sha256=direct_input_sha,measurement_definitions_identical=True,
                       output_schedule_difference='direct serialized, broadcast parallel',direct_evidence_preserved=True)
        p.write(batch/'preflight.json',preflight)
        p.write(batch/'regex-verification.json',controls.verify_regexes())
        shutil.copy2(ROOT/'logs/broadcast_v2_trace_allowlist_audit.json',batch/'trace-emission-audit.json')
        shutil.copy2(ROOT/'logs/diagnose_broadcast_v2_startup.py',batch/'trace-controls.py')
        for name in ('report.json','verification.json','readout.json','runtime-config.json'):
            shutil.copy2(DIRECT/name,batch/('direct-'+name))
        # Use exactly the prepared checker, not an edited working version.
        for name in ('presentation_grid.py','word_export.py'):
            require(p.sha(KIT/name)==p.sha(BUILD/name),'Checker differs from prepared build: '+name)
        for name in ('grid_broadcast.elf','grid_broadcast.dump','grid_broadcast.map','grid.c','grid.ld','word_export.h','gemm_fp32.S','compiler.json'):
            shutil.copy2(BUILD/name,batch/name)
        shutil.copy2(BUILD/'report.json',batch/'build-report.json')
        shutil.copy2(PRIOR/'report.json',batch/'prior-report.json')
        for name in ('configured-source.json','installed-artifacts.json','dependencies.json'):
            shutil.copy2(PRIOR/name,batch/name)
        (batch/'checking-tools').mkdir()
        for name in ('presentation_grid.py','word_export.py','presentation_spatz.py','spatz_macos_compat.py','spatz_cycle_compat.py'):
            shutil.copy2(KIT/name,batch/'checking-tools'/name)
        shutil.copy2(__file__,batch/'verify_grid_broadcast_v2_pty.py')
        baseline=grid.kernel_bytes((PRIOR/'grid_direct.elf').read_bytes())
        require(grid.kernel_bytes((batch/'grid_broadcast.elf').read_bytes())==baseline,'Shared GEMM kernel changed.')
        state.update(elf_sha256=p.sha(batch/'grid_broadcast.elf'),kernel_sha256=hashlib.sha256(baseline).hexdigest(),kernel_unchanged=True)
        command=[str(PROFILE/'install/bin/gvsoc'),'--target=pulp.chips.flex_cluster.flex_cluster',
                 '--binary',str(batch/'grid_broadcast.elf'),'run','--trace-level=debug',
                 *('--trace='+pattern for pattern in controls.TRACES)]
        env=p.profile_env(ROOT,PROFILE)
        state.update(status='running',command=command,cwd=str(PROFILE/'source'),simulations_launched=0,
                     environment={k:env.get(k) for k in controls.ENV_KEYS},stdout_stderr='merged continuously drained PTY',
                     trace_patterns=controls.TRACES)
        p.write(batch/'report.json',state)
        print('One broadcast-only event-allowlist verification, 300 seconds / 64 MiB, no retry.',flush=True)
        capture_pty(command,env,batch/'trace.log',state)
        require(state['bytes_captured']<=BYTE_LIMIT,'Output exceeded capture limit.')
        config=PROFILE/'source/gvsoc_config.json'
        if config.is_file():shutil.copy2(config,batch/'runtime-config.json')
        text,progress,dma,times,computes,vector_ends=inspect_trace((batch/'trace.log').read_text(errors='replace'))
        p.write(batch/'progress.json',progress)
        require(state['stop_reason']=='process_exit' and state['exit_code']==0,'Run did not exit normally within limits.')
        result=grid.verify_log(text,'grid_broadcast',require_trace=True)
        require(result['checked_words']==262144,'Incomplete numerical validation.')
        require(input_fingerprint(text)==direct_input_sha,'Delivered input values differ across variants.')
        direct_result=json.loads((DIRECT/'verification.json').read_text())['result']
        require(result['output_sha256']==direct_result['output_sha256'],'Paired final HBM output differs.')
        after_config=json.loads((batch/'runtime-config.json').read_text())
        require(canonical_config(after_config,batch/'grid_broadcast.elf')==canonical_config(direct_config,DIRECT/'grid_direct.elf'), 'Runtime model/clock/configuration differs after normalizing ELF path.')
        details=extra_checks(dma,times,computes,vector_ends,result)
        details['pair_inputs_and_final_outputs_bit_identical']=True
        details['runtime_configuration_matches_direct_except_elf_debug_names_and_trace_allowlist']=True
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
