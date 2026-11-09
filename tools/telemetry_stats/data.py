"""Validate primary manifests and import native run metrics without recomputing them."""
import csv
import json
import math
from pathlib import Path
import run_seed_series as io

KINDS={'network':'network_manifest.json','timeout':'timeout_manifest.json','kappa':'kappa_manifest.json',
       'coefficient':'coefficient_manifest.json','window':'window_manifest.json','load':'load_manifest.json'}
# output name: (native metric, unit); durations alone are converted from us to ms.
METRICS={'latency_ms':('mean_latency','ms'),'p95_ms':('p95_latency','ms'),
         'detection_ms':('mean_detection','ms'),'recovery_ms':('mean_recovery','ms'),
         'throughput':('throughput','tasks/s'),'unfinished':('unfinished','tasks'),
         'completed':('completed','tasks'),'fp':('false_positives','nodes'),
         'fn':('false_negatives','nodes'),'late':('crashes_after_false_exclusion','crashes'),
         'incomplete_recoveries':('incomplete_recoveries','nodes'),'retries':('retries','assignments')}


def read_json(path):return json.loads(path.read_text(encoding='utf-8'))
def csv_rows(path):
    with path.open(newline='',encoding='utf-8') as stream:return list(csv.DictReader(stream))


def prepare(plan_path,results_root):
    plan=io.load(plan_path)
    io.require(isinstance(plan,dict) and set(plan)=={'schema_version','bootstrap_seed','resamples','confidence_level','window_index','studies'} and type(plan['schema_version']) is int and plan['schema_version']==1,'Invalid statistics plan schema')
    io.require(type(plan['bootstrap_seed']) is int and 0<=plan['bootstrap_seed']<=io.MAX_SEED,'Invalid bootstrap seed')
    io.require(type(plan['resamples']) is int and 100<=plan['resamples']<=1000000,'Invalid bootstrap resample count')
    io.require(type(plan['confidence_level']) in (int,io.Decimal) and 0<plan['confidence_level']<1,'Invalid confidence level')
    io.require(type(plan['window_index']) is int and plan['window_index']>=0,'Invalid selected window')
    io.require(isinstance(plan['studies'],list) and plan['studies'],'No primary studies')
    ids=set();paths=set();sources=[]
    for study in plan['studies']:
        io.require(isinstance(study,dict) and set(study)=={'id','kind','directory'} and study['kind'] in KINDS,'Invalid study schema')
        import re
        io.require(isinstance(study['id'],str) and re.fullmatch('[a-z][a-z0-9_]*',study['id']) and study['id'] not in ids,'Invalid/duplicate study ID')
        name=study['directory'];io.require(isinstance(name,str) and name and not Path(name).is_absolute(),'Expected relative primary study directory')
        root=(results_root/name).resolve();io.require(root.is_relative_to(results_root) and root not in paths,'Escaping/duplicate study directory')
        manifest=read_json(root/KINDS[study['kind']])
        io.require(manifest.get('status')=='complete' and manifest.get('schema_version')==1,'Incomplete primary study')
        io.require(isinstance(manifest.get('cases'),list) and manifest['cases'],'Missing primary cases')
        ids.add(study['id']);paths.add(root);sources.append((study,root,manifest))
    return plan,sources


def check_equal(csv_value,value,label):
    if value is None:io.require(csv_value=='','Nonempty CSV for undefined '+label)
    elif type(value) is int:io.require(csv_value==str(value),'Native count mismatch: '+label)
    else:io.require(csv_value!='' and math.isclose(float(csv_value),float(value),rel_tol=1e-12,abs_tol=1e-9),'Native metric mismatch: '+label)


