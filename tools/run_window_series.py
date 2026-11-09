#!/usr/bin/env python3
"""Stage 6: adaptive window sweep on frozen stage-2 inputs, through seed-series reuse only."""
import argparse
import csv
import json
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import run_seed_series as seeds

EXTRA = ['case_id','source_case','network_mode','jitter_half_width_ticks','loss_probability','window']


def valid_window(value):
    return type(value) is int and 2 <= value <= seeds.MAX_SEED


def prepare(plan_path, source):
    plan = seeds.load(plan_path)
    seeds.require(isinstance(plan,dict) and set(plan)=={'schema_version','seeds','window','source_cases'}
                  and type(plan['schema_version']) is int and plan['schema_version']==1,'Invalid window plan schema')
    values = plan['seeds'];windows = plan['window'];names = plan['source_cases']
    seeds.require(isinstance(values,list) and values and all(type(v) is int and 0<=v<=seeds.MAX_SEED for v in values)
                  and len(values)==len(set(values)),'Expected explicit distinct uint64 seeds')
    seeds.require(isinstance(windows,list) and windows and all(valid_window(v) for v in windows)
                  and len(windows)==len(set(windows)),'Expected distinct integer windows in 2..uint64_max')
    seeds.require(isinstance(names,list) and names and all(isinstance(v,str) and re.fullmatch('[a-z][a-z0-9_]*',v) for v in names)
                  and len(names)==len(set(names)),'Invalid/duplicate source cases')
    network = seeds.load(source/'network_manifest.json')
    seeds.require(network.get('status')=='complete' and network.get('schema_version')==1,'Source network series is incomplete')
    mapping = {row['id']:row for row in network['cases']};cases=[]
    for name in names:
        seeds.require(name in mapping,'Missing source case: '+name)
        metadata = mapping[name];directory=(source/'cases'/name).resolve()
        seeds.require(directory.is_relative_to(source),'Source case escapes network series')
        series=seeds.load(directory/'series_manifest.json')
        seeds.require(series['status']=='complete' and series['seeds']==values and
                      [e['seed'] for e in series['entries']]==values,'Source seed list differs from plan')
        for entry in series['entries']:
            for filename in seeds.INPUT_FILES:
                path=seeds.contained(directory,entry['input_files'][filename])
                seeds.require(seeds.sha(path)==entry['input_sha256'][filename],'Source input checksum mismatch')
            scenario=seeds.load(seeds.contained(directory,entry['input_files']['scenario.json']))
            seeds.require(scenario['adaptive']['window'] in windows,'Plan must include the source window baseline')
            seeds.require(all(v>=scenario['adaptive']['min_intervals'] for v in windows),'Window must be >= source min_intervals')
        for value in windows:
            cases.append({'id':name+'_m'+str(value),'source_case':name,'mode':metadata['mode'],
                          'jitter_half_width_ticks':metadata['jitter_half_width_ticks'],
                          'loss_probability':metadata['loss_probability'],'window':value,
                          'source_series_sha256':seeds.sha(directory/'series_manifest.json')})
    return plan,network,cases


def csv_rows(path):
    with path.open(newline='',encoding='utf-8') as stream:
        reader=csv.DictReader(stream)
        return reader.fieldnames,list(reader)


def verify_history(path, scenario):
    """Check native history diagnostics, without estimating thresholds or metrics."""
    maximum = scenario['adaptive']['window']
    minimum = scenario['adaptive']['min_intervals']
    accepted = {};observations = 0;largest = 0
    with path.open(encoding='utf-8') as stream:
        for line in stream:
            record = json.loads(line)
            if record['kind'] != 'heartbeat_observed':continue
            node = record['message']['node_id']
            if record['decision'] == 'accepted':accepted[node] = accepted.get(node,0)+1
            count = min(max(accepted.get(node,0)-1,0),maximum)
            diagnostic = record['thresholds'];observations += 1
            seeds.require(diagnostic['interval_count'] == count,'Native adaptive interval count/window mismatch')
            seeds.require(diagnostic['warmed_up'] == (count >= minimum),'Native adaptive warmup mismatch')
            if count < minimum:
                seeds.require(diagnostic['suspect_ticks'] == scenario['tau0_ticks'],'Initial timeout changed before warmup')
            largest = max(largest,count)
    return {'heartbeat_observations':observations,'largest_interval_count':largest}


