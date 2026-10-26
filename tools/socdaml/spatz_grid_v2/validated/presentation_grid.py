#!/usr/bin/env python3
"""Native K=64 Spatz grid pair retaining benchmark 4/5 source schedules."""
from collections import Counter
from functools import lru_cache
from pathlib import Path
import argparse
import fcntl
import hashlib
import json
import platform
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import presentation_spatz as p
from word_export import expand_word_blocks

HERE = Path(__file__).resolve().parent
ASSETS = HERE/'presentation_grid'
TAG = '[SOCDAML_SPATZ_GRID_V2]'
VARIANTS = ('grid_direct', 'grid_broadcast')
SECTION = '.socdaml_spatz_gemm'
ADDRESS = 0x803f0000
LINKER_SHA = '23c5a751e0b8095f135bd79fd1fbd2ca426ddc4db786f3681b2d77cd75be559f'


def linker(original):
    if hashlib.sha256(original.encode()).hexdigest()!=LINKER_SHA:
        raise ValueError('Unexpected native linker script; preserve for review.')
    at=original.rfind('}')
    return original[:at]+f'''
  /* Both grid executables use identical kernel bytes at this fixed address. */
  {SECTION} 0x{ADDRESS:08x} : {{ KEEP(*({SECTION})) }} >L3 :l3
  ASSERT(SIZEOF({SECTION}) > 0, "Missing Spatz kernel")
  ASSERT(SIZEOF({SECTION}) <= 0x1000, "Spatz kernel exceeds reserved page")
'''+original[at:]


def kernel_bytes(elf):
    """Validate the executable ELF mapping, not just a disassembly label."""
    if len(elf)<52 or elf[:6]!=b'\x7fELF\x01\x01' or struct.unpack_from('<H',elf,18)[0]!=243:
        raise ValueError('Expected ELF32 little-endian RISC-V.')
    shoff=struct.unpack_from('<I',elf,32)[0]; size,count,names=struct.unpack_from('<HHH',elf,46)
    if size!=40 or not 0<names<count or shoff+size*count>len(elf): raise ValueError('Invalid sections.')
    heads=[struct.unpack_from('<10I',elf,shoff+i*size) for i in range(count)]
    def blob(h):
        if h[1]==8:return b''
        if h[4]+h[5]>len(elf):raise ValueError('Truncated section.')
        return elf[h[4]:h[4]+h[5]]
    strings=blob(heads[names]); selected=[]
    for h in heads:
        if h[0]>=len(strings) or strings.find(b'\0',h[0])<0:raise ValueError('Invalid section name.')
        if strings[h[0]:strings.find(b'\0',h[0])].decode()==SECTION:selected.append(h)
    if len(selected)!=1:raise ValueError('Missing/repeated kernel section.')
    h=selected[0]; data=blob(h)
    if h[3]!=ADDRESS or h[2]&6!=6 or h[8]<64 or not 0<len(data)<=4096:raise ValueError('Invalid kernel placement.')
    for other in heads:
        if other!=h and other[2]&2 and other[5] and other[3]<ADDRESS+len(data) and ADDRESS<other[3]+other[5]:
            raise ValueError('Kernel overlaps another allocated section.')
    off=struct.unpack_from('<I',elf,28)[0]; nsize,ncount=struct.unpack_from('<HH',elf,42)
    if nsize!=32 or off+nsize*ncount>len(elf):raise ValueError('Invalid segments.')
    for i in range(ncount):
        typ,start,va,pa,filesz,memsz,flags,align=struct.unpack_from('<8I',elf,off+i*nsize)
        delta=ADDRESS-va
        if typ==1 and flags&1 and 0<=delta and delta+len(data)<=filesz and start+filesz<=len(elf):
            if pa+delta==ADDRESS and elf[start+delta:start+delta+len(data)]==data:return data
    raise ValueError('Kernel not mapped in an executable load segment.')


