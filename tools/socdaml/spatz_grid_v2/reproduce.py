#!/usr/bin/env python3
"""Check/build/run/validate the reconstructed V2 schedules; no environment setup.

Only the explicit 'run' subcommand launches a simulator, exactly once. Build
uses the existing compiler. Validation/results operate on saved evidence only.
"""
from pathlib import Path
import argparse
import fcntl
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

sys.dont_write_bytecode = True
from common import (ASSETS, PACKAGE, REFERENCE, REPO, config_fingerprint, grid,
                    metadata, model_guard, native, read, require, verify_artifacts,
                    verify_package, write)
import broadcast_checks
import direct_checks
import capture
import trace_allowlist


def validate(evidence, variant, root):
    evidence = Path(evidence).resolve()
    meta = verify_package()
    report = read(evidence/'report.json')
    verify_artifacts(evidence, report['artifacts'])
    require(report.get('preservation_verified') is True and report.get('kernel_unchanged') is True,
            'Run did not verify model/source/kernel preservation.')
    require(report['stop_reason']=='process_exit' and report['exit_code']==0,
            'Run did not complete normally inside its limits.')
    require(report['bytes_captured']<=report['byte_limit'] and
            report['host_elapsed_seconds']<=report['seconds_limit'], 'Run reached/exceeded a limit.')
    elf = evidence/('grid_'+variant+'.elf')
    require(hashlib.sha256(grid.kernel_bytes(elf.read_bytes())).hexdigest()==meta['kernel_sha256'],
            'Shared kernel bytes differ.')
    for name, digest in meta['exact_source_sha256'].items():
        require(native.sha(evidence/name)==digest, 'Captured target source differs: '+name)
    config = read(evidence/'runtime-config.json')
    require(config['target']['clock']['frequency']==1_000_000_000, 'Expected 1-GHz clock.')
    require(config_fingerprint(config,root)==meta['model_config_fingerprint'], 'Runtime hardware/model differs.')
    text = (evidence/'trace.log').read_text(errors='replace')
    text, progress, dma, times, computes, vector_ends = direct_checks.inspect_trace(text)
    result = grid.verify_log(text,'grid_'+variant,require_trace=True)
    checks = direct_checks if variant=='direct' else broadcast_checks
    details = checks.extra_checks(dma,times,computes,vector_ends,result)
    require(result['checked_words']==262144, 'Incomplete word validation.')
    require(sum(result['master_phase_cycles'].values())==result['application_cycles'], 'Non-contiguous phase sum.')
    return dict(result=result,details=details,progress=progress,
                input_sha256=broadcast_checks.input_fingerprint(text))


def build(root, variant, output_dir=None):
    profile = model_guard(root)
    if output_dir is None:
        batch = Path(tempfile.mkdtemp(prefix='spatz-grid-v2-build-',dir=root/'logs'))
    else:
        batch = output_dir.expanduser().resolve()
        require(batch.is_relative_to(root/'logs'), 'Build output must be under native/logs.')
        batch.mkdir()  # Never overwrite an existing build/evidence directory.
    cc = root/'prefix/riscv/bin/riscv32-unknown-elf-gcc'
    objcopy = cc.with_name('riscv32-unknown-elf-objcopy')
    objdump = cc.with_name('riscv32-unknown-elf-objdump')
    env = native.profile_env(root,profile)
    for name in metadata()['exact_source_sha256']:
        shutil.copy2(ASSETS/name,batch/name)
    commands = [[cc,*native.FLAGS,'-c',batch/'gemm_fp32.S','-o',batch/'gemm_fp32.o'],
                [objcopy,'--rename-section','.text.socdaml_spatz_gemm='+grid.SECTION+',alloc,load,readonly,code,contents',batch/'gemm_fp32.o',batch/'shared_gemm.o']]
    sdk = profile/'source'/native.SDK
    selected = ('direct','broadcast') if variant=='both' else (variant,)
    for name in selected:
        commands.append([cc,*native.FLAGS,'-DGRID_BROADCAST='+str(int(name=='broadcast')),
                         '-I'+str(sdk/'runtime/include'),'-I'+str(sdk/'app_example/include'),
                         '-nostartfiles','-Wl,--gc-sections','-Wl,--no-relax',
                         '-Wl,-Map='+str(batch/('grid_'+name+'.map')),'-T'+str(batch/'grid.ld'),
                         '-o',batch/('grid_'+name+'.elf'),sdk/'runtime/flex_start.s',
                         batch/'grid.c',batch/'shared_gemm.o'])
    for command in commands:
        native.command(command,profile/'source',env,batch/'build.log',120)
    for name in selected:
        elf = batch/('grid_'+name+'.elf')
        require(hashlib.sha256(grid.kernel_bytes(elf.read_bytes())).hexdigest()==metadata()['kernel_sha256'],
                'Rebuilt shared kernel differs from accepted bytes.')
        (batch/('grid_'+name+'.dump')).write_text(subprocess.check_output([objdump,'-d',elf],text=True,env=env))
    model_guard(root)
    record = dict(status='built_not_simulated',variants=selected,commands=[[str(x) for x in c] for c in commands],
                  source_sha256=metadata()['exact_source_sha256'],kernel_sha256=metadata()['kernel_sha256'],
                  model_source_sha256=native.sha(REFERENCE/'configured-source.json'))
    record['artifacts'] = {p.name:native.sha(p) for p in batch.iterdir() if p.is_file()}
    write(batch/'build.json',record)
    return dict(status=record['status'],evidence=str(batch))