def verify_controls(out, source, plan, cases):
    baseline=0;fixed_controls=0;checked=0;watched={};levels={};generation_calls=0;histories=0;largest_counts={}
    for case in cases:
        origin=source/'cases'/case['source_case'];directory=out/'cases'/case['id']
        parent=seeds.load(origin/'series_manifest.json');current=seeds.load(directory/'series_manifest.json')
        seeds.require(seeds.sha(origin/'series_manifest.json')==case['source_series_sha256'],'Source manifest changed')
        seeds.require(current['status']=='complete' and current['mode']=='reused' and current['seeds']==plan['seeds']
                      and current['parameter_overrides']=={'adaptive':{'window':case['window']}},'Wrong window reuse series')
        calls=sum(c['argv'][1]=='generate' for c in current['commands']);generation_calls+=calls
        seeds.require(calls==0 and all(c['returncode']==0 for c in current['commands']),'Unexpected generation/failing CLI')
        seeds.require(len(current['entries'])==len(parent['entries'])==len(plan['seeds']),'Wrong realization count')
        for original,entry in zip(parent['entries'],current['entries']):
            seeds.require(entry['seed']==original['seed'] and entry['scenario_id']==original['scenario_id'] and
                          entry['repeat_id']==original['repeat_id'],'Seed/label mapping changed')
            a=seeds.load(seeds.contained(origin,original['input_files']['scenario.json']))
            b=seeds.load(seeds.contained(directory,entry['input_files']['scenario.json']))
            seeds.require(b==dict(a,adaptive=dict(a['adaptive'],window=case['window'])),'Fields other than adaptive.window changed')
            for name in seeds.INPUT_FILES:
                original_path=seeds.contained(origin,original['input_files'][name]);watched[original_path]=original['input_sha256'][name]
                seeds.require(seeds.sha(original_path)==watched[original_path],'Source input changed')
                seeds.require(seeds.sha(seeds.contained(directory,entry['input_files'][name]))==entry['input_sha256'][name],'Patched input checksum mismatch')
            seeds.require(entry['input_sha256']['delivery.csv']==original['input_sha256']['delivery.csv'],'Frozen trace changed')
            signature=hashlib.sha256(seeds.encode(dict(b,adaptive={k:v for k,v in b['adaptive'].items() if k!='window'})).encode()).hexdigest()
            key=(case['source_case'],entry['seed'])
            if key in levels:seeds.require(levels[key]==signature,'Frozen configuration differs between window levels')
            else:levels[key]=signature
            same_window=a['adaptive']['window']==case['window']
            if same_window:
                seeds.require(entry['results_sha256']==original['results_sha256'],
                              'Baseline content differs from source; use a compatible simulator/environment')
                baseline+=4
            for variant in ['fixed_rr','fixed_ll']:
                seeds.require(entry['results_sha256'][variant]==original['results_sha256'][variant],
                              'Fixed content differs from source while only adaptive.window changes')
                fixed_controls+=1
            output=directory/entry['batch_output'];detector_views={}
            for variant in seeds.VARIANTS:
                run=output/variant
                seeds.require(seeds.load(run/'manifest.json')['status']=='complete','Incomplete variant')
                for name in seeds.INPUT_FILES:
                    seeds.require(seeds.sha(run/'input'/name)==entry['input_sha256'][name],'Variant inputs differ')
                m=seeds.load(run/'metrics.json')
                seeds.require(all(m['invariants'].values()) and m['arrived']==len(b['tasks']) and
                              m['arrived']==m['completed']+m['unfinished'] and
                              m['unfinished']==m['queued']+m['assigned'],'Task balance failed')
                seeds.require(m['crashes']==len(b['crashes']) and
                              m['crashes']==m['detection_samples']+m['false_negatives']+m['crashes_after_false_exclusion'],
                              'Crash classification/sample counts inconsistent')
                if variant.startswith('adaptive'):
                    history=verify_history(run/'events.jsonl',b);histories+=1
                    largest_counts[case['id']]=max(largest_counts.get(case['id'],0),history['largest_interval_count'])
                _,nodes=csv_rows(run/'nodes.csv')
                view=[(r['node_id'],r['failed_ticks'],r['final_status'],r['classification']) for r in nodes]
                detector=variant.split('_')[0]
                if detector in detector_views:seeds.require(detector_views[detector]==view,'Detector depends on scheduler')
                else:detector_views[detector]=view
                for name,h in entry['results_sha256'][variant].items():seeds.require(seeds.sha(run/name)==h,'Result fingerprint mismatch')
            checked+=1
    seeds.require(all(seeds.sha(p)==h for p,h in watched.items()),'Source inputs changed during verification')
    return {'input_realizations_checked':checked,'variant_runs_checked':checked*4,
            'fixed_variant_runs':checked*2,'adaptive_variant_runs':checked*2,
            'baseline_variant_runs_identical_to_stage2':baseline,'fixed_control_runs_identical_to_stage2':fixed_controls,'generation_calls':generation_calls,
            'only_adaptive_window_changed':True,'tasks_and_delivery_frozen':True,'same_inputs_for_all_variants':True,
            'all_task_balances_valid':True,'crash_classification_counts_valid':True,
            'rr_ll_detector_classification_equal':True,'all_content_fingerprints_valid':True,
            'source_inputs_unchanged':True,'adaptive_history_runs_checked':histories,
            'largest_interval_count_by_case':largest_counts,'window_and_warmup_valid':True}


