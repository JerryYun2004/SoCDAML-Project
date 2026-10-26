"""Unchanged accepted helper bodies; packaged imports replace log-only locations."""
from pathlib import Path
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import base64, hashlib, re, struct, os, time, pty, selectors, errno, subprocess, signal
from types import SimpleNamespace
from common import grid, diagnostic
from word_export import RECORD
controls=SimpleNamespace(diagnostic=diagnostic)
SECONDS=300
BYTE_LIMIT=64*1024*1024
PROFILE=None

def require(condition, message):
    if not condition:
        raise ValueError(message)

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
