"""Repeat all chapter-4 experiments; Python standard library, no shell commands.
The expected rounded values below were transcribed from chapter 4 tables 4/5.
Inputs and archived results remain read-only. --out must not already exist.
"""
from pathlib import Path
from decimal import Decimal, ROUND_HALF_UP
from fractions import Fraction
import argparse, csv, hashlib, json, math, platform, shutil, subprocess, time

CASES = ['A_reference','B_crash','C_heartbeat_gap','D_heterogeneous']
VARIANTS = ['fixed_rr','fixed_ll','adaptive_rr','adaptive_ll']
CANONICAL = ['manifest.json','events.jsonl','metrics.json','tasks.csv','attempts.csv','nodes.csv']
# mean latency ms, P95 ms, completed, retries, FAILED ms, Detection ms, Recovery ms, FP
EXPECTED = {
 'A_reference': [('17.00',22,12,0,None,None,None,0),('18.58',29,12,0,None,None,None,0),('17.00',22,12,0,None,None,None,0),('18.58',29,12,0,None,None,None,0)],
 'B_crash': [('30.09',83,11,3,70,35,None,0),('32.27',91,11,3,70,35,None,0),('31.45',71,11,2,50,15,66,0),('33.64',79,11,2,50,15,69,0)],
 'C_heartbeat_gap': [('18.42',34,12,0,None,None,None,0),('19.92',37,12,0,None,None,None,0),('21.83',46,12,0,50,None,None,1),('23.58',49,12,0,50,None,None,1)],
 'D_heterogeneous': [('18.92',56,12,3,70,35,53,0),('11.33',46,12,1,70,35,41,0),('15.17',39,12,2,50,15,30,0),('10.92',29,12,1,50,15,24,0)]}


def read(p): return json.loads(p.read_text())
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def table(p):
    with p.open(newline='') as stream: return list(csv.DictReader(stream))