def collate(out,cases,filename):
    fields=None;count=0
    with (out/('window_'+filename)).open('w',newline='',encoding='utf-8') as stream:
        for case in cases:
            header,rows=csv_rows(out/'cases'/case['id']/'batch'/filename)
            if fields is None:
                fields=header;writer=csv.DictWriter(stream,fieldnames=EXTRA+fields);writer.writeheader()
            seeds.require(header==fields,'Native batch CSV schemas differ')
            metadata={k:case[k] for k in EXTRA if k not in ('case_id','network_mode')}
            metadata.update(case_id=case['id'],network_mode=case['mode'])
            for row in rows:writer.writerow({**metadata,**row});count+=1
    return count


def execute(args):
    plan_path=args.plan.expanduser().resolve();source=args.source_network.expanduser().resolve()
    plan,network,cases=prepare(plan_path,source)
    program=args.program.expanduser().resolve()
    seeds.require(program.is_file() and os.access(program,os.X_OK),'Executable unavailable')
    requested=args.out.expanduser()
    seeds.require(not requested.exists() and not requested.is_symlink(),'Output exists; choose a new directory')
    out=requested.resolve();seeds.require(not out.is_relative_to(source),'Output must be outside source network series')
    out.mkdir(parents=True)
    for folder in ['overrides','cases','logs']:(out/folder).mkdir()
    shutil.copyfile(plan_path,out/'plan.json');shutil.copyfile(source/'network_manifest.json',out/'source_network_manifest.json')
    watched={p:seeds.sha(p) for p in [plan_path,source/'network_manifest.json',program,Path(__file__).resolve(),Path(seeds.__file__).resolve()]}
    state={'schema_version':1,'status':'incomplete','plan_sha256':watched[plan_path],
           'source_network':str(source),'source_network_manifest_sha256':watched[source/'network_manifest.json'],
           'program':str(program),'program_sha256':watched[program],
           'driver_sha256':watched[Path(__file__).resolve()],'seed_runner_sha256':watched[Path(seeds.__file__).resolve()],
           'seeds':plan['seeds'],'window':plan['window'],'cases':[],'commands':[],
           'primary_variants':['adaptive_rr','adaptive_ll'],'invariant_controls':['fixed_rr','fixed_ll'],
           'note':'Only adaptive.window changes; min_intervals stays fixed, so window capacity does not redefine warmup. Fixed content is invariant. Clean adaptive events may differ in interval counts. No generation or new metric/statistical formulas.'}
    manifest=out/'window_manifest.json'
    try:
        for index,case in enumerate(cases):
            patch=out/'overrides'/('window_'+str(case['window'])+'.json')
            if not patch.exists():seeds.save(patch,{'adaptive':{'window':case['window']}})
            cmd=[sys.executable,str(Path(seeds.__file__).resolve()),'--reuse-series',str(source/'cases'/case['source_case']),
                 '--detector-overrides',str(patch),'--program',str(program),'--out',str(out/'cases'/case['id'])]
            record={'argv':cmd,'returncode':None,'stdout':'logs/'+case['id']+'.stdout.log','stderr':'logs/'+case['id']+'.stderr.log'}
            state['commands'].append(record);seeds.save(manifest,state);started=time.perf_counter()
            with (out/record['stdout']).open('w',encoding='utf-8') as stdout,(out/record['stderr']).open('w',encoding='utf-8') as stderr:
                r=subprocess.run(cmd,cwd=out,stdout=stdout,stderr=stderr,text=True)
            record.update(returncode=r.returncode,seconds=time.perf_counter()-started)
            seeds.require(r.returncode==0,'Saved-input window run failed; see '+str(out/record['stderr']))
            state['cases'].append(case);seeds.save(manifest,state)
            print('[%d/%d] %s complete'%(index+1,len(cases),case['id']),flush=True)
        state['checks']=verify_controls(out,source,plan,cases)
        state['comparison_rows']=collate(out,cases,'comparison.csv')
        state['summary_rows']=collate(out,cases,'summary.csv')
        for p,h in watched.items():seeds.require(seeds.sha(p)==h,'Source/program changed during experiment')
        state['status']='complete';seeds.save(manifest,state)
    except BaseException as error:
        state['status']='failed';state['error']=str(error);seeds.save(manifest,state);raise
    print('Saved:',out,'; runs:',state['checks']['variant_runs_checked'],'; generate calls: 0')
    print('Primary adaptive results; fixed content is invariant across M. No universal optimum or significance claim.')
    return 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--source-network',type=Path,required=True)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    try:return execute(args)
    except KeyboardInterrupt:
        print('Interrupted; partial experiment marked failed.',file=sys.stderr);return 130
    except (OSError,ValueError,KeyError,TypeError,ArithmeticError,subprocess.SubprocessError) as error:
        print('Coefficient series failed: '+str(error),file=sys.stderr);return 1

if __name__=='__main__':sys.exit(main())
