"""Demonstrations checked against manual expectations and fresh file reloads."""
import csv
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
from collections import Counter

PROGRAM = pathlib.Path(sys.argv[1])
SOURCE = pathlib.Path(sys.argv[2])
DATA = pathlib.Path(sys.argv[3]) / 'stage13-tests' / sys.argv[4]
GROUP = sys.argv[4]
NAMES = ['baseline', 'crash', 'blackout', 'long_blackout', 'reordered', 'simultaneous', 'no_alive', 'unfinished', 'generated_recovery']
VARIANTS = ['fixed_rr', 'fixed_ll', 'adaptive_rr', 'adaptive_ll']


def require(value, message):
    if not value:
        raise RuntimeError(message)


def call(*args):
    result = subprocess.run([str(PROGRAM), *map(str, args)], cwd=DATA, capture_output=True, text=True, timeout=30)
    require(result.returncode == 0, f'{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result


def read(path):
    return json.loads(path.read_text())


def csv_rows(path):
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream))


def inputs(path):
    return {name: hashlib.sha256((path/name).read_bytes()).hexdigest() for name in ['scenario.json', 'delivery.csv']}


def events(rows, kind):
    return [e for e in rows if e['kind'] == kind]


def case(name):
    src = SOURCE/'examples'/name
    original = inputs(src)
    # Each invocation reloads the files, then shares its own immutable snapshot
    # across four independent Simulation instances.
    for repeat in ['first', 'reloaded']:
        call('compare', '--scenario', src/'scenario.json', '--out', DATA/name/repeat)
    runs = {}
    for variant in VARIANTS:
        first = DATA/name/'first'/variant
        second = DATA/name/'reloaded'/variant
        for filename in ['manifest.json', 'events.jsonl', 'metrics.json', 'tasks.csv', 'attempts.csv', 'nodes.csv']:
            require((first/filename).read_bytes() == (second/filename).read_bytes(), f'{name}/{variant}/{filename}: fresh reload changed result')
        before = (second/'metrics.json').read_bytes()
        call('metrics', '--run-dir', second)
        require((second/'metrics.json').read_bytes() == before, 'Saved-journal recalculation changed metrics')
        require(inputs(first/'input') == original == inputs(second/'input'), 'Different input bytes between variants/reloads')
        rows = [json.loads(line) for line in (first/'events.jsonl').read_text().splitlines()]
        require([r['record_no'] for r in rows] == list(range(1, len(rows)+1)), 'Record sequence')
        horizon = read(src/'scenario.json')['horizon_ticks']
        require(rows[-1]['kind'] == 'run_completed' and rows[-1]['time_ticks'] == horizon, 'Run did not reach horizon')
        keys = [(r['time_ticks'], r['phase'], r['tie_key'], r['action_sequence']) for r in rows if r['phase']]
        require(keys == sorted(keys) and all(k[0] <= horizon for k in keys), 'Engine key order/horizon')
        for kind, phase in [('reconfiguration', 6), ('dispatch', 8)]:
            selected = events(rows, kind)
            require(all(e['phase'] == phase for e in selected) and all(c == 1 for c in Counter(e['time_ticks'] for e in selected).values()), 'Repeated batch or wrong phase')
        accepted = [r['attempt']['task_id'] for r in events(rows, 'result') if r['decision'] == 'accepted']
        metric = read(first/'metrics.json')
        require(len(accepted) == len(set(accepted)) == metric['completed'], 'Duplicate result acceptance')
        require(metric['arrived'] == metric['completed'] + metric['unfinished'] and metric['unfinished'] == metric['queued'] + metric['assigned'], 'Task balance')
        require(all(metric['invariants'].values()), 'Recorded invariant failure')
        runs[variant] = (rows, metric, csv_rows(first/'tasks.csv'), csv_rows(first/'attempts.csv'), csv_rows(first/'nodes.csv'))
    require(inputs(src) == original, 'Demonstration inputs were modified')
    return runs