def ms(t): return None if t is None else Decimal(str(t))/1000
def rounded(t): return None if t is None else str(ms(t).quantize(Decimal('.01'),rounding=ROUND_HALF_UP))
def hashes(d,names=CANONICAL): return {n:sha(d/n) for n in names}
def ticks(v): return None if v is None else v*1000


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--materials',type=Path,default=Path(__file__).resolve().parent/'materials')
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();exe=args.program.resolve();materials=args.materials.resolve();out=args.out.resolve()
    if out.exists():raise SystemExit('Refusing to overwrite existing output: '+str(out))
    out.mkdir(parents=True);(out/'logs').mkdir()
    checks=[];rows=[];numeric_differences=[];calls=[];started=time.perf_counter()
    def check(condition,label,**details):
        checks.append({'check':label,'passed':bool(condition),**details})
        if not condition: print('FAIL:',label,details)
    def close(actual,expected,label):
        check(abs(Decimal(str(actual))-Decimal(expected.numerator)/Decimal(expected.denominator))<=Decimal('1e-8'),label)
    def call(tag,*arguments):
        before=time.perf_counter();r=subprocess.run([str(exe),*map(str,arguments)],capture_output=True,text=True,timeout=90,cwd=out)
        (out/'logs'/(tag+'.log')).write_text(r.stdout+'\nSTDERR:\n'+r.stderr)
        calls.append({'command':[str(exe),*map(str,arguments)],'returncode':r.returncode,'seconds':time.perf_counter()-before})
        if r.returncode:raise RuntimeError('Command failed: '+tag+'\n'+r.stderr)
    material_hashes={str(p.relative_to(materials)):sha(p) for p in materials.rglob('*') if p.is_file()}
    build=json.loads(subprocess.check_output([str(exe),'--build-info'],text=True))
    for case in CASES:
        scenario=materials/'inputs'/case/'scenario.json';s=read(scenario);delivery=table(scenario.parent/'delivery.csv')
        check(s['tick_ns']==1000 and s['horizon_ticks']==120000 and s['heartbeat_period_ticks']==10000 and s['tau0_ticks']==20000 and s['kappa']==2 and s['adaptive']=={'window':3,'min_intervals':2,'c':2},case+': chapter common parameters')
        check(s['nodes']==[{'id':1,'performance_work_per_second':1000000},{'id':2,'performance_work_per_second':2000000 if case=='D_heterogeneous' else 1000000}],case+': chapter nodes/performance')
        check(s['tasks']==[{'id':i+1,'arrival_ticks':i*5000,'work':12000} for i in range(12)],case+': chapter tasks')
        check(s['crashes']==([{'node_id':1,'at_ticks':35000}] if case in ['B_crash','D_heterogeneous'] else []) and s['blackouts']==([{'node_id':1,'start_ticks':40000,'end_ticks':65000}] if case=='C_heartbeat_gap' else []) and s['measurement_windows']==[{'start_ticks':0,'end_ticks':120000}],case+': chapter violations/windows')
        check(len(delivery)==24 and {(int(r['node_id']),int(r['heartbeat_seq'])) for r in delivery}=={(n,q) for n in [1,2] for q in range(1,13)} and all(r['delay_ticks']=='0' and r['lost']=='0' for r in delivery),case+': chapter zero-delay full trace')
        call(case+'-validate','validate','--scenario',scenario)
        call(case+'-compare','compare','--scenario',scenario,'--out',out/'reproduced'/case)
        for index,v in enumerate(VARIANTS):
            name=case+'/'+v;d=out/'reproduced'/name;arch=materials/'results'/name
            m=read(d/'metrics.json');old=read(arch/'metrics.json');tasks=table(d/'tasks.csv');attempts=table(d/'attempts.csv');nodes=table(d/'nodes.csv')
            events=[json.loads(line) for line in (d/'events.jsonl').read_text().splitlines()]
            mean,p95,done,retries,failed,detection,recovery,fp=EXPECTED[case][index]
            # Chapter tables: rounded mean, exact integer durations, reasons and counts.
            check(rounded(m['mean_latency']['value_ticks'])==mean and m['p95_latency']['value_ticks']==ticks(p95),name+': table 5 latency/P95')
            check(m['arrived']==12 and m['completed']==done and m['unfinished']==12-done,name+': table 5 completion')
            check(m['retries']==retries and m['false_positives']==fp and m['false_negatives']==0,name+': retries/FP/FN')
            n1=next(n for n in nodes if n['node_id']=='1')
            check(n1['failed_ticks']==('' if failed is None else str(ticks(failed))),name+': table 4 FAILED')
            check(m['mean_detection']['value_ticks']==ticks(detection) and m['mean_recovery']['value_ticks']==ticks(recovery),name+': table 4 Detection/Recovery')
            expected_reason='recovery_incomplete' if case=='B_crash' and v.startswith('fixed') else 'not_applicable' if detection is None else ''
            check(n1['recovery_reason']==expected_reason,name+': Recovery null reason')
            # Independent arithmetic using exact integer CSV data, not stored metrics.
            completed=[r for r in tasks if r['accepted_ticks']!=''];lat=[int(r['accepted_ticks'])-int(r['arrival_ticks']) for r in completed]
            check(len(tasks)==12 and len({r['task_id'] for r in tasks})==12 and len(completed)==done,name+': unique task rows')
            close(m['mean_latency']['value_ticks'],Fraction(sum(lat),len(lat)),name+': independent exact mean')
            check(sorted(lat)[len(lat)-len(lat)//20-1]==m['p95_latency']['value_ticks'] and all(int(r['latency_ticks'])==int(r['accepted_ticks'])-int(r['arrival_ticks']) for r in completed),name+': independent P95/latency')
            retry=sum(int(r['attempts'])-1 for r in tasks)
            check(retry==retries and len(attempts)==12+retry,name+': independent retries')
            window=[r for r in completed if 0<int(r['accepted_ticks'])<=120000]
            close(m['throughput_windows'][0]['tasks_per_second'],Fraction(len(window)*1000000,120000),name+': independent throughput')
            check(all(m['invariants'].values()) and m['arrived']==m['completed']+m['unfinished'] and m['unfinished']==m['queued']+m['assigned'],name+': balances')
            accepts=[e for e in events if e['kind']=='result' and e['decision']=='accepted']
            check(len(accepts)==done and len({e['attempt']['task_id'] for e in accepts})==done and {e['attempt']['task_id']:e['time_ticks'] for e in accepts}=={int(r['task_id']):int(r['accepted_ticks']) for r in completed},name+': unique journal acceptance and CSV agreement')
            check(events[-1]['kind']=='run_completed' and events[-1]['time_ticks']==120000 and read(d/'manifest.json')['status']=='complete',name+': successful horizon')
            changes=[e for e in events if e['kind']=='reconfiguration' and 1 in e['newly_failed']]
            returned=changes[0]['revoked'] if changes else []
            check(len(returned)==retries,name+': returned set size (chapter scenarios)')
            if detection is not None:
                check(int(n1['failed_ticks'])-35000==ticks(detection),name+': independent Detection')
                end={int(r['task_id']):int(r['accepted_ticks']) for r in completed}
                ids=[r['task_id'] for r in returned]
                expected_recovery=max(end[i] for i in ids)-35000 if all(i in end for i in ids) else None
                check(expected_recovery==ticks(recovery),name+': independent fixed-set Recovery')
            # Compare actual execution to supplied archived execution.
            for f in ['events.jsonl','tasks.csv','attempts.csv','nodes.csv']:
                check(sha(d/f)==sha(arch/f),name+': archive byte equality '+f)
            for field in set(old)|set(m):
                if old.get(field)==m.get(field):continue
                if field not in ['mean_latency','mean_detection','mean_recovery']:
                    check(False,name+': unexpected metric discrepancy '+field);continue
                a=Decimal(str(old[field]['value_ticks']));b=Decimal(str(m[field]['value_ticks']))
                numeric_differences.append({'run':name,'field':field,'archive_ticks':str(a),'reproduced_ticks':str(b),'absolute_delta_ticks':str(abs(a-b))})
                check(old[field]['reason']==m[field]['reason'] and abs(a-b)<=Decimal('1e-8'),name+': only floating representation differs '+field)
            for f in ['scenario.json','delivery.csv']:
                check(sha(d/'input'/f)==sha(scenario.parent/f)==sha(arch/'input'/f),name+': identical input '+f)
            # Detailed narrative assertions from 4.2--4.4.
            ids=[r['task_id'] for r in returned];ends={int(r['task_id']):int(r['accepted_ticks']) if r['accepted_ticks'] else None for r in tasks}
            if case=='B_crash' and v.endswith('_rr'):
                expected_ids=[5,7,9] if v.startswith('fixed') else [5,7]
                check(ids==expected_ids and ends[5]==(101000 if v.startswith('fixed') else 89000) and ends[7]==(113000 if v.startswith('fixed') else 101000),name+': 4.2 returned IDs and completions')
                check([i for i,t in ends.items() if t is None]==([9] if v.startswith('fixed') else [12]),name+': 4.2/4.3 distinct unfinished tasks')
                if v.startswith('fixed'):
                    check(any(e['kind']=='task_assigned' and e['assignment']['task']['id']==9 and e['time_ticks']>35000 and e['assignment']['node_id']==1 for e in events),name+': assignment after real crash')
            h=[e for e in events if e['kind']=='heartbeat_observed' and e['message']['node_id']==1 and e['time_ticks']==30000]
            check(len(h)==1 and h[0]['thresholds']['interval_count']==(2 if v.startswith('adaptive') else 0) and h[0]['thresholds']['suspect_ticks']==(10000 if v.startswith('adaptive') else 20000) and h[0]['thresholds']['failed_ticks']==(20000 if v.startswith('adaptive') else 40000),name+': 4.3 thresholds after stable contacts')
            if case=='C_heartbeat_gap':
                if v.startswith('fixed'):
                    check(any(e['kind']=='heartbeat_observed' and e['message']['node_id']==1 and e['time_ticks']==70000 and e['before']=='suspect' and e['after']=='alive' for e in events) and n1['final_status']=='alive',name+': 4.3 heartbeat at deadline restores ALIVE')
                else:
                    check(ids==[] and n1['final_status']=='failed' and all(e['assignment']['node_id']==2 for e in events if e['kind']=='task_assigned' and e['time_ticks']>=50000),name+': 4.3 false exclusion, no returned work, only node 2 thereafter')
            if case=='A_reference':
                a4=next(r for r in attempts if r['task_id']=='4')
                check(a4['node_id']==('2' if v.endswith('_rr') else '1') and a4['accepted_ticks']==('29000' if v.endswith('_rr') else '36000'),name+': 4.4 task 4 assignment and finish')
                if v.endswith('_ll'):
                    current={}
                    for e in events:
                        if e['kind']=='task_assigned':
                            a=e['assignment'];i=a['task']['id']
                            if i==4:
                                work={1:0,2:0}
                                for n,amount in current.values():work[n]+=amount
                                check(work=={1:12000,2:12000} and e['time_ticks']==15000,name+': 4.4 equal full-work Load before tie')
                                break
                            current[i]=(a['node_id'],int(a['task']['work_decimal']))
                        elif e['kind']=='result' and e['decision']=='accepted':current.pop(e['attempt']['task_id'])
            rows.append({'scenario':case,'variant':v,'mean_latency_ms':rounded(m['mean_latency']['value_ticks']),'p95_ms':str(ms(m['p95_latency']['value_ticks'])),'completed':m['completed'],'unfinished':m['unfinished'],'FAILED_ms':str(ms(n1['failed_ticks'])) if n1['failed_ticks'] else None,'Detection_ms':str(ms(m['mean_detection']['value_ticks'])) if m['mean_detection']['value_ticks'] is not None else None,'Recovery_ms':str(ms(m['mean_recovery']['value_ticks'])) if m['mean_recovery']['value_ticks'] is not None else None,'FP':m['false_positives'],'FN':m['false_negatives'],'retries':retry,'throughput_tasks_per_second':m['throughput_windows'][0]['tasks_per_second']})
        # comparison.csv may use different long-double decimal rendering.
        actual=table(out/'reproduced'/case/'comparison.csv');previous=table(materials/'results'/case/'comparison.csv')
        check(len(actual)==len(previous)==4,case+': archive comparison row count')
        for i,(a,b) in enumerate(zip(actual,previous)):
            for key in a:
                if a[key]==b[key]:continue
                try:equivalent=abs(Decimal(a[key])-Decimal(b[key]))<=Decimal('1e-8')
                except Exception:equivalent=False
                check(equivalent,f'{case}: archive comparison row {i} field {key}')
    # Reverse variant order, then independently replay native and archived journals.
    for case in CASES:
        for v in reversed(VARIANTS):
            name=case+'/'+v;d=out/'reproduced'/name;repeat=out/'reverse_repeat'/name
            detector,scheduler=v.split('_')
            call(case+'-'+v+'-repeat','run','--scenario',materials/'inputs'/case/'scenario.json','--detector',detector,'--scheduler',scheduler,'--out',repeat)
            check(hashes(d)==hashes(repeat),name+': reverse-order canonical repeat')
            before=hashes(d);call(case+'-'+v+'-metrics','metrics','--run-dir',d)
            check(before==hashes(d),name+': independent metrics replay unchanged')
            archived=out/'archive_replay'/name;archived.parent.mkdir(parents=True,exist_ok=True);shutil.copytree(materials/'results'/name,archived)
            call(case+'-'+v+'-archive-metrics','metrics','--run-dir',archived)
            check(hashes(d,['metrics.json','tasks.csv','attempts.csv','nodes.csv'])==hashes(archived,['metrics.json','tasks.csv','attempts.csv','nodes.csv']),name+': archived journal replay equals fresh native result')
    # Chapter qualitative conclusions use actual independently recomputed values.
    lookup={(r['scenario'],r['variant']):r for r in rows}
    for case in ['A_reference','B_crash','C_heartbeat_gap','D_heterogeneous']:
        for detector in ['fixed','adaptive']:
            rr=lookup[case,detector+'_rr'];ll=lookup[case,detector+'_ll']
            check((Decimal(ll['mean_latency_ms'])<Decimal(rr['mean_latency_ms']))==(case=='D_heterogeneous'),case+'/'+detector+': 4.4 direction of LL/RR mean comparison')
    bfixed=lookup['B_crash','fixed_rr'];badapt=lookup['B_crash','adaptive_rr']
    check(Decimal(badapt['mean_latency_ms'])>Decimal(bfixed['mean_latency_ms']) and Decimal(badapt['p95_ms'])<Decimal(bfixed['p95_ms']), '4.3 faster detection does not reduce RR mean, but reduces P95')
    check({str(p.relative_to(materials)):sha(p) for p in materials.rglob('*') if p.is_file()}==material_hashes,'Original materials unchanged')
    with (out/'chapter4_comparison.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    report={'all_passed':all(x['passed'] for x in checks),'checks_total':len(checks),'checks_failed':[x for x in checks if not x['passed']], 'cases':4,'variants_per_case':4,'fresh_comparison_runs':16,'reverse_repeat_runs':16,'native_metrics_replays':16,'archived_metrics_replays':16,'build_info':build,'binary_sha256':sha(exe),'host':platform.platform(),'python':platform.python_version(),'seconds':time.perf_counter()-started,'metrics_representation_differences':numeric_differences,'rows':rows,'checks':checks,'commands':calls,'materials_sha256':material_hashes}
    (out/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['all_passed','checks_total','checks_failed','seconds','metrics_representation_differences']},ensure_ascii=False,indent=2))
    if not report['all_passed']:raise SystemExit(1)

if __name__=='__main__':main()