@lru_cache(maxsize=1)
def reference():
    a=[[1+(r*3+k*5+(r^k)+(r>>6)*3)%16 for k in range(64)] for r in range(256)]
    b=[[1+(k*7+c*11+(k^c)+(c>>6)*5)%16 for c in range(256)] for k in range(64)]
    def bits(n):return struct.unpack('<I',struct.pack('<f',n))[0]
    result={}
    for cid in range(16):
        y,x=divmod(cid,4)
        result[cid,'A']=tuple(bits(a[y*64+r][k]) for r in range(64) for k in range(64))
        result[cid,'B']=tuple(bits(b[k][x*64+c]) for k in range(64) for c in range(64))
        result[cid,'C']=tuple(bits(sum(a[y*64+r][k]*b[k][x*64+c] for k in range(64))) for r in range(64) for c in range(64))
        result[cid,'HBM_C']=result[cid,'C']
    return result


def verify_log(text, variant, require_trace=True):
    if variant not in VARIANTS:raise ValueError('Unknown variant.')
    text=expand_word_blocks(text)
    text=re.sub(r'\x1b\[[0-9;]*m','',text)
    begin=TAG+f' BEGIN broadcast={int(variant==VARIANTS[1])} M=256 K=64 N=256'
    for marker in (begin,TAG+' INTERVAL_END',TAG+' END'):
        if text.count(marker)!=1:raise ValueError('Missing/repeated protocol boundary: '+marker)
    lo,hi,tail=(text.index(marker) for marker in (begin,TAG+' INTERVAL_END',TAG+' END'))
    if not lo<hi<tail:raise ValueError('Protocol boundaries out of order.')
    for name in ('TIMES','COMPUTE','DMA','WORDS'):
        if TAG+' '+name in text[:hi]+text[tail:]:raise ValueError('Evidence printed inside/outside wrong timing boundary.')
    prefix=re.escape(TAG)
    times={}; computes={}; dma={}; words={}
    for line in text[hi:tail].splitlines():
        if line.startswith(TAG+' TIMES'):
            m=re.fullmatch(prefix+r' TIMES cid=(\d+) ((?:\d+ ){7}\d+)',line)
            if not m or int(m[1]) in times:raise ValueError('Malformed/repeated TIMES.')
            times[int(m[1])]=list(map(int,m[2].split()))
        elif line.startswith(TAG+' COMPUTE'):
            m=re.fullmatch(prefix+r' COMPUTE cid=(\d+) begin=(\d+) end=(\d+) joined=(\d+) status=(\d+) vl=(\d+) vtype=([0-9a-f]{8})',line)
            if not m or int(m[1]) in computes:raise ValueError('Malformed/repeated COMPUTE.')
            cid,start,end,joined,status,vl=map(int,m.groups()[:6])
            if status or vl not in (1,2,4,8,16,32,64) or int(m[7],16)!=0xd2 or not 0<start<end<=joined<2**32:
                raise ValueError('Invalid compute cycles or vector configuration.')
            computes[cid]={'begin':start,'end':end,'joined':joined,'vl':vl}
        elif line.startswith(TAG+' DMA'):
            m=re.fullmatch(prefix+r' DMA cid=(\d+) kind=(A|B|BA|BB|OUT) begin=(\d+) end=(\d+)',line)
            if not m:raise ValueError('Malformed DMA record.')
            key=(int(m[1]),m[2]); span=(int(m[3]),int(m[4]))
            if key in dma or not 0<span[0]<span[1]<2**32:raise ValueError('Invalid/repeated DMA span.')
            dma[key]=span
        elif line.startswith(TAG+' WORDS'):
            m=re.fullmatch(prefix+r' WORDS cid=(\d+) kind=(A|B|C|HBM_C) index=(\d+) ((?:[0-9a-f]{8} ){7}[0-9a-f]{8})',line)
            if not m:raise ValueError('Malformed output words.')
            key=(int(m[1]),m[2]); row=words.setdefault(key,[])
            if int(m[3])!=len(row):raise ValueError('Duplicate/missing/reordered word index.')
            row.extend(int(x,16) for x in m[4].split())
    if set(times)!=set(range(16)) or set(computes)!=set(range(16)):raise ValueError('Missing/unexpected worker timing.')
    for cid,t in times.items():
        if not 0<t[0]<t[1]<t[2]<=t[3]<=t[4]<t[5]<t[6]<t[7]<2**32:raise ValueError('Invalid phase boundaries.')
        if variant==VARIANTS[0] and t[2:5]!=[t[2]]*3:raise ValueError('Direct must not include a broadcast stage.')
        if variant==VARIANTS[1] and not t[2]<t[3]<t[4]:raise ValueError('Broadcast stage absent.')
    expected=reference()
    if set(words)!=set(expected):raise ValueError('Missing/unexpected numerical record.')
    for key,values in words.items():
        if tuple(values)!=expected[key]:raise ValueError('Numerical/transport mismatch: '+str(key))
    expected_dma={(cid,'OUT') for cid in range(16)}
    for cid in range(16):
        y,x=divmod(cid,4)
        if variant==VARIANTS[0]:expected_dma.update(((cid,'A'),(cid,'B')))
        else:
            if x==0:expected_dma.update(((cid,'A'),(cid,'BA')))
            if y==0:expected_dma.update(((cid,'B'),(cid,'BB')))
    if set(dma)!=expected_dma:raise ValueError('Missing/unexpected DMA actions.')
    master=times[0]
    for (cid,kind),(start,end) in dma.items():
        t=times[cid]
        low,high=(t[0],t[2]) if kind in ('A','B') else (t[2],t[4]) if kind in ('BA','BB') else (t[5],t[7])
        if not low<=start<end<=high or start<master[0] or end>master[7]:raise ValueError('DMA outside timing scope.')
        if kind!='OUT' and end>master[4]:raise ValueError('Input tail exceeds master completion join.')
    if max(c['end'] for c in computes.values())>master[5]:raise ValueError('Compute tail exceeds output handoff.')
    operations={cid:[] for cid in range(16)}
    for line in text.splitlines():
        if '[Spatz] Finished' not in line:continue
        m=re.search(r'/cluster_(\d+)/(?:pe0|fp_ss0)/spatz/trace.*\[Spatz\] Finished\s*:\s*(\d+) ns ---> (\d+) ns.*Instruction = (\S+)',line)
        if not m or int(m[1]) not in operations:raise ValueError('Unexpected vector execution path.')
        operations[int(m[1])].append((int(m[2]),int(m[3]),m[4]))
    if '[Spatz] Finished' in text[:lo]+text[hi:]:raise ValueError('Vector execution outside application window.')
    for cid,ops in operations.items():
        c=computes[cid]; chunks=64//c['vl']
        want={'vle32.v':1024*chunks,'vfmul.vf':64*chunks,'vfmacc.vf':4032*chunks,'vse32.v':64*chunks}
        if require_trace and Counter(op for _,_,op in ops)!=want:raise ValueError('Runtime instruction count mismatch: '+str(cid))
        if any(not c['begin']<=a<=b<=c['end'] for a,b,_ in ops):raise ValueError('Vector completion outside compute interval: '+str(cid))
    # Synthetic host fixtures deliberately provide no vector trace; never mark
    # their cycle values as accepted performance evidence.
    inputs=[span for (cid,kind),span in dma.items() if kind!='OUT']
    outputs=[span for (cid,kind),span in dma.items() if kind=='OUT']
    phases=['hbm_input_schedule','sync_after_input','broadcast_schedule','sync_after_broadcast',
            'compute_and_required_sync','output_schedule','final_sync']
    return {'status':'passed','variant':variant,'protocol':'source_scheduled_spatz_grid_v2',
        'timing_accepted':require_trace,'checked_words':sum(len(x) for x in words.values()),
        'validation_export':'lossless_base64_le32_v2',
        'output_sha256':hashlib.sha256(b''.join(struct.pack('<4096I',*words[cid,'HBM_C']) for cid in range(16))).hexdigest(),
        'application_cycles':master[7]-master[0],
        'input_stage_through_completion_cycles':master[4]-master[0],
        'data_in_transfer_envelope_cycles':max(e for s,e in inputs)-min(s for s,e in inputs),
        'data_in_dma_service_sum_cycles':sum(e-s for s,e in inputs),
        'data_out_transfer_envelope_cycles':max(e for s,e in outputs)-min(s for s,e in outputs),
        'master_phase_cycles':dict(zip(phases,(b-a for a,b in zip(master,master[1:])))),
        'compute_per_worker_cycles':{str(cid):c['end']-c['begin'] for cid,c in computes.items()},
        'sync_after_compute_per_worker_cycles':{str(cid):c['joined']-c['end'] for cid,c in computes.items()},
        'dma_spans':[{'cid':cid,'kind':kind,'begin':s,'end':e} for (cid,kind),(s,e) in sorted(dma.items())],
        'checksum_sync':'absent; no checksum or validation inside application interval',
        'allocation':'fixed buffers; setup excluded',
        'historical_slide_equivalence':False,'isolated_broadcast_speedup_claim':False}