def check_elf(elf, variant):
    meta = verify_package()
    require(hashlib.sha256(grid.kernel_bytes(elf.read_bytes())).hexdigest()==meta['kernel_sha256'], 'Kernel differs.')
    if native.sha(elf)==meta['accepted_elf_sha256'][variant]:
        return
    record = read(elf.parent/'build.json')
    verify_artifacts(elf.parent,record['artifacts'])
    require(record['status']=='built_not_simulated' and variant in record['variants'] and
            record['source_sha256']==meta['exact_source_sha256'], 'Unknown executable provenance.')


def run(root, variant, elf):
    profile = model_guard(root)
    check_elf(elf,variant)
    batch = Path(tempfile.mkdtemp(prefix='spatz-grid-v2-'+variant+'-',dir=root/'logs'))
    original = native.tree_manifest(root/'source')
    state = dict(status='preflight',variant='grid_'+variant,seconds_limit=300,byte_limit=64*1024*1024,
                 simulations_launched=0,timing_accepted=False)
    try:
        for name in metadata()['exact_source_sha256']:
            shutil.copy2(ASSETS/name,batch/name)
        shutil.copy2(elf,batch/('grid_'+variant+'.elf'))
        for file in (PACKAGE/'validated').glob('*.py'):
            dest=batch/'checking-tools'/file.name;dest.parent.mkdir(exist_ok=True);shutil.copy2(file,dest)
        shutil.copy2(__file__,batch/'packaged-runner.py')
        write(batch/'regex-verification.json',trace_allowlist.verify())
        command = [str(profile/'install/bin/gvsoc'),'--target=pulp.chips.flex_cluster.flex_cluster',
                   '--binary',str(batch/('grid_'+variant+'.elf')),'run','--trace-level=debug',
                   *('--trace='+x for x in trace_allowlist.TRACES)]
        env = native.profile_env(root,profile)
        state.update(status='running',command=command,cwd=str(profile/'source'),
                     environment={k:env.get(k) for k in trace_allowlist.ENV_KEYS},kernel_unchanged=True)
        write(batch/'report.json',state)
        print('One bounded launch; evidence: '+str(batch),flush=True)
        capture.PROFILE=profile;capture.SECONDS=300;capture.BYTE_LIMIT=64*1024*1024
        capture.capture_pty(command,env,batch/'trace.log',state)
        shutil.copy2(profile/'source/gvsoc_config.json',batch/'runtime-config.json')
        model_guard(root)
        require(native.tree_manifest(root/'source')==original, 'Original native source changed.')
        state['preservation_verified']=True
        state['artifacts']={str(p.relative_to(batch)):native.sha(p) for p in batch.rglob('*') if p.is_file() and p.name!='report.json'}
        write(batch/'report.json',state)
        verified = validate(batch,variant,root)
        write(batch/'verification.json',verified)
        state.update(status='passed',timing_accepted=True,checked_words=262144)
    except BaseException as error:
        state.update(status='failed',timing_accepted=False,error=repr(error))
        # Preserve useful partial progress without weakening complete-run checks.
        if (batch/'trace.log').exists():
            write(batch/'progress.json',direct_checks.inspect_trace((batch/'trace.log').read_text(errors='replace'))[1])
    finally:
        try:
            model_guard(root)
            require(native.tree_manifest(root/'source')==original,'Original native source changed.')
            state['preservation_verified']=True
        except BaseException as error:
            state.update(status='failed',timing_accepted=False,preservation_error=repr(error))
        state['artifacts']={str(p.relative_to(batch)):native.sha(p) for p in batch.rglob('*') if p.is_file() and p.name!='report.json'}
        write(batch/'report.json',state)
        with tarfile.open(batch.with_suffix('.tar.gz'),'w:gz') as archive:
            archive.add(batch,arcname=batch.name)
    require(state['status']=='passed','Run failed/limited; evidence preserved at '+str(batch))
    return dict(status=state['status'],evidence=str(batch))