def catalog():
    manifest = read(SOURCE/'examples/experiments.json')
    require({r['scenario_id'] for r in manifest['runs']} == set(NAMES) and len(manifest['runs']) == len(NAMES), 'Batch catalog is incomplete')
    for name in NAMES:
        path = SOURCE/'examples'/name
        require((path/'README.md').is_file(), f'{name}: missing expected behavior')
        call('validate', '--scenario', path/'scenario.json')
        config = read(path/'scenario.json')
        rows = csv_rows(path/'delivery.csv')
        expected = {(n['id'], seq) for n in config['nodes'] for seq in range(1, config['horizon_ticks']//config['heartbeat_period_ticks']+1)}
        actual = [(int(r['node_id']), int(r['heartbeat_seq'])) for r in rows]
        require(len(actual) == len(set(actual)) and set(actual) == expected, 'Delivery trace is not complete and unique')


def long_blackout():
    for variant, (rows, m, tasks, attempts, nodes) in case('long_blackout').items():
        require(m['false_positives'] == 1 and m['crashes'] == 0 and m['false_negatives'] == 0 and m['completed'] == 3, 'False exclusion classification')
        node = next(n for n in nodes if n['node_id'] == '1')
        require(node['failed_ticks'] == '8' and node['classification'] == 'false_positive', 'False FAILED deadline')
        restored = [e for e in events(rows, 'heartbeat_observed') if e['message']['node_id'] == 1 and e['time_ticks'] >= 14]
        require(restored and all(e['decision'] == 'already_failed' and e['after'] == 'failed' for e in restored), 'FAILED must remain terminal after delivery resumes')
        expected = ['18', '1', '21'] if variant.endswith('_rr') else ['18', '1', '4']
        require([t['accepted_ticks'] for t in tasks] == expected, 'Full-work retry completion times')
        require(m['rejected_revoked_results'] == (2 if variant.endswith('_rr') else 1), 'Old physical results counted incorrectly')
        if variant.endswith('_rr'):
            old = next(a for a in attempts if a['task_id'] == '3' and a['attempt_no'] == '1')
            require(old['started_ticks'] == '10' and old['finished_ticks'] == '13' and old['revoked_ticks'] == '8', 'False FAILED cancelled the old local FIFO')


def reordered():
    for variant, (rows, m, tasks, _, _) in case('reordered').items():
        require(m['false_positives'] == m['false_negatives'] == m['retries'] == 0 and m['completed'] == 4, 'Reordered stream caused a failure')
        for sequence, at in [(1, 7), (5, 14)]:
            event = next(e for e in events(rows, 'heartbeat_observed') if e['message'] == {'node_id': 1, 'heartbeat_seq': sequence})
            require(event['time_ticks'] == at and event['decision'] == 'non_increasing_sequence', 'Old heartbeat sequence accepted')
        first_old = next(e for e in events(rows, 'heartbeat_observed') if e['message'] == {'node_id': 1, 'heartbeat_seq': 1})
        require(first_old['contact_ticks'] == 6 and first_old['generation'] == 2, 'Rejected heartbeat reset contact/timers')
        assigned = next(e for e in events(rows, 'task_assigned') if e['assignment']['task']['id'] == 3)
        require(assigned['assignment']['node_id'] == (2 if variant.startswith('adaptive') else 1), 'Phase-6 SUSPECT availability not reflected in assignment')
        transitions = [e for e in rows if e['kind'] in ['heartbeat_observed', 'timeout_observed'] and e['before'] != e['after']]
        if variant.startswith('adaptive'):
            require([(e['time_ticks'], e['after']) for e in transitions] == [(10, 'suspect'), (12, 'alive')], 'Adaptive short gap expectations')
        else:
            require(not transitions, 'Fixed should remain ALIVE in this trace')
        require([t['accepted_ticks'] for t in tasks] == ['4', '4', '11', '13'], 'Reordered example completion times')


def simultaneous():
    for variant, (rows, m, tasks, _, _) in case('simultaneous').items():
        batches = [e for e in events(rows, 'reconfiguration') if e['newly_failed']]
        require(len(batches) == 1 and batches[0]['time_ticks'] == 10 and batches[0]['newly_failed'] == [1, 2], 'FAILED nodes were not excluded together')
        returned = [r['task_id'] for r in batches[0]['revoked']]
        expected = [1, 2, 4, 5] if variant.endswith('_rr') else [1, 2]
        require(returned == expected, 'Joint retry set is not globally sorted')
        assignments = [e['assignment']['task']['id'] for e in events(rows, 'task_assigned') if e['time_ticks'] == 10]
        require(assignments == expected + [7], 'Dispatch did not follow all revocations and same-time arrivals')
        require(m['completed'] == 7 and m['crashes'] == 2 and m['detection_samples'] == 2 and m['mean_detection']['value_ticks'] == 7, 'Correct detection samples')
        expected_times = ['20', '29', '1', '31', '34', '2', '35'] if variant.endswith('_rr') else ['20', '29', '1', '3', '6', '7', '30']
        require([t['accepted_ticks'] for t in tasks] == expected_times, 'Simultaneous recovery completion times')
        recoveries = [n['recovery']['value_ticks'] for n in m['nodes'] if n['node_id'] in [1, 2]]
        require(recoveries == ([28, 31] if variant.endswith('_rr') else [17, 26]), 'Per-node recovery membership')


def no_alive():
    for _, (rows, m, tasks, attempts, nodes) in case('no_alive').items():
        require(m['completed'] == 0 and m['unfinished'] == m['queued'] == 4 and m['assigned'] == 0, 'No-ALIVE task accounting')
        require(m['crashes'] == 2 and m['false_negatives'] == m['false_positives'] == 0 and m['incomplete_recoveries'] == 2, 'No-ALIVE failure accounting')
        require(all(n['failed_ticks'] == '8' and n['recovery_reason'] == 'recovery_incomplete' for n in nodes), 'Incomplete recovery replaced by horizon/zero')
        require([t['attempts'] for t in tasks] == ['1', '1', '0', '0'], 'Assignments continued without ALIVE')
        require(len(attempts) == 2 and not events(rows, 'physical_started'), 'Tasks started after crash at zero')
        require(all(e['time_ticks'] == 0 for e in events(rows, 'task_assigned')), 'Crash leaked into initial ALIVE, or dispatch resumed without ALIVE')
        excluded = next(e for e in events(rows, 'reconfiguration') if e['time_ticks'] == 4)
        require(excluded['alive'] == [] and excluded['newly_failed'] == [], 'SUSPECT must exclude without revoking')
        pending = []
        for event in rows:
            if event['kind'] == 'task_arrived':
                pending.append(event['task']['id'])
            elif event['kind'] == 'task_assigned':
                require(pending.pop(0) == event['assignment']['task']['id'], 'Assignment violated FIFO')
            elif event['kind'] == 'reconfiguration':
                pending.extend(r['task_id'] for r in event['revoked'])
        require(pending == [3, 1, 2, 4], 'Existing queue/returned set/new arrival order')
        require(all(n['detection']['value_ticks'] == 8 for n in m['nodes']), 'No-first-heartbeat Detection')


def unfinished():
    for variant, (rows, m, tasks, attempts, _) in case('unfinished').items():
        require(m['completed'] == 1 and m['unfinished'] == m['assigned'] == 2 and m['queued'] == 0, 'Horizon balance')
        require(m['false_positives'] == m['false_negatives'] == m['retries'] == 0, 'Healthy unfinished work misclassified')
        require([t['accepted_ticks'] for t in tasks] == ['', '1', ''] and m['mean_latency']['value_ticks'] == 1 and m['p95_latency']['value_ticks'] == 1, 'Only accepted tasks enter latency')
        require(m['throughput_windows'][0]['tasks_per_second'] == 100000, 'Horizon throughput')
        last = next(e for e in events(rows, 'task_assigned') if e['assignment']['task']['id'] == 3)
        require(last['time_ticks'] == 10 and last['phase'] == 8, 'Arrival/dispatch at horizon was omitted')
        physical = next(a for a in attempts if a['task_id'] == '3')
        require(physical['started_ticks'] == ('' if variant.endswith('_rr') else '10') and not physical['finished_ticks'], 'Physical state of horizon assignment')


def reload():
    for name in ['baseline', 'crash', 'blackout']:
        case(name)


def generator():
    config = SOURCE/'examples/generator_recovery.json'
    stored = SOURCE/'examples/generated_recovery'
    def fnv64(path):
        value = 14695981039346656037
        for byte in path.read_bytes():
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        return f'{value:016x}'
    metadata = read(stored/'generation_manifest.json')
    require((stored/'generator_config.json').read_bytes() == config.read_bytes(), 'Stored generator config differs')
    require(metadata['seed'] == 1304 and metadata['status'] == 'complete' and
            metadata['config_checksum_fnv1a64'] == fnv64(config) and
            metadata['scenario_checksum_fnv1a64'] == fnv64(stored/'scenario.json') and
            metadata['delivery_checksum_fnv1a64'] == fnv64(stored/'delivery.csv'), 'Stored generation metadata/checksums')
    for name in ['generated-first', 'generated-again']:
        call('generate', '--config', config, '--out', DATA/name)
        call('validate', '--scenario', DATA/name/'scenario.json')
        require(inputs(DATA/name) == inputs(stored), 'Regenerated materialized control inputs differ')
        require((DATA/name/'generator_config.json').read_bytes() == config.read_bytes(), 'Config snapshot differs')
        manifest = read(DATA/name/'generation_manifest.json')
        require(manifest['status'] == 'complete' and manifest['seed'] == 1304 and manifest['generator_version'] == 1, 'Generator metadata')
    require(len(csv_rows(stored/'delivery.csv')) == 40, 'Generated trace must include sends after crash')
    runs = case('generated_recovery')
    for _, (_, m, tasks, _, _) in runs.items():
        require([t['accepted_ticks'] for t in tasks] == ['14', '7', '20'], 'Generated manual completion times')
        failed = next(n for n in m['nodes'] if n['node_id'] == 1)
        require(failed['detection']['value_ticks'] == 5 and failed['recovery']['value_ticks'] == 17, 'Generated manual detection/recovery')
    # The regenerated on-disk files, not a generator's transient memory, drive run.
    call('run', '--scenario', DATA/'generated-first/scenario.json', '--detector', 'fixed', '--scheduler', 'rr', '--out', DATA/'generated-run')
    original = DATA/'generated_recovery/first/fixed_rr'
    for filename in ['events.jsonl', 'metrics.json', 'tasks.csv', 'attempts.csv', 'nodes.csv']:
        require((DATA/'generated-run'/filename).read_bytes() == (original/filename).read_bytes(), 'Saved regenerated inputs changed execution')


def batch():
    call('batch', '--manifest', SOURCE/'examples/experiments.json', '--out', DATA/'suite')
    report = read(DATA/'suite/batch_manifest.json')
    require(report['status'] == 'complete' and len(report['runs']) == len(NAMES), 'Incomplete demonstration batch')
    rows = csv_rows(DATA/'suite/comparison.csv')
    require(len(rows) == 4*len(NAMES) and {r['scenario_id'] for r in rows} == set(NAMES), 'Missing scenario/variant')
    stopped = [r for r in rows if r['scenario_id'] == 'no_alive']
    require(len(stopped) == 4 and all(r['unfinished'] == '4' and r['recovery_reason'] == 'empty_sample' for r in stopped), 'Batch stopped on valid unfinished scenario')
    require(len(csv_rows(DATA/'suite/summary.csv')) == 4*len(NAMES), 'Missing summary groups')


if __name__ == '__main__':
    try:
        if DATA.exists():
            shutil.rmtree(DATA)
        DATA.mkdir(parents=True)
        {'catalog': catalog, 'long_blackout': long_blackout, 'reordered': reordered,
         'simultaneous': simultaneous, 'no_alive': no_alive, 'unfinished': unfinished,
         'reload': reload, 'generator': generator, 'batch': batch}[GROUP]()
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
