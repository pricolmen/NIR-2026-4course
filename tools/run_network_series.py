#!/usr/bin/env python3
"""Stage 2: fixed-mean heartbeat jitter/loss grid, using the seed-series CLI."""
import argparse
import copy
import csv
import hashlib
import os
from decimal import Decimal
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import run_seed_series as seeds

EXTRA = ['case_id', 'network_mode', 'delay_center_ticks', 'jitter_half_width_ticks',
         'min_delay_ticks', 'max_delay_ticks', 'loss_probability']


def prepare(plan_path):
    plan = seeds.load(plan_path)
    fields = {'schema_version','base_generator','seeds','delay_center_ticks',
              'jitter_half_width_ticks','loss_probabilities','modes'}
    seeds.require(isinstance(plan, dict) and set(plan) == fields and type(plan['schema_version']) is int and plan['schema_version'] == 1, 'Invalid network plan schema')
    values = plan['seeds']
    seeds.require(isinstance(values, list) and values and all(type(v) is int and 0 <= v <= seeds.MAX_SEED for v in values)
                  and len(values) == len(set(values)), 'Expected explicit distinct uint64 seeds')
    center = plan['delay_center_ticks'];widths = plan['jitter_half_width_ticks'];losses = plan['loss_probabilities']
    seeds.require(type(center) is int and 0 <= center <= (1 << 63)-1, 'Invalid delay center')
    seeds.require(isinstance(widths, list) and widths and all(type(v) is int and 0 <= v <= center and center+v < 1 << 63 for v in widths)
                  and len(widths) == len(set(widths)), 'Invalid/duplicate jitter half-widths')
    seeds.require(isinstance(losses, list) and losses and all(type(v) in (int, Decimal) and 0 <= v <= 1 for v in losses)
                  and len(losses) == len(set(losses)), 'Invalid/duplicate loss probabilities')
    name = plan['base_generator']
    seeds.require(isinstance(name, str) and name and not Path(name).is_absolute(), 'Expected relative base_generator path')
    base_path = (plan_path.parent/name).resolve();base = seeds.load(base_path)
    seeds.require(base['scenario']['crashes'] == [] and base['scenario']['blackouts'] == [], 'Base must have no crash/blackout; crashes are explicit plan modes')
    modes = plan['modes'];seen = set();cases = []
    seeds.require(isinstance(modes, list) and modes, 'Missing network modes')
    for mode in modes:
        seeds.require(isinstance(mode, dict) and set(mode) == {'id','crashes'} and isinstance(mode['id'], str)
                      and re.fullmatch('[a-z][a-z0-9_]*', mode['id']) and mode['id'] not in seen
                      and isinstance(mode['crashes'], list), 'Invalid/duplicate mode')
        seen.add(mode['id'])
        for width in widths:
            for index, probability in enumerate(losses):
                config = copy.deepcopy(base)
                config['scenario']['crashes'] = copy.deepcopy(mode['crashes'])
                config['delivery'].update(min_delay_ticks=center-width,max_delay_ticks=center+width,loss_probability=probability)
                cases.append({'id':'%s_j%d_q%d' % (mode['id'],width,index),'mode':mode['id'],
                              'delay_center_ticks':center,'jitter_half_width_ticks':width,
                              'min_delay_ticks':center-width,'max_delay_ticks':center+width,
                              'loss_probability':probability,'config':config})
    return plan, base_path, base, cases


def csv_rows(path):
    with path.open(newline='',encoding='utf-8') as stream:
        reader = csv.DictReader(stream)
        return reader.fieldnames, list(reader)


