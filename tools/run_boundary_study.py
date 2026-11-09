#!/usr/bin/env python3
"""Stage 9: explicit deterministic heartbeat boundaries and sigma=0, native batch only."""
import argparse
import copy
import csv
from decimal import Decimal
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode=True
import run_seed_series as io

FAMILIES=('initial_boundaries','warmed_boundaries','reordered_boundary','regular_sigma_zero',
          'zero_intervals','sigma_zero_gap','horizon_boundary','rounded_boundary')


def prepare(path):
    p=io.load(path)
    io.require(isinstance(p,dict) and set(p)=={'schema_version','heartbeat_period_ticks','offsets_ticks','sigma_coefficients','sigma_windows','families'} and type(p['schema_version']) is int and p['schema_version']==1,'Invalid boundary plan schema')
    io.require(type(p['heartbeat_period_ticks']) is int and p['heartbeat_period_ticks']==10000,'This explicit study requires H=10000 us')
    io.require(p['offsets_ticks']==[-1,0,1] and all(type(v) is int for v in p['offsets_ticks']),'Expected exact offsets -1/0/+1 ticks')
    values=p['sigma_coefficients'];sizes=p['sigma_windows'];families=p['families']
    io.require(isinstance(values,list) and values and all(type(v) in (int,Decimal) and (not isinstance(v,Decimal) or v.is_finite()) and v>=0 for v in values) and len(set(values))==len(values),'Expected distinct finite c>=0')
    io.require(isinstance(sizes,list) and sizes and all(type(v) is int and 2<=v<=io.MAX_SEED for v in sizes) and len(set(sizes))==len(sizes),'Expected distinct M>=2')
    io.require(isinstance(families,list) and families and all(isinstance(f,str) and f in FAMILIES for f in families) and len(set(families))==len(families),'Unknown/duplicate boundary family')
    return p


def base(horizon=160000,period=10000,tau=20000,c=2,window=3,crash=None,work=140000,kappa=2):
    return {'schema_version':1,'tick_ns':1000,'horizon_ticks':horizon,'heartbeat_period_ticks':period,'tau0_ticks':tau,'kappa':kappa,
            'adaptive':{'window':window,'min_intervals':2,'c':c},
            'nodes':[{'id':1,'performance_work_per_second':1000000},{'id':2,'performance_work_per_second':1000000}],
            'tasks':[{'id':1,'arrival_ticks':0,'work':work},{'id':2,'arrival_ticks':0,'work':1 if period==3 else 10000}],
            'crashes':[] if crash is None else [{'node_id':1,'at_ticks':crash}],'blackouts':[],
            'measurement_windows':[{'start_ticks':0,'end_ticks':horizon}],'delivery_trace':'delivery.csv'}


def token(value):return str(value).lower().replace('.','p').replace('+','').replace('-','m')
def offset_name(value):return 'minus1' if value==-1 else ('exact' if value==0 else 'plus1')