def ingest(plan,sources):
    records=[];watched={};cases={};native_checks=0
    def watch(path,expected=None):
        actual=io.sha(path)
        io.require(expected is None or actual==expected,'Source checksum mismatch: '+str(path))
        io.require(path not in watched or watched[path]==actual,'Source changed during import')
        watched[path]=actual
    for study,root,manifest in sources:
        watch(root/KINDS[study['kind']]);case_ids=set()
        for metadata in manifest['cases']:
            case_id=metadata['id'];io.require(isinstance(case_id,str) and case_id not in case_ids,'Duplicate case ID');case_ids.add(case_id)
            directory=(root/'cases'/case_id).resolve();io.require(directory.is_relative_to(root),'Escaping case path')
            sp=directory/'series_manifest.json';watch(sp);series=read_json(sp)
            io.require(series['status']=='complete' and series['seeds']==manifest['seeds'] and [e['seed'] for e in series['entries']]==series['seeds'] and len(set(series['seeds']))==len(series['seeds']),'Incomplete/duplicate seed series')
            io.require(series.get('mode') in ('generated','reused'),'Unknown seed series mode')
            comparison=directory/'batch/comparison.csv';watch(comparison);rows=csv_rows(comparison);mapping={}
            for row in rows:
                key=(row['scenario_id'],row['repeat_id'],row['variant'],int(row['window_index']))
                io.require(key not in mapping,'Duplicate native comparison row');mapping[key]=row
            cases[(study['id'],case_id)]={'kind':study['kind'],'metadata':metadata,'expected':len(series['seeds']),'seeds':tuple(series['seeds'])}
            selected=set()
            for entry in series['entries']:
                scenario_path=io.contained(directory,entry['input_files']['scenario.json']);watch(scenario_path,entry['input_sha256']['scenario.json'])
                delivery=io.contained(directory,entry['input_files']['delivery.csv']);watch(delivery,entry['input_sha256']['delivery.csv'])
                scenario=io.load(scenario_path)
                io.require(scenario['tick_ns']==1000,'Expected microsecond native ticks')
                for variant in io.VARIANTS:
                    run=(directory/entry['batch_output']/variant).resolve();io.require(run.is_relative_to(directory),'Escaping batch output')
                    rp=run/'manifest.json';watch(rp);io.require(read_json(rp)['status']=='complete','Incomplete native variant')
                    for name in io.INPUT_FILES:watch(run/'input'/name,entry['input_sha256'][name])
                    io.require(set(entry['results_sha256'][variant])==set(io.RESULT_FILES),'Incomplete native fingerprint set')
                    for name in io.RESULT_FILES:watch(run/name,entry['results_sha256'][variant][name])
                    m=read_json(run/'metrics.json');io.require(m['units']=={'durations':'ticks','throughput':'tasks/second','tick_ns':1000},'Unexpected metric units');io.require(all(m['invariants'].values()) and m['arrived']==len(scenario['tasks']) and m['arrived']==m['completed']+m['unfinished'],'Native invariants/balance failed')
                    index=plan['window_index'];io.require(index<len(m['throughput_windows']),'Selected window missing')
                    window=m['throughput_windows'][index];key=(entry['scenario_id'],entry['repeat_id'],variant,index)
                    io.require(key in mapping,'Native comparison mapping missing');row=mapping[key];selected.add(key)
                    for name in ['arrived','completed','unfinished','queued','assigned','retries','rejected_revoked_results','crashes','false_positives','false_negatives','crashes_after_false_exclusion','incomplete_recoveries','latency_samples','detection_samples','recovery_samples']:
                        check_equal(row[name],m[name],name)
                    values={};reasons={}
                    for output,(native,unit) in METRICS.items():
                        if unit=='ms':
                            value=m[native]['value_ticks'];check_equal(row[native+'_ticks'],value,native)
                            reason=m[native]['reason'] or '';io.require((value is None)==bool(reason),'Native value/reason mismatch')
                            reason_key={'mean_latency':'latency_reason','p95_latency':'p95_reason','mean_detection':'detection_reason','mean_recovery':'recovery_reason'}[native]
                            io.require(row[reason_key]==reason,'Native null reason mismatch')
                            values[output]=None if value is None else float(value)/1000;reasons[output]=reason
                        elif native=='throughput':values[output]=float(window['tasks_per_second']);check_equal(row['throughput_tasks_per_second'],values[output],native)
                        else:values[output]=m[native]
                    check_equal(row['window_start_ticks'],window['start_ticks'],'window start');check_equal(row['window_end_ticks'],window['end_ticks'],'window end');check_equal(row['window_completed'],window['completed'],'window completed')
                    io.require(all(v is None or math.isfinite(v) for v in values.values()),'Non-finite native metric')
                    records.append({'study':study['id'],'case':case_id,'variant':variant,'seed':entry['seed'],
                                    'node_count':len(scenario['nodes']),'task_count':len(scenario['tasks']),
                                    'window_start_ticks':window['start_ticks'],'window_end_ticks':window['end_ticks'],
                                    'scenario_sha256':entry['input_sha256']['scenario.json'],'delivery_sha256':entry['input_sha256']['delivery.csv'],
                                    'native_latency_samples':m['latency_samples'],'native_detection_samples':m['detection_samples'],'native_recovery_samples':m['recovery_samples'],
                                    'values':values,'reasons':reasons,'scenario':scenario})
                    native_checks+=1
            expected_rows=len(series['entries'])*len(io.VARIANTS)
            io.require(len(selected)==expected_rows and selected=={k for k in mapping if k[-1]==plan['window_index']},'Wrong selected native row count')
            print('Imported',study['id'],case_id,flush=True)
    io.require(len({(r['study'],r['case'],r['variant'],r['seed']) for r in records})==len(records),'Duplicate observation')
    return records,cases,watched,native_checks