def build_pair(profile,batch,env):
    source=profile/'source'; sdk=source/p.SDK
    cc=shutil.which('riscv32-unknown-elf-gcc',path=env['PATH'])
    dump=shutil.which('riscv32-unknown-elf-objdump',path=env['PATH'])
    copy=shutil.which('riscv32-unknown-elf-objcopy',path=env['PATH'])
    if not all((cc,dump,copy)):raise ValueError('Existing RISC-V compiler/binutils required.')
    shutil.copy2(ASSETS/'grid.c',batch/'grid.c'); shutil.copy2(p.ASSETS/'gemm_fp32.S',batch/'gemm_fp32.S')
    shutil.copy2(ASSETS/'word_export.h',batch/'word_export.h')
    (batch/'grid.ld').write_text(linker((sdk/'runtime/flex_memory.ld').read_text()))
    obj=batch/'gemm_fp32.o'; renamed=batch/'shared_gemm.o'
    p.command([cc,*p.FLAGS,'-c',batch/'gemm_fp32.S','-o',obj],source,env,batch/'build.log',120)
    p.command([copy,'--rename-section','.text.socdaml_spatz_gemm='+SECTION+',alloc,load,readonly,code,contents',obj,renamed],source,env,batch/'build.log',120)
    programs={}; identity=None
    for flag,variant in enumerate(VARIANTS):
        elf=batch/(variant+'.elf')
        p.command([cc,*p.FLAGS,'-DGRID_BROADCAST='+str(flag),'-I'+str(sdk/'runtime/include'),
            '-I'+str(sdk/'app_example/include'),'-nostartfiles','-Wl,--gc-sections','-Wl,--no-relax',
            '-Wl,-Map='+str(batch/(variant+'.map')),'-T'+str(batch/'grid.ld'),'-o',elf,
            sdk/'runtime/flex_start.s',batch/'grid.c',renamed],source,env,batch/'build.log',120)
        text=subprocess.check_output([dump,'-d',str(elf)],text=True,env=env,timeout=60)
        (batch/(variant+'.dump')).write_text(text)
        data=kernel_bytes(elf.read_bytes())
        if f'{ADDRESS:08x} <socdaml_spatz_gemm>:' not in text or any(op not in text for op in ('vle32.v','vfmul.vf','vfmacc.vf','vse32.v')):
            raise ValueError('Vector kernel absent from disassembly.')
        if identity is not None and data!=identity:raise ValueError('Paired linked kernel bytes differ.')
        identity=data; programs[variant]=elf
    p.write(batch/'compiler.json',{'compiler':cc,'compiler_sha256':p.sha(cc),
        'version':subprocess.check_output([cc,'--version'],text=True,timeout=30),
        'common_flags':p.FLAGS,'shared_object_sha256':p.sha(renamed),
        'kernel_address':ADDRESS,'kernel_bytes_sha256':hashlib.sha256(identity).hexdigest(),
        'kernel_source_sha256':p.sha(batch/'gemm_fp32.S')})
    return programs