def build_cases(plan):
    cases=[];h=plan['heartbeat_period_ticks']
    def add(name,family,scenario,arrivals,meta):
        trace=[]
        for node in [1,2]:
            for seq in range(1,scenario['horizon_ticks']//scenario['heartbeat_period_ticks']+1):
                at=seq*scenario['heartbeat_period_ticks'] if node==2 else arrivals.get(seq)
                io.require(at is None or at>=seq*scenario['heartbeat_period_ticks'],'Fixture would deliver before send')
                trace.append({'node_id':node,'heartbeat_seq':seq,'delay_ticks':0 if at is None else at-seq*scenario['heartbeat_period_ticks'],'lost':int(at is None)})
        cases.append({'id':name,'family':family,'scenario':scenario,'trace':trace,**meta})
    if 'initial_boundaries' in plan['families']:
        for boundary,deadline in [('suspect',2*h),('failed',4*h)]:
            for delta in plan['offsets_ticks']:
                at=deadline+delta;scenario=base();arrivals={s:at+(s-1)*h for s in range(1,17)}
                add('initial_'+boundary+'_'+offset_name(delta),'initial_boundaries',scenario,arrivals,
                    {'target':'both','boundary':boundary,'deadline':deadline,'offset_ticks':delta,'probe_seq':1,'probe_ticks':at,'old_generation':0,'last_contact':0,
                     'old_suspect':{'fixed':2*h,'adaptive':2*h},'old_failed':{'fixed':4*h,'adaptive':4*h},
                     'expected_failed':{d:4*h if at>4*h else None for d in ['fixed','adaptive']}})
    if 'warmed_boundaries' in plan['families']:
        for target,s,f in [('fixed',9*h,12*h),('adaptive',8*h,10*h)]:
            for boundary,deadline in [('suspect',s),('failed',f)]:
                for delta in plan['offsets_ticks']:
                    at=deadline+delta;scenario=base(tau=3*h);arrivals={1:2*h,2:4*h,3:6*h}
                    arrivals.update({seq:at+(seq-4)*2*h for seq in range(4,17)})
                    add('warmed_'+target+'_'+boundary+'_'+offset_name(delta),'warmed_boundaries',scenario,arrivals,
                        {'target':target,'boundary':boundary,'deadline':deadline,'offset_ticks':delta,'probe_seq':4,'probe_ticks':at,'old_generation':3,'last_contact':6*h,
                         'old_suspect':{'fixed':9*h,'adaptive':8*h},'old_failed':{'fixed':12*h,'adaptive':10*h},
                         'expected_failed':{'fixed':12*h if at>12*h else None,'adaptive':10*h if at>10*h else None},
                         'sigma_checkpoint':{'seq':3,'at_ticks':6*h,'mean_ticks':2*h,'suspect_ticks':2*h,'failed_ticks':4*h}})
    if 'reordered_boundary' in plan['families']:
        for valid in [True,False]:
            for delta in plan['offsets_ticks']:
                at=9*h+delta;seq=5 if valid else 2;arrivals={1:h,3:3*h,4:5*h,seq:at}
                arrivals.update({s:at+(s-5)*2*h for s in range(6,17)})
                add('reordered_'+('fresh_' if valid else 'stale_')+offset_name(delta),'reordered_boundary',base(tau=5*h),arrivals,
                    {'target':'adaptive','boundary':'failed','deadline':9*h,'offset_ticks':delta,'probe_seq':seq,'probe_ticks':at,'old_generation':3,'last_contact':5*h,'stale_probe':not valid,
                     'old_suspect':{'fixed':10*h,'adaptive':7*h},'old_failed':{'fixed':15*h,'adaptive':9*h},
                     'expected_failed':{'fixed':None,'adaptive':9*h if not valid or delta>0 else None},
                     'sigma_checkpoint':{'seq':4,'at_ticks':5*h,'mean_ticks':2*h,'suspect_ticks':2*h,'failed_ticks':4*h}})
    if 'regular_sigma_zero' in plan['families']:
        for c in plan['sigma_coefficients']:
            for size in plan['sigma_windows']:
                for crashed in [False,True]:
                    scenario=base(horizon=24*h,c=c,window=size,crash=65000 if crashed else None,work=80000)
                    add('regular_c'+token(c)+'_m'+str(size)+'_'+('crash' if crashed else 'healthy'),'regular_sigma_zero',scenario,{s:s*h for s in range(1,25)},
                        {'mode':'crash' if crashed else 'healthy','expected_failed':{'fixed':100000 if crashed else None,'adaptive':80000 if crashed else None},
                         'sigma_checkpoint':{'seq':3,'at_ticks':3*h,'mean_ticks':h,'suspect_ticks':h,'failed_ticks':2*h}})
    if 'zero_intervals' in plan['families']:
        for c in plan['sigma_coefficients']:
            add('zero_intervals_c'+token(c),'zero_intervals',base(horizon=5*h+1,tau=4*h,c=c,window=2),{1:3*h,2:3*h,3:3*h},
                {'expected_failed':{'fixed':None,'adaptive':5*h},'sigma_checkpoint':{'seq':3,'at_ticks':3*h,'mean_ticks':0,'suspect_ticks':h,'failed_ticks':2*h}})
    if 'sigma_zero_gap' in plan['families']:
        for c in plan['sigma_coefficients']:
            for delta in plan['offsets_ticks']:
                at=5*h+delta;arrivals={1:h,2:2*h,3:3*h};arrivals.update({s:at+(s-4)*h for s in range(4,17)})
                add('zero_gap_c'+token(c)+'_'+offset_name(delta),'sigma_zero_gap',base(c=c),arrivals,
                    {'target':'adaptive','boundary':'failed','deadline':5*h,'offset_ticks':delta,'probe_seq':4,'probe_ticks':at,'old_generation':3,'last_contact':3*h,
                     'old_suspect':{'fixed':5*h,'adaptive':4*h},'old_failed':{'fixed':7*h,'adaptive':5*h},
                     'expected_failed':{'fixed':None,'adaptive':5*h if delta>0 else None},
                     'sigma_checkpoint':{'seq':3,'at_ticks':3*h,'mean_ticks':h,'suspect_ticks':h,'failed_ticks':2*h}})
    if 'horizon_boundary' in plan['families']:
        for delta in plan['offsets_ticks']:
            add('horizon_'+offset_name(delta),'horizon_boundary',base(horizon=4*h+delta),{},
                {'horizon_offset_ticks':delta,'expected_failed':{d:4*h if delta>=0 else None for d in ['fixed','adaptive']}})
    if 'rounded_boundary' in plan['families']:
        for delta in plan['offsets_ticks']:
            at=13+delta;arrivals={1:3,2:6,3:9};arrivals.update({s:at+(s-4)*3 for s in range(4,14)})
            add('rounded_'+offset_name(delta),'rounded_boundary',base(horizon=40,period=3,tau=6,kappa=Decimal('1.01'),work=30),arrivals,
                {'target':'adaptive','boundary':'failed','deadline':13,'offset_ticks':delta,'probe_seq':4,'probe_ticks':at,'old_generation':3,'last_contact':9,
                 'old_suspect':{'fixed':15,'adaptive':12},'old_failed':{'fixed':16,'adaptive':13},
                 'expected_failed':{'fixed':None,'adaptive':13 if delta>0 else None},
                 'sigma_checkpoint':{'seq':3,'at_ticks':9,'mean_ticks':3,'suspect_ticks':3,'failed_ticks':4}})
    io.require(cases and len({c['id'] for c in cases})==len(cases),'Empty/duplicate fixtures')
    return cases


def read_json(path):return json.loads(path.read_text(encoding='utf-8'))
def csv_rows(path):
    with path.open(newline='',encoding='utf-8') as f:return list(csv.DictReader(f))


def verify_case(case,variant,run):
    detector=variant.split('_')[0];scenario=case['scenario'];expected=case['expected_failed'][detector]
    events=[json.loads(line) for line in (run/'events.jsonl').read_text().splitlines()]
    heartbeats=[e for e in events if e['kind']=='heartbeat_observed' and e['message']['node_id']==1]
    timers=[e for e in events if e['kind']=='timeout_observed' and e['request']['node_id']==1]
    failed=[e for e in timers if e['decision']=='applied' and e['after']=='failed']
    io.require([e['time_ticks'] for e in failed]==([] if expected is None else [expected]),'Unexpected FAILED timeline: '+case['id']+' '+variant)
    m=read_json(run/'metrics.json');nodes=csv_rows(run/'nodes.csv');node=next(r for r in nodes if r['node_id']=='1')
    io.require(all(m['invariants'].values()) and m['arrived']==2 and m['arrived']==m['completed']+m['unfinished'] and m['unfinished']==m['queued']+m['assigned'],'Native balance failed')
    physical=bool(scenario['crashes']);io.require(m['crashes']==int(physical) and m['false_positives']==int(expected is not None and not physical) and m['false_negatives']==0 and m['crashes_after_false_exclusion']==0,'Unexpected classification')
    io.require(node['failed_ticks']==('' if expected is None else str(expected)),'Node FAILED time differs')
    checked=0;probe=None;equal_timeout=None
    if 'probe_seq' in case:
        probe=next(e for e in heartbeats if e['message']['heartbeat_seq']==case['probe_seq']);at=case['probe_ticks']
        old_f=case['old_failed'][detector];stale=case.get('stale_probe',False)
        decision='already_failed' if at>old_f else ('non_increasing_sequence' if stale else 'accepted')
        io.require(probe['time_ticks']==at and probe['phase']==4 and probe['decision']==decision,'Probe timing/decision mismatch')
        accepted=decision=='accepted'
        io.require(probe['generation']==case['old_generation']+int(accepted) and probe['contact_ticks']==(at if accepted else case['last_contact']),'Probe changed contact/generation incorrectly')
        io.require(probe['after']==('alive' if accepted else ('failed' if decision=='already_failed' else 'suspect' if at>case['old_suspect'][detector] else 'alive')),'Probe status mismatch')
        if case['offset_ticks']==0 and case['target'] in ('both',detector):
            timeout=next(e for e in timers if e['request']['at_ticks']==case['deadline'] and e['request']['kind']==case['boundary'] and e['request']['generation']==case['old_generation'])
            equal_timeout=timeout
            io.require(probe['record_no']<timeout['record_no'] and timeout['phase']==5,'Equal-time delivery must precede timeout')
            io.require(timeout['decision']==('stale_generation' if accepted else 'applied'),'Wrong equal-time timeout decision');checked+=1
        if case['boundary']=='suspect' and case['target'] in ('both',detector):
            applied=[e for e in timers if e['request']['at_ticks']==case['deadline'] and e['request']['kind']=='suspect' and e['request']['generation']==case['old_generation'] and e['decision']=='applied']
            io.require(bool(applied)==(case['offset_ticks']>0),'SUSPECT boundary mismatch')
            if case['offset_ticks']>0:io.require(probe['before']=='suspect' and probe['after']=='alive','SUSPECT must recover')
    if 'sigma_checkpoint' in case and detector=='adaptive':
        cp=case['sigma_checkpoint'];hb=next(e for e in heartbeats if e['message']['heartbeat_seq']==cp['seq'])
        th=hb['thresholds']
        io.require(hb['time_ticks']==cp['at_ticks'] and th['warmed_up'] and Decimal(th['mean_ticks_decimal'])==cp['mean_ticks'] and Decimal(th['sample_std_ticks_decimal'])==0 and th['suspect_ticks']==cp['suspect_ticks'] and th['failed_ticks']==cp['failed_ticks'],'Sigma=0 checkpoint mismatch')
        previous=[e for e in heartbeats if e['record_no']<hb['record_no'] and e['decision']=='accepted']
        io.require(len(previous)==2 and previous[0]['thresholds']['mean_ticks_decimal'] is None and previous[0]['thresholds']['sample_std_ticks_decimal'] is None and previous[1]['thresholds']['sample_std_ticks_decimal'] is None and not previous[0]['thresholds']['warmed_up'] and previous[0]['thresholds']['interval_count']==0 and not previous[1]['thresholds']['warmed_up'] and previous[1]['thresholds']['interval_count']==1,'Artificial interval or premature warmup')
        if case['family']=='rounded_boundary':io.require(3<Decimal(th['raw_failed_ticks_decimal'])<4,'Fractional raw F not exercised')
    if case['family']=='regular_sigma_zero':
        if detector=='adaptive':
            for e in heartbeats:
                if e['thresholds']['warmed_up']:
                    io.require(Decimal(e['thresholds']['mean_ticks_decimal'])==10000 and Decimal(e['thresholds']['sample_std_ticks_decimal'])==0 and e['thresholds']['suspect_ticks']==10000 and e['thresholds']['failed_ticks']==20000,'Regular sigma=0 thresholds changed')
        io.require(m['completed']==2 and m['unfinished']==0,'Regular control tasks not complete')
        if physical:
            io.require(m['mean_detection']['value_ticks']==(35000 if detector=='fixed' else 15000) and m['mean_recovery']['value_ticks']==(115000 if detector=='fixed' else 95000),'Regular crash Detection/Recovery mismatch')
    if case['family']=='horizon_boundary' and expected is None:io.require(node['final_status']=='suspect','Pre-F horizon status wrong')
    if expected is not None:
        for e in heartbeats:
            if e['time_ticks']>expected:io.require(e['decision']=='already_failed' and e['after']=='failed','FAILED was not terminal')
        if not physical and scenario['horizon_ticks']>=scenario['tasks'][0]['work']:
            io.require(m['retries']==1 and m['rejected_revoked_results']==1,'Revoked original result must be rejected exactly once')
        reconfig=[e for e in events if e['kind']=='reconfiguration' and e['time_ticks']==expected]
        io.require(len(reconfig)==1 and reconfig[0]['phase']==6 and 1 in reconfig[0]['newly_failed'],'Missing phase-6 reconfiguration')
    return {'case_id':case['id'],'family':case['family'],'variant':variant,'failed_ticks':expected,
            'false_positives':m['false_positives'],'completed':m['completed'],'unfinished':m['unfinished'],
            'detection_ticks':m['mean_detection']['value_ticks'],'recovery_ticks':m['mean_recovery']['value_ticks'],
            'equal_time_phase_checks':checked,'probe_ticks':None if probe is None else probe['time_ticks'],
            'probe_decision':None if probe is None else probe['decision'],'probe_before':None if probe is None else probe['before'],
            'probe_after':None if probe is None else probe['after'],'probe_generation':None if probe is None else probe['generation'],
            'equal_timeout_decision':None if equal_timeout is None else equal_timeout['decision'],
            'retries':m['retries'],'rejected_revoked_results':m['rejected_revoked_results']}


def write_csv(path,rows):
    with path.open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)


