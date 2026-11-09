#!/usr/bin/env python3
"""Stage 7: four-node load/performance series through existing generate/batch."""
import argparse
import copy
import csv
from decimal import Decimal
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode=True
import run_seed_series as seeds

EXTRA=['case_id','performance_profile','load_id','network_profile','network_mode',
       'arrival_period_ticks','total_performance_work_per_second']


def number(value):return type(value) in (int,Decimal) and (not isinstance(value,Decimal) or value.is_finite())
def ticks(value,positive=False):return type(value) is int and (0<value if positive else 0<=value) and value<1<<63


def named_rows(rows,fields):
    seeds.require(isinstance(rows,list) and rows,'Expected explicit nonempty profile list')
    seen=set()
    for row in rows:
        seeds.require(isinstance(row,dict) and set(row)==fields and isinstance(row['id'],str) and
                      re.fullmatch('[a-z][a-z0-9_]*',row['id']) and row['id'] not in seen,'Invalid/duplicate profile')
        seen.add(row['id'])
    return rows


def prepare(plan_path):
    plan=seeds.load(plan_path)
    fields={'schema_version','base_generator','seeds','performance_profiles','loads','networks','modes'}
    seeds.require(isinstance(plan,dict) and set(plan)==fields and type(plan['schema_version']) is int and plan['schema_version']==1,'Invalid load plan schema')
    values=plan['seeds']
    seeds.require(isinstance(values,list) and values and all(type(v) is int and 0<=v<=seeds.MAX_SEED for v in values)
                  and len(values)==len(set(values)),'Expected explicit distinct uint64 seeds')
    name=plan['base_generator']
    seeds.require(isinstance(name,str) and name and not Path(name).is_absolute(),'Expected relative base_generator path')
    base_path=(plan_path.parent/name).resolve();base=seeds.load(base_path)
    seeds.require(base['scenario']['crashes']==[] and base['scenario']['blackouts']==[],'Base must have no crash/blackout')
    profiles=named_rows(plan['performance_profiles'],{'id','nodes'});capacity=None;node_ids=None
    for profile in profiles:
        nodes=profile['nodes']
        seeds.require(isinstance(nodes,list) and len(nodes)==4,'Require exactly four nodes per profile')
        seeds.require(all(isinstance(n,dict) and set(n)=={'id','performance_work_per_second'} and
                          type(n['id']) is int and 0<n['id']<=seeds.MAX_SEED and
                          number(n['performance_work_per_second']) and n['performance_work_per_second']>0 for n in nodes),'Invalid node/performance')
        ids={n['id'] for n in nodes};total=sum(n['performance_work_per_second'] for n in nodes)
        seeds.require(len(ids)==4,'Duplicate node IDs')
        if node_ids is None:node_ids=ids;capacity=total
        seeds.require(ids==node_ids and total==capacity,'Profiles must retain node IDs and total capacity')
    count=base['tasks']['count'];first=base['tasks']['first_arrival_ticks'];horizon=base['scenario']['horizon_ticks']
    seeds.require(type(count) is int and 0<count<=seeds.MAX_SEED and ticks(first) and ticks(horizon,True),'Invalid task count/horizon')
    loads=named_rows(plan['loads'],{'id','period_ticks'})
    for load in loads:
        period=load['period_ticks']
        seeds.require(ticks(period,True) and first+(count-1)*period<=horizon,'Load arrivals exceed horizon or invalid period')
    networks=named_rows(plan['networks'],{'id','min_delay_ticks','max_delay_ticks','loss_probability'})
    for network in networks:
        lo=network['min_delay_ticks'];hi=network['max_delay_ticks'];q=network['loss_probability']
        seeds.require(ticks(lo) and ticks(hi) and lo<=hi and number(q) and 0<=q<=1,'Invalid network range/probability')
    modes=named_rows(plan['modes'],{'id','crashes'})
    for mode in modes:
        seeds.require(isinstance(mode['crashes'],list),'Expected explicit crashes')
        crashed=set()
        for crash in mode['crashes']:
            seeds.require(isinstance(crash,dict) and set(crash)=={'node_id','at_ticks'} and type(crash['node_id']) is int and
                          crash['node_id'] in node_ids and crash['node_id'] not in crashed and ticks(crash['at_ticks']) and crash['at_ticks']<=horizon,'Invalid crash')
            crashed.add(crash['node_id'])
            lost=[next(n['performance_work_per_second'] for n in p['nodes'] if n['id']==crash['node_id']) for p in profiles]
            seeds.require(all(p==lost[0] for p in lost),'Crash must remove equal capacity across profiles')
    cases=[]
    for profile in profiles:
        for load in loads:
            for network in networks:
                for mode in modes:
                    config=copy.deepcopy(base)
                    config['scenario'].update(nodes=copy.deepcopy(profile['nodes']),crashes=copy.deepcopy(mode['crashes']))
                    config['tasks']['period_ticks']=load['period_ticks']
                    config['delivery']={k:v for k,v in network.items() if k!='id'}
                    cases.append({'id':'_'.join([profile['id'],load['id'],network['id'],mode['id']]),
                                  'performance_profile':profile['id'],'load_id':load['id'],'network_profile':network['id'],
                                  'mode':mode['id'],'arrival_period_ticks':load['period_ticks'],
                                  'total_performance_work_per_second':capacity,'config':config})
    seeds.require(len({c['id'] for c in cases})==len(cases),'Ambiguous case IDs')
    return plan,base_path,base,cases