def results(direct, broadcast, root, output):
    records={name:validate(path,name,root) for name,path in [('direct',direct),('broadcast',broadcast)]}
    a,z=records['direct'],records['broadcast']
    require(a['input_sha256']==z['input_sha256'],'Paired delivered inputs differ.')
    require(a['result']['output_sha256']==z['result']['output_sha256'],'Paired final output differs.')
    def compare(x,y):
        return dict(direct_ns=x,broadcast_ns=y,speedup=x/y,saved_ns=x-y,reduction_percent=100*(x-y)/x)
    metrics={
        'complete_input':compare(a['result']['input_stage_through_completion_cycles'],z['result']['input_stage_through_completion_cycles']),
        'DATA_IN_DMA_envelope':compare(a['details']['data_in_actual_dma_envelope_ns'],z['details']['data_in_actual_dma_envelope_ns']),
        'application':compare(a['result']['application_cycles'],z['result']['application_cycles'])}
    accepted = dict(schema='socdaml_spatz_grid_v2_results_v1',clock_hz=1_000_000_000,units='simulated ns',
                    samples_per_variant=1,workload=dict(M=256,K=64,N=256,tile=[64,64],precision='FP32',clusters=[4,4],cores_per_cluster=3),
                    kernel_sha256=metadata()['kernel_sha256'],comparisons=metrics,variants={},
                    limitations=['Current reconstruction, not exact historical presentation reproduction.',
                                 'One accepted run per variant, no variability estimate.',
                                 'Direct output serialized; broadcast output parallel. Overall comparison covers both full schedules.',
                                 'Intermittent startup timeout remains unexplained.'])
    for name,path in [('direct',direct),('broadcast',broadcast)]:
        v=records[name]
        accepted['variants'][name]=dict(evidence_id=path.name,report_sha256=native.sha(path/'report.json'),
              archive_sha256=native.sha(path.with_suffix('.tar.gz')),elf_sha256=native.sha(path/('grid_'+name+'.elf')),
              phase_ns=v['result']['master_phase_cycles'],application_ns=v['result']['application_cycles'],
              complete_input_ns=v['result']['input_stage_through_completion_cycles'],DMA_envelope_ns=v['details']['data_in_actual_dma_envelope_ns'],
              input_sha256=v['input_sha256'],output_sha256=v['result']['output_sha256'],checked_words=v['result']['checked_words'])
    write(output,accepted)
    return accepted


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-root',type=Path,default=Path(os.environ.get('SOCDAML_NATIVE_ROOT',str(REPO.parent/'SoCDAML-native'))))
    sub=parser.add_subparsers(dest='action',required=True)
    sub.add_parser('check',help='Read-only package/native dependency and regex checks; no tool launch.')
    builder=sub.add_parser('build',help='Compile applications only, never rebuild the model.')
    builder.add_argument('--variant',choices=('direct','broadcast','both'),default='both')
    builder.add_argument('--output-dir',type=Path,help='Fresh directory under native/logs; must not exist.')
    runner=sub.add_parser('run',help='One explicit 300-second/64-MiB PTY run, no retry.')
    runner.add_argument('--variant',choices=('direct','broadcast'),required=True)
    runner.add_argument('--elf',type=Path,required=True)
    validator=sub.add_parser('validate',help='Offline complete-run validation.')
    validator.add_argument('--variant',choices=('direct','broadcast'),required=True)
    validator.add_argument('--evidence',type=Path,required=True)
    pair=sub.add_parser('results',help='Offline revalidation and machine-readable pair summary.')
    pair.add_argument('--direct',type=Path,required=True)
    pair.add_argument('--broadcast',type=Path,required=True)
    pair.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();root=args.native_root.expanduser().resolve()
    if args.action in ('check','build','run'):
        require(root.is_dir(),'Native workspace missing: '+str(root))
        with (root/'logs/row-reproduction.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            if args.action=='check':
                profile=model_guard(root)
                result=dict(status='checked_no_build_no_simulation',profile=str(profile),regex=trace_allowlist.verify())
            elif args.action=='build':result=build(root,args.variant,args.output_dir)
            else:result=run(root,args.variant,args.elf.expanduser().resolve())
    elif args.action=='validate':
        result=validate(args.evidence,args.variant,root)
    else:result=results(args.direct.resolve(),args.broadcast.resolve(),root,args.output)
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    try:main()
    except (OSError,ValueError,subprocess.SubprocessError) as error:raise SystemExit('STOP: '+str(error))