def run(root):
    if platform.system()!='Darwin' or platform.machine()!='arm64' or not (root/'.socdaml-native-v1').is_file():
        raise ValueError('Use native.sh with the existing Apple Silicon native workspace.')
    batch=Path(tempfile.mkdtemp(prefix='presentation-grid-',dir=root/'logs'))
    print('Spatz grid evidence: '+str(batch),flush=True)
    state={'status':'running','protocol':'source_scheduled_spatz_grid_v2'}; before=None
    try:
        for name in ('flex_cluster.py','cluster_unit.py','ctrl_registers.cpp'):
            if p.sha(root/'source/soft_hier/flex_cluster'/name)!=p.sha(HERE/'evidence/compute-model-source'/name):
                raise ValueError('Native machine/clock source changed: '+name)
        profile,before=p.create_profile(root,batch); env=p.profile_env(root,profile)
        for name in ('presentation_grid.py','word_export.py','presentation_spatz.py','spatz_cycle_compat.py','spatz_macos_compat.py'):
            target=batch/'checking-tools'/name; target.parent.mkdir(exist_ok=True); shutil.copy2(HERE/name,target)
        programs=build_pair(profile,batch,env)
        p.build_simulator(root,profile,batch,env)
        model=json.loads((batch/'installed-artifacts.json').read_text())
        state['variants']={}
        for variant,elf in programs.items():
            print('Running '+variant+' with K=64 and Spatz.',flush=True)
            command=[str(profile/'install/bin/gvsoc'),'--target=pulp.chips.flex_cluster.flex_cluster','--binary',str(elf),'run','--trace=spatz','--trace-level=debug']
            execution=p.command(command,profile/'source',env,batch/(variant+'.log'),timeout=1800)
            result=verify_log((batch/(variant+'.log')).read_text(errors='replace'),variant)
            result['execution']=execution; state['variants'][variant]=result
            p.write(batch/(variant+'.json'),result)
            for rel,digest in model.items():
                if p.sha(profile/rel)!=digest:raise ValueError('Installed model changed during pair: '+rel)
        direct,bcast=(state['variants'][v] for v in VARIANTS)
        if direct['output_sha256']!=bcast['output_sha256']:raise ValueError('Paired outputs differ.')
        state['comparison']={key:{'direct_cycles':direct[key],'broadcast_cycles':bcast[key],
            'speedup':direct[key]/bcast[key],'time_reduction_percent':100*(direct[key]-bcast[key])/direct[key]}
            for key in ('data_in_transfer_envelope_cycles','input_stage_through_completion_cycles','application_cycles')}
        if p.tree_manifest(profile/'source')!=json.loads((profile/'configured-source.json').read_text()):
            raise ValueError('Isolated source changed during grid run.')
        state['status']='passed'
        print('PASS: both K=64 Spatz grid strategies; full input/output and vector-completion checks passed.',flush=True)
        print(json.dumps(state['comparison'],indent=2),flush=True)
    except BaseException as error:
        state['status']='failed';state['error']=str(error);raise
    finally:
        if before is not None:
            try:state['original_native_source_unchanged']=p.tree_manifest(root/'source')==before
            except (ValueError,OSError) as error:
                state['original_native_source_unchanged']=False
                state['preservation_check_error']=str(error)
            if not state['original_native_source_unchanged']:
                state['status']='failed';state['error']='Original native source changed during run.'
        state['artifacts']={str(f.relative_to(batch)):p.sha(f) for f in sorted(batch.rglob('*')) if f.is_file() and f.name!='report.json'}
        p.write(batch/'report.json',state)
        archive=batch.with_suffix('.tar.gz')
        with tarfile.open(archive,'w:gz') as output:output.add(batch,arcname=batch.name)
        print(f'Evidence archive ({state["status"]}): {archive}',flush=True)
    if state['status']!='passed':raise ValueError(state['error'])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('native_directory',type=Path)
    args=parser.parse_args();root=args.native_directory.expanduser().resolve()
    try:
        with (root/'logs/row-reproduction.lock').open('a') as lock:
            try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:raise ValueError('Another reproduction runner is active.')
            run(root)
    except (ValueError,OSError,subprocess.SubprocessError) as error:raise SystemExit('STOP: '+str(error))