def csv_rows(path):
    with path.open(newline='',encoding='utf-8') as stream:
        reader=csv.DictReader(stream);return reader.fieldnames,list(reader)


def verify_controls(out,plan,cases):
    work_by_seed={};traces={};detectors={};frozen=None;checked=0;generated=0
    for case in cases:
        directory=out/'cases'/case['id'];series=seeds.load(directory/'series_manifest.json');config=case['config']
        seeds.require(series['status']=='complete' and series['mode']=='generated' and series['seeds']==plan['seeds'] and
                      [e['seed'] for e in series['entries']]==plan['seeds'],'Incomplete/wrong child series')
        seeds.require(series['template_sha256']==seeds.sha(out/'templates'/(case['id']+'.json')),'Child template mismatch')
        calls=sum(c['argv'][1]=='generate' for c in series['commands']);generated+=calls
        seeds.require(calls==len(plan['seeds']) and all(c['returncode']==0 for c in series['commands']),'Unexpected generate count/failing CLI')
        for entry in series['entries']:
            seed=entry['seed'];scenario=seeds.load(seeds.contained(directory,entry['input_files']['scenario.json']))
            for name in seeds.INPUT_FILES:
                seeds.require(seeds.sha(seeds.contained(directory,entry['input_files'][name]))==entry['input_sha256'][name],'Child input checksum mismatch')
            seeds.require(seeds.load(seeds.contained(directory,entry['generation_config']))==dict(config,seed=seed),'Generation configuration changed')
            expected=dict(config['scenario'],schema_version=1,delivery_trace='delivery.csv',tasks=scenario['tasks'])
            expected['nodes']=sorted(expected['nodes'],key=lambda n:n['id'])
            seeds.require(scenario==expected,'Generated fields differ from explicit config')
            tasks=scenario['tasks'];settings=config['tasks']
            seeds.require(len(tasks)==settings['count'] and all(t['id']==i+1 and
                          t['arrival_ticks']==settings['first_arrival_ticks']+i*case['arrival_period_ticks'] and
                          settings['work_min']<=t['work']<=settings['work_max'] for i,t in enumerate(tasks)),'Wrong generated arrivals/work bounds')
            work=[(t['id'],t['work']) for t in tasks]
            if seed in work_by_seed:seeds.require(work_by_seed[seed]==work,'Task work changed across study cells')
            else:work_by_seed[seed]=work
            common={k:v for k,v in scenario.items() if k not in ('nodes','tasks','crashes')}
            if frozen is None:frozen=common
            seeds.require(common==frozen,'Non-study scenario fields changed')
            key=(seed,case['network_profile']);checksum=entry['input_sha256']['delivery.csv']
            if key in traces:seeds.require(traces[key]==checksum,'Delivery changed with load/performance/crash mode')
            else:traces[key]=checksum
            header,trace=csv_rows(seeds.contained(directory,entry['input_files']['delivery.csv']))
            ids=sorted(n['id'] for n in scenario['nodes']);messages=scenario['horizon_ticks']//scenario['heartbeat_period_ticks']
            seeds.require(header==['node_id','heartbeat_seq','delay_ticks','lost'] and
                          [(int(r['node_id']),int(r['heartbeat_seq'])) for r in trace]==[(n,s) for n in ids for s in range(1,messages+1)],'Wrong trace coverage/order')
            network=config['delivery'];q=network['loss_probability']
            seeds.require(all(network['min_delay_ticks']<=int(r['delay_ticks'])<=network['max_delay_ticks'] and r['lost'] in ('0','1') for r in trace),'Trace outside configured bounds')
            if q in (0,1):seeds.require(all(r['lost']==str(int(q)) for r in trace),'Wrong deterministic loss mask')
            for variant in seeds.VARIANTS:
                run=directory/entry['batch_output']/variant
                seeds.require(seeds.load(run/'manifest.json')['status']=='complete','Incomplete variant')
                for name in seeds.INPUT_FILES:seeds.require(seeds.sha(run/'input'/name)==entry['input_sha256'][name],'Variant input mismatch')
                m=seeds.load(run/'metrics.json')
                seeds.require(all(m['invariants'].values()) and m['arrived']==len(tasks) and m['arrived']==m['completed']+m['unfinished'] and m['unfinished']==m['queued']+m['assigned'],'Task balance failed')
                seeds.require(m['crashes']==len(scenario['crashes']) and m['crashes']==m['detection_samples']+m['false_negatives']+m['crashes_after_false_exclusion'],'Crash classification failed')
                _,nodes=csv_rows(run/'nodes.csv');view=[(r['node_id'],r['crash_ticks'],r['failed_ticks'],r['final_status'],r['classification']) for r in nodes]
                key=(seed,case['network_profile'],case['mode'],variant.split('_')[0])
                if key in detectors:seeds.require(detectors[key]==view,'Detector history changed with load/performance/scheduler')
                else:detectors[key]=view
                for name,h in entry['results_sha256'][variant].items():seeds.require(seeds.sha(run/name)==h,'Result fingerprint mismatch')
            checked+=1
    return {'input_realizations_checked':checked,'variant_runs_checked':checked*4,'generation_calls':generated,
            'paired_work_sets':len(work_by_seed),'paired_trace_sets':len(traces),'detector_history_groups':len(detectors),
            'four_nodes_equal_total_capacity_and_equal_crash_capacity':True,'task_work_identical_across_cells':True,
            'only_explicit_arrival_times_change_across_loads':True,'trace_identical_across_load_performance_modes':True,
            'detector_history_invariant_to_load_performance_scheduler':True,'same_inputs_for_all_variants':True,
            'all_task_balances_valid':True,'crash_classification_counts_valid':True,'all_content_fingerprints_valid':True}


