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
