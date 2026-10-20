"""Finite load smoke, canonical repeats and independent replay; stdlib only.

Timings belong to this noncanonical verification report, never to events.jsonl.
The fixture is a software check, not a scientifically justified workload.
"""
import collections
import csv
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess
import sys
import time

PROGRAM = pathlib.Path(sys.argv[1])
SOURCE = pathlib.Path(sys.argv[2])
DATA = pathlib.Path(sys.argv[3]) / 'stage14-load'
VARIANTS = ['fixed_rr', 'fixed_ll', 'adaptive_rr', 'adaptive_ll']
CANONICAL = ['manifest.json', 'events.jsonl', 'metrics.json', 'tasks.csv', 'attempts.csv', 'nodes.csv']


def require(value, message):
    if not value:
        raise RuntimeError(message)


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(block)
    return checksum.hexdigest()


def call(*args):
    started = time.perf_counter()
    result = subprocess.run([str(PROGRAM), *map(str, args)], cwd=DATA, capture_output=True, text=True, timeout=60)
    elapsed = time.perf_counter() - started
    require(result.returncode == 0, f'{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
    return elapsed


def read(path):
    return json.loads(path.read_text())


def count_csv(path):
    with path.open(newline='') as stream:
        return sum(1 for _ in csv.DictReader(stream))


def journal(path, horizon):
    count = 0
    kinds = collections.Counter()
    batches = collections.Counter()
    accepted = set()
    key = None
    last = None
    with path.open() as stream:
        for line in stream:
            row = json.loads(line)
            count += 1
            require(row['record_no'] == count and 0 <= row['time_ticks'] <= horizon, 'Journal sequence/time')
            if row['phase']:
                current = (row['time_ticks'], row['phase'], row['tie_key'], row['action_sequence'])
                require(key is None or key <= current, 'Journal order')
                key = current
            kinds[row['kind']] += 1
            if row['kind'] in ['reconfiguration', 'dispatch']:
                batches[(row['time_ticks'], row['kind'])] += 1
            if row['kind'] == 'result' and row['decision'] == 'accepted':
                task = row['attempt']['task_id']
                require(task not in accepted, 'Double acceptance in load smoke')
                accepted.add(task)
            last = row
    require(last and last['kind'] == 'run_completed' and last['time_ticks'] == horizon, 'Missing successful horizon')
    require(all(value == 1 for value in batches.values()), 'Repeated availability/dispatch batch')
    return {'journal_records': count, 'executed_events': last['executed_events'],
            'journal_bytes': path.stat().st_size, 'accepted': len(accepted), 'event_kinds': dict(kinds)}


def main():
    if DATA.exists():
        shutil.rmtree(DATA)
    DATA.mkdir(parents=True)
    started = time.perf_counter()
    config = SOURCE / 'tests/data/load_smoke.json'
    settings = read(config)
    generation_seconds = call('generate', '--config', config, '--out', DATA/'input')
    scenario = DATA/'input/scenario.json'
    input_hashes = {name: digest(DATA/'input'/name) for name in ['scenario.json', 'delivery.csv']}
    dimensions = {'nodes': len(settings['scenario']['nodes']), 'tasks': settings['tasks']['count'],
                  'horizon_ticks': settings['scenario']['horizon_ticks'],
                  'heartbeat_period_ticks': settings['scenario']['heartbeat_period_ticks'],
                  'potential_heartbeat_rows': count_csv(DATA/'input/delivery.csv'),
                  'crashes': len(settings['scenario']['crashes']), 'blackouts': len(settings['scenario']['blackouts'])}
    expected_rows = dimensions['nodes'] * (dimensions['horizon_ticks'] // dimensions['heartbeat_period_ticks'])
    require(dimensions['potential_heartbeat_rows'] == expected_rows, 'Incomplete generated finite trace')
    compare_seconds = call('compare', '--scenario', scenario, '--out', DATA/'comparison')
    details = {}
    # A second invocation per variant, deliberately in the opposite order.
    for variant in reversed(VARIANTS):
        detector, scheduler = variant.split('_')
        target = DATA/'reverse'/variant
        repeat_seconds = call('run', '--scenario', scenario, '--detector', detector, '--scheduler', scheduler, '--out', target)
        first = DATA/'comparison'/variant
        hashes = {name: digest(first/name) for name in CANONICAL}
        require(hashes == {name: digest(target/name) for name in CANONICAL}, f'{variant}: order/state leak or noncanonical output')
        replay_seconds = call('metrics', '--run-dir', target)
        require(hashes == {name: digest(target/name) for name in CANONICAL}, f'{variant}: independent replay changed output')
        manifest = read(target/'manifest.json')
        metrics = read(target/'metrics.json')
        require(manifest['status'] == 'complete', 'Incomplete load run')
        require(all(metrics['invariants'].values()), 'Native invariant failure')
        require(metrics['arrived'] == dimensions['tasks'] and metrics['arrived'] == metrics['completed'] + metrics['unfinished'], 'Load task balance')
        require(metrics['unfinished'] == metrics['queued'] + metrics['assigned'], 'Load unfinished balance')
        require(count_csv(target/'tasks.csv') == dimensions['tasks'], 'Task IDs lost in report')
        require(metrics['crashes'] == dimensions['crashes'] and metrics['false_positives'] >= 1, 'Intended crash/false exclusion absent')
        for name, checksum in input_hashes.items():
            require(digest(target/'input'/name) == checksum == digest(first/'input'/name), 'Different inputs between variants')
        events = journal(first/'events.jsonl', dimensions['horizon_ticks'])
        require(events['accepted'] == metrics['completed'], 'Journal/metrics completion mismatch')
        details[variant] = {**events, 'completed': metrics['completed'], 'unfinished': metrics['unfinished'],
                            'retries': metrics['retries'], 'false_positives': metrics['false_positives'],
                            'false_negatives': metrics['false_negatives'],
                            'repeat_run_seconds': repeat_seconds, 'replay_seconds': replay_seconds,
                            'canonical_sha256': hashes}
    require(input_hashes == {name: digest(DATA/'input'/name) for name in input_hashes}, 'Shared inputs modified')
    output_bytes = sum(p.stat().st_size for p in DATA.rglob('*') if p.is_file())
    build = subprocess.run([str(PROGRAM), '--build-info'], capture_output=True, text=True, check=True, timeout=10)
    report = {'schema_version': 1, 'purpose': 'finite software smoke; not an algorithm-performance experiment',
              'dimensions': dimensions, 'seed': settings['seed'], 'config_sha256': digest(config), 'input_sha256': input_hashes,
              'build_info': json.loads(build.stdout), 'host': {'platform': platform.platform(), 'machine': platform.machine(), 'python': platform.python_version()},
              'generation_seconds': generation_seconds, 'compare_four_seconds': compare_seconds,
              'total_verification_seconds': time.perf_counter()-started, 'artifact_bytes_excluding_report': output_bytes,
              'variants': details, 'repeat_order': list(reversed(VARIANTS)),
              'checks': {'balance': True, 'unique_acceptance': True, 'canonical_repeat': True, 'variant_order_isolation': True, 'independent_replay': True}}
    (DATA/'report.json').write_text(json.dumps(report, indent=2, sort_keys=True)+'\n')
    print(json.dumps({'dimensions': dimensions, 'compare_four_seconds': compare_seconds,
                      'total_verification_seconds': report['total_verification_seconds'], 'report': str(DATA/'report.json')}))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
