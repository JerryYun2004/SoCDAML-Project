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