def collate(out,cases,filename):
    fields=None;count=0
    with (out/('load_'+filename)).open('w',newline='',encoding='utf-8') as stream:
        for case in cases:
            header,rows=csv_rows(out/'cases'/case['id']/'batch'/filename)
            if fields is None:fields=header;writer=csv.DictWriter(stream,fieldnames=EXTRA+fields);writer.writeheader()
            seeds.require(header==fields,'Native batch CSV schemas differ')
            metadata={k:case[k] for k in EXTRA if k not in ('case_id','network_mode')};metadata.update(case_id=case['id'],network_mode=case['mode'])
            for row in rows:writer.writerow({**metadata,**row});count+=1
    return count


def execute(args):
    plan_path=args.plan.expanduser().resolve();plan,base_path,base,cases=prepare(plan_path)
    program=args.program.expanduser().resolve();seeds.require(program.is_file() and os.access(program,os.X_OK),'Executable unavailable')
    requested=args.out.expanduser();seeds.require(not requested.exists() and not requested.is_symlink(),'Output exists; choose a new directory')
    out=requested.resolve();out.mkdir(parents=True)
    for folder in ['templates','cases','logs']:(out/folder).mkdir()
    shutil.copyfile(plan_path,out/'plan.json');shutil.copyfile(base_path,out/'base_generator.json')
    watched={p:seeds.sha(p) for p in (plan_path,base_path,program,Path(__file__).resolve(),Path(seeds.__file__).resolve())}
    state={'schema_version':1,'status':'incomplete','plan_sha256':watched[plan_path],'base_sha256':watched[base_path],
           'program':str(program),'program_sha256':watched[program],'driver_sha256':watched[Path(__file__).resolve()],
           'seed_runner_sha256':watched[Path(seeds.__file__).resolve()],'seeds':plan['seeds'],'cases':[],'commands':[],
           'note':'Four nodes, explicit finite arrivals and equal total/lost capacity. Native generate/batch only. Work/delivery pairing is checked; cells sharing seeds are not independent repeats.'}
    manifest=out/'load_manifest.json'
    try:
        for index,case in enumerate(cases):
            template=out/'templates'/(case['id']+'.json');seeds.save(template,case['config'])
            cmd=[sys.executable,str(Path(seeds.__file__).resolve()),'--template',str(template),'--seeds',*map(str,plan['seeds']),
                 '--scenario-id',case['id'],'--program',str(program),'--out',str(out/'cases'/case['id'])]
            record={'argv':cmd,'returncode':None,'stdout':'logs/'+case['id']+'.stdout.log','stderr':'logs/'+case['id']+'.stderr.log'}
            state['commands'].append(record);seeds.save(manifest,state);started=time.perf_counter()
            with (out/record['stdout']).open('w',encoding='utf-8') as stdout,(out/record['stderr']).open('w',encoding='utf-8') as stderr:
                r=subprocess.run(cmd,cwd=out,stdout=stdout,stderr=stderr,text=True)
            record.update(returncode=r.returncode,seconds=time.perf_counter()-started)
            seeds.require(r.returncode==0,'Load series failed; see '+str(out/record['stderr']))
            state['cases'].append({k:v for k,v in case.items() if k!='config'});seeds.save(manifest,state)
            print('[%d/%d] %s complete'%(index+1,len(cases),case['id']),flush=True)
        state['checks']=verify_controls(out,plan,cases)
        state['comparison_rows']=collate(out,cases,'comparison.csv');state['summary_rows']=collate(out,cases,'summary.csv')
        for p,h in watched.items():seeds.require(seeds.sha(p)==h,'Source/program changed during experiment')
        state['status']='complete';seeds.save(manifest,state)
    except BaseException as error:
        state['status']='failed';state['error']=str(error);seeds.save(manifest,state);raise
    print('Saved:',out,'; runs:',state['checks']['variant_runs_checked'],'; generate calls:',state['checks']['generation_calls'])
    print('Native summaries retained. Finite burst load is not steady utilization or proof of superiority.')
    return 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',type=Path,required=True);parser.add_argument('--program',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    try:return execute(args)
    except KeyboardInterrupt:print('Interrupted; partial series marked failed.',file=sys.stderr);return 130
    except (OSError,ValueError,KeyError,TypeError,ArithmeticError,subprocess.SubprocessError) as error:
        print('Load series failed: '+str(error),file=sys.stderr);return 1

if __name__=='__main__':sys.exit(main())