def verify_controls(out, plan, cases):
    """Validate input pairing only. No simulation or result-metric calculation."""
    workloads = {};traces_by_mode = {};loss_controls = {};checked = 0
    for case in cases:
        directory = out/'cases'/case['id'];series = seeds.load(directory/'series_manifest.json')
        seeds.require(series['status'] == 'complete' and series['seeds'] == plan['seeds'], 'Incomplete/wrong child series')
        seeds.require(series['template_sha256'] == seeds.sha(out/'templates'/(case['id']+'.json')), 'Child template mismatch')
        seeds.require(len(series['entries']) == len(plan['seeds']), 'Wrong child input count')
        for entry in series['entries']:
            seed = entry['seed']
            scenario_path = seeds.contained(directory,entry['input_files']['scenario.json'])
            delivery_path = seeds.contained(directory,entry['input_files']['delivery.csv'])
            scenario = seeds.load(scenario_path)
            seeds.require(seeds.sha(scenario_path) == entry['input_sha256']['scenario.json'] and seeds.sha(delivery_path) == entry['input_sha256']['delivery.csv'], 'Child input changed')
            common = {k:v for k,v in scenario.items() if k != 'crashes'}
            if seed in workloads:
                seeds.require(common == workloads[seed], 'Workload/detector/physical parameters changed across network cells')
            else:workloads[seed] = common
            seeds.require(scenario['crashes'] == case['config']['scenario']['crashes'], 'Wrong crash mode')
            key = (seed,case['jitter_half_width_ticks'],case['loss_probability'])
            checksum = seeds.sha(delivery_path)
            if key in traces_by_mode:seeds.require(traces_by_mode[key] == checksum, 'Healthy/crash delivery trace differs')
            else:traces_by_mode[key] = checksum
            header, rows = csv_rows(delivery_path)
            seeds.require(header == ['node_id','heartbeat_seq','delay_ticks','lost'], 'Unexpected delivery schema')
            expected = len(scenario['nodes'])*(scenario['horizon_ticks']//scenario['heartbeat_period_ticks'])
            seeds.require(len(rows) == expected and all(case['min_delay_ticks'] <= int(r['delay_ticks']) <= case['max_delay_ticks'] and r['lost'] in ('0','1') for r in rows), 'Wrong realized trace size/range')
            delays = [(r['node_id'],r['heartbeat_seq'],r['delay_ticks']) for r in rows]
            flags = [int(r['lost']) for r in rows]
            loss_controls.setdefault((seed,case['mode'],case['jitter_half_width_ticks']),[]).append((case['loss_probability'],delays,flags))
            if case['loss_probability'] == 0:seeds.require(not any(flags), 'Losses present at q=0')
            if case['loss_probability'] == 1:seeds.require(all(flags), 'Missing losses at q=1')
            result = directory/entry['batch_output']
            detector_views = {}
            for variant in seeds.VARIANTS:
                run = result/variant
                for name in seeds.INPUT_FILES:
                    seeds.require(seeds.sha(run/'input'/name) == entry['input_sha256'][name], 'Variant input differs')
                seeds.require(seeds.load(run/'manifest.json')['status'] == 'complete', 'Variant not complete')
                metrics = seeds.load(run/'metrics.json')
                seeds.require(all(metrics['invariants'].values()) and metrics['arrived'] == len(scenario['tasks']) and
                              metrics['arrived'] == metrics['completed']+metrics['unfinished'] and
                              metrics['unfinished'] == metrics['queued']+metrics['assigned'], 'Task balance/invariants failed')
                seeds.require(metrics['crashes'] == len(scenario['crashes']) and
                              metrics['crashes'] == metrics['detection_samples']+metrics['false_negatives']+metrics['crashes_after_false_exclusion'],
                              'Crash classification/sample counts inconsistent')
                _, nodes = csv_rows(run/'nodes.csv')
                view = [(r['node_id'],r['failed_ticks'],r['final_status'],r['classification']) for r in nodes]
                detector = variant.split('_')[0]
                if detector in detector_views:seeds.require(view == detector_views[detector], 'Detector history depends on scheduler')
                else:detector_views[detector] = view
                for name, fingerprint in entry['results_sha256'][variant].items():
                    seeds.require(seeds.sha(run/name) == fingerprint, 'Result fingerprint mismatch')
            checked += 1
    for group in loss_controls.values():
        ordered = sorted(group,key=lambda r:r[0])
        for first,second in zip(ordered,ordered[1:]):
            seeds.require(first[1] == second[1], 'Delays changed when only q changed')
            seeds.require(all(a <= b for a,b in zip(first[2],second[2])), 'Loss masks not nested across q at fixed jitter/seed')
    return {'input_realizations_checked':checked,'variant_runs_checked':checked*4,
            'same_workload_and_detectors_across_network_cells':True,'same_trace_across_crash_modes':True,
            'same_delays_across_q_at_fixed_jitter':True,'nested_loss_masks_across_q_at_fixed_jitter':True,
            'exact_variant_input_hashes':True,'all_result_fingerprints_valid':True,'all_task_balances_valid':True,'crash_classification_counts_valid':True,
            'detector_classification_identical_for_rr_ll':True,
            'paired_crash_mode_trace_sets':len(traces_by_mode),'controlled_loss_groups':len(loss_controls),
            'workload_sha256_per_seed':{str(k):hashlib.sha256(seeds.encode(v).encode()).hexdigest() for k,v in workloads.items()}}


def collate(out, cases, filename):
    fields = None;count = 0
    with (out/('network_'+filename)).open('w',newline='',encoding='utf-8') as stream:
        for case in cases:
            current, rows = csv_rows(out/'cases'/case['id']/'batch'/filename)
            if fields is None:
                fields = current;writer = csv.DictWriter(stream,fieldnames=EXTRA+fields);writer.writeheader()
            seeds.require(fields == current, 'Native batch CSV schemas differ')
            metadata = {k:case[k] for k in EXTRA if k not in ('case_id','network_mode')}
            metadata.update(case_id=case['id'],network_mode=case['mode'])
            for row in rows:writer.writerow({**metadata,**row});count += 1
    return count


def execute(args):
    plan_path = args.plan.expanduser().resolve();plan,base_path,base,cases = prepare(plan_path)
    program = args.program.expanduser().resolve()
    seeds.require(program.is_file() and os.access(program,os.X_OK), 'Executable unavailable')
    requested = args.out.expanduser()
    seeds.require(not requested.exists() and not requested.is_symlink(), 'Output exists; choose a new directory')
    out = requested.resolve();out.mkdir(parents=True)
    for folder in ['templates','cases','logs']:(out/folder).mkdir()
    shutil.copyfile(plan_path,out/'plan.json');shutil.copyfile(base_path,out/'base_generator.json')
    watched = {p:seeds.sha(p) for p in (plan_path,base_path,program,Path(__file__).resolve(),Path(seeds.__file__).resolve())}
    state = {'schema_version':1,'status':'incomplete','plan_sha256':watched[plan_path],
             'base_sha256':watched[base_path],'program':str(program),'program_sha256':watched[program],
             'driver_sha256':seeds.sha(Path(__file__).resolve()),'seed_runner_sha256':seeds.sha(Path(seeds.__file__).resolve()),
             'seeds':plan['seeds'],'cases':[],'commands':[],
             'note':'Native generate/batch metrics only; native summary rows concatenated without recalculation. Repeated saved inputs are not independent samples.'}
    manifest = out/'network_manifest.json'
    try:
        for index,case in enumerate(cases):
            template = out/'templates'/(case['id']+'.json');seeds.save(template,case['config'])
            cmd = [sys.executable,str(Path(seeds.__file__).resolve()),'--template',str(template),
                   '--seeds',*map(str,plan['seeds']),'--scenario-id',case['id'],'--program',str(program),
                   '--out',str(out/'cases'/case['id'])]
            record = {'argv':cmd,'returncode':None,'stdout':'logs/'+case['id']+'.stdout.log','stderr':'logs/'+case['id']+'.stderr.log'}
            state['commands'].append(record);seeds.save(manifest,state);started=time.perf_counter()
            with (out/record['stdout']).open('w',encoding='utf-8') as stdout,(out/record['stderr']).open('w',encoding='utf-8') as stderr:
                result = subprocess.run(cmd,stdout=stdout,stderr=stderr,cwd=out,text=True)
            record.update(returncode=result.returncode,seconds=time.perf_counter()-started)
            seeds.require(result.returncode == 0,'Seed-series command failed; see '+str(out/record['stderr']))
            state['cases'].append({k:v for k,v in case.items() if k != 'config'})
            seeds.save(manifest,state)
            print('[%d/%d] %s complete' % (index+1,len(cases),case['id']),flush=True)
        state['checks'] = verify_controls(out,plan,cases)
        state['comparison_rows'] = collate(out,cases,'comparison.csv')
        state['summary_rows'] = collate(out,cases,'summary.csv')
        for path,fingerprint in watched.items():seeds.require(seeds.sha(path) == fingerprint,'Source/binary changed during network experiment')
        state['status'] = 'complete';seeds.save(manifest,state)
    except BaseException as error:
        state['status'] = 'failed';state['error'] = str(error);seeds.save(manifest,state);raise
    print('Saved:',out)
    print('Cases:',len(cases),'; seeds per case:',len(plan['seeds']),'; variant runs:',state['checks']['variant_runs_checked'])
    print('Native summaries retained. No confidence intervals or general superiority claim calculated.')
    return 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    try:return execute(args)
    except KeyboardInterrupt:
        print('Interrupted; partial experiment marked failed.',file=sys.stderr);return 130
    except (OSError,ValueError,KeyError,TypeError,ArithmeticError,subprocess.SubprocessError) as error:
        print('Network series failed: '+str(error),file=sys.stderr);return 1

if __name__=='__main__':sys.exit(main())