def execute(args):
    plan_path=args.plan.expanduser().resolve();plan=prepare(plan_path);cases=build_cases(plan)
    program=args.program.expanduser().resolve();io.require(program.is_file() and os.access(program,os.X_OK),'Executable unavailable')
    requested=args.out.expanduser();io.require(not requested.exists() and not requested.is_symlink(),'Output exists; choose a new directory')
    out=requested.resolve();out.mkdir(parents=True);(out/'inputs').mkdir();(out/'logs').mkdir();shutil.copyfile(plan_path,out/'plan.json')
    watched={p:io.sha(p) for p in [plan_path,program,Path(__file__).resolve(),Path(io.__file__).resolve()]}
    state={'schema_version':1,'status':'incomplete','plan_sha256':watched[plan_path],'program':str(program),'program_sha256':watched[program],
           'driver_sha256':watched[Path(__file__).resolve()],'source':io.source_info(Path(__file__).resolve().parents[1]),
           'case_note':'Explicit deterministic fixtures, not independent stochastic seed repetitions','cases':[],'commands':[]}
    manifest=out/'boundary_manifest.json'
    def call(argv):
        i=len(state['commands'])+1;record={'argv':[str(program),*map(str,argv)],'stdout':f'logs/{i:04d}.stdout.log','stderr':f'logs/{i:04d}.stderr.log','returncode':None}
        state['commands'].append(record);io.save(manifest,state)
        with (out/record['stdout']).open('w') as stdout,(out/record['stderr']).open('w') as stderr:r=subprocess.run(record['argv'],cwd=out,stdout=stdout,stderr=stderr,text=True)
        record['returncode']=r.returncode;io.save(manifest,state);io.require(r.returncode==0,'Native CLI failed; see '+str(out/record['stderr']))
        return out/record['stdout']
    try:
        io.save(manifest,state);state['build_info']=io.load(call(['--build-info']))
        runs=[]
        for case in cases:
            directory=out/'inputs'/case['id'];directory.mkdir();io.save(directory/'scenario.json',case['scenario']);write_csv(directory/'delivery.csv',case['trace'])
            case['input_files']={n:str((directory/n).relative_to(out)) for n in io.INPUT_FILES}
            case['input_sha256']={n:io.sha(directory/n) for n in io.INPUT_FILES}
            call(['validate','--scenario',directory/'scenario.json']);runs.append({'scenario_id':case['id'],'repeat_id':'deterministic_1','scenario':case['input_files']['scenario.json']})
            state['cases'].append({k:v for k,v in case.items() if k not in ('scenario','trace')})
        io.save(out/'batch_input.json',{'schema_version':1,'runs':runs});call(['batch','--manifest',out/'batch_input.json','--out',out/'batch'])
        batch=io.load(out/'batch/batch_manifest.json');io.require(batch['status']=='complete' and len(batch['runs'])==len(cases),'Incomplete batch')
        mapping={(r['scenario_id'],r['repeat_id']):r for r in batch['runs']};checks=[];sigma_c={};sigma_m={};c_comparisons=0;outcome_comparisons=0
        for case,record in zip(cases,state['cases']):
            relative=Path('batch')/mapping[case['id'],'deterministic_1']['output'];directory=io.contained(out,str(relative/'comparison_manifest.json')).parent
            io.require(io.load(directory/'comparison_manifest.json')['status']=='complete','Incomplete compare');record['batch_output']=str(relative);record['results_sha256']={}
            for name in io.INPUT_FILES:io.require(io.sha(out/case['input_files'][name])==case['input_sha256'][name],'Batch changed source input')
            for variant in io.VARIANTS:
                run=directory/variant;io.require(io.load(run/'manifest.json')['status']=='complete','Incomplete variant')
                for name in io.INPUT_FILES:io.require(io.sha(run/'input'/name)==case['input_sha256'][name],'Variant input mismatch')
                fingerprints={n:io.sha(run/n) for n in io.RESULT_FILES};record['results_sha256'][variant]=fingerprints
                checks.append(verify_case(case,variant,run))
                if case['family']=='regular_sigma_zero':
                    key=(case['mode'],case['scenario']['adaptive']['window'],variant)
                    if key in sigma_c:
                        io.require(sigma_c[key]==fingerprints,'c changed regular sigma=0 content');c_comparisons+=1
                    else:sigma_c[key]=fingerprints
                    key=(case['mode'],variant);outcomes={k:v for k,v in fingerprints.items() if k!='events.jsonl'}
                    if key in sigma_m:
                        io.require(sigma_m[key]==outcomes,'M changed regular sigma=0 outcomes');outcome_comparisons+=1
                    else:sigma_m[key]=outcomes
            print('Verified',case['id'],flush=True)
        write_csv(out/'boundary_checks.csv',checks)
        state['checks']={'case_count':len(cases),'variant_runs':len(checks),'equal_time_phase_checks':sum(r['equal_time_phase_checks'] for r in checks),
                         'sigma_zero_c_content_invariant':True if c_comparisons else None,'sigma_zero_m_outcomes_invariant':True if outcome_comparisons else None,
                         'sigma_c_comparisons':c_comparisons,'sigma_outcome_comparisons':outcome_comparisons,'all_oracles_passed':True}
        for p,h in watched.items():io.require(io.sha(p)==h,'Source/program changed during study')
        report=['# Граничные времена heartbeat и σ=0','',
                f"Детерминированные сценарии: {len(cases)}; четыре сочетания: {len(checks)} запусков. Это не независимые seed-повторы.",
                '', 'Выполненные семейства: '+', '.join(plan['families'])+'.',
                '', 'Ручные ожидания проверены по native-журналам. Подробности: boundary_checks.csv и boundary_manifest.json; события — batch/runs/*/*/events.jsonl.',
                '', '| Наблюдение | Подтверждённое поведение |','|---|---|']
        rules=[(('initial_boundaries','warmed_boundaries'), 'S/F±1 тик', 'Равенство доставки/таймера отдаёт приоритет фазе 4; SUSPECT восстанавливается, FAILED окончателен'),
               (('reordered_boundary',), 'Старый номер ровно на F','Контакт не меняется; таймер текущего поколения применяет FAILED'),
               (('regular_sigma_zero','sigma_zero_gap'), 'σ=0, μ=H','S=H, F=κH; c не создаёт запаса до возмущения'),
               (('zero_intervals',),'Три получения в одной отметке','Два реальных нулевых интервала, μ=σ=0, S ограничен снизу H'),
               (('horizon_boundary',),'Событие на горизонте','Срок на горизонте включён, строго за ним не выполняется'),
               (('rounded_boundary',),'Дробный raw F','Для H=3 и κ=1.01 raw F≈3.03 округляется до F=4 тиков')]
        for families,label,meaning in rules:
            if any(f in plan['families'] for f in families):report.append('| '+label+' | '+meaning+' |')
        if 'regular_sigma_zero' in plan['families']:
            report+=['','В регулярном crash последний контакт в 60 мс, crash в 65 мс: Detection adaptive/fixed = 15/35 мс; Recovery = 95/115 мс. Оба задания завершены.']
        report+=['','Сценарии искусственные и проверяют семантику модели. Вероятности редких ошибок и универсальные настройки по ним не оцениваются. Формулы/старые данные не менялись. Этап 10 не выполнялся.']
        (out/'report.md').write_text('\n'.join(report)+'\n',encoding='utf-8')
        state['status']='complete';io.save(manifest,state)
    except BaseException as error:
        state['status']='failed';state['error']=str(error);io.save(manifest,state);raise
    print('Saved boundary study:',out,state['checks']);return 0


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--program',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    try:return execute(args)
    except KeyboardInterrupt:print('Interrupted; partial study marked failed.',file=sys.stderr);return 130
    except (OSError,ValueError,KeyError,TypeError,ArithmeticError,subprocess.SubprocessError) as error:
        print('Boundary study failed: '+str(error),file=sys.stderr);return 1

if __name__=='__main__':sys.exit(main())
