"""End-to-end CLI checks; standard library only, no shell and no assert statements."""
import csv
import io
import json
import pathlib
import shutil
import subprocess
import sys

PROGRAM = pathlib.Path(sys.argv[1])
SOURCE = pathlib.Path(sys.argv[2])
DATA = pathlib.Path(sys.argv[3]) / 'stage12-tests' / sys.argv[4]
GROUP = sys.argv[4]
VARIANTS = ['fixed_rr', 'fixed_ll', 'adaptive_rr', 'adaptive_ll']


def require(value, message):
    if not value:
        raise RuntimeError(message)


def call(*args, code=0, cwd=None):
    result = subprocess.run([str(PROGRAM), *map(str, args)], cwd=cwd or DATA,
                            text=True, capture_output=True, timeout=25)
    require(result.returncode == code, f'{args}: exit {result.returncode}, expected {code}\n{result.stdout}\n{result.stderr}')
    return result


def source(name='baseline'):
    return SOURCE / 'examples' / name / 'scenario.json'


def read(path):
    return json.loads(path.read_text())


def table(path):
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream))


def put(path, value):
    path.write_text(json.dumps(value))


def snapshot(path):
    return {str(p.relative_to(path)): p.read_bytes() for p in path.rglob('*') if p.is_file()}


def fixture(name, mutate=None, base="baseline"):
    path = DATA / name
    path.mkdir()
    value = read(source(base))
    if mutate:
        mutate(value)
    put(path / 'scenario.json', value)
    shutil.copyfile(source(base).parent / 'delivery.csv', path / 'delivery.csv')
    return path / 'scenario.json'


def run(out, scenario=None, *extra, code=0):
    return call('run', '--scenario', scenario or source(), '--detector', 'fixed', '--scheduler', 'rr', '--out', out, *extra, code=code)


def parsing():
    call(code=2)
    for args in [('--unknown',), ('what',), ('--help', 'extra'), ('run',), ('metrics', '--run-dir'),
                 ('validate', '--scenario', source(), '--oops'), ('run', '--scenario', source(), '--detector', 'bad', '--scheduler', 'rr', '--out', DATA/'out'),
                 ('validate', '--scenario', source(), '--scenario', source()), ('validate', '--scenario', source(), '--overwrite'),
                 ('compare', '--scenario', source(), '--out', DATA/'out', '--overwrite', '--overwrite')]:
        call(*args, code=2)
    require(not (DATA/'out').exists(), 'Bad arguments created output')
    for name in ['validate', 'run', 'compare', 'metrics', 'generate', 'batch']:
        require('Usage:' in call(name, '--help').stdout, 'Missing command help')
    require(read_build_info()['simulation_available'] is True, 'CLI build metadata is stale')


def read_build_info():
    return json.loads(call('--build-info').stdout)


def validate():
    require('tasks=4' in call('validate', '--scenario', source(), cwd=DATA).stdout, 'Validation counts')
    bad = fixture('bad', lambda value: value.update(horizon_ticks=1.5))
    require('horizon_ticks' in call('validate', '--scenario', bad, code=1).stderr, 'Missing input field context')
    call('validate', '--scenario', DATA/'missing.json', code=1)
    for empty_tasks in [False, True]:
        name = 'no-nodes-empty-tasks' if empty_tasks else 'no-nodes-with-tasks'
        invalid = fixture(name, lambda value: value.update(nodes=[], **({'tasks': []} if empty_tasks else {})))
        (invalid.parent/'delivery.csv').write_text('node_id,heartbeat_seq,delay_ticks,lost\n')
        require('$.nodes' in call('validate', '--scenario', invalid, code=1).stderr, 'Empty node catalog needs field context')
        out = DATA/(name+'-out')
        require('$.nodes' in run(out, invalid, code=1).stderr, 'run accepted an empty node catalog')
        require('$.nodes' in call('compare', '--scenario', invalid, '--out', out, code=1).stderr, 'compare accepted an empty node catalog')
        manifest = DATA/(name+'-batch.json')
        put(manifest, {'schema_version': 1, 'runs': [{'scenario_id': name, 'repeat_id': '1', 'scenario': str(invalid)}]})
        require('$.nodes' in call('batch', '--manifest', manifest, '--out', out, code=1).stderr, 'batch accepted an empty node catalog')
        require(not out.exists(), 'Invalid node catalog published results')
    one = fixture('one-node', lambda value: value.update(nodes=value['nodes'][:1]))
    (one.parent/'delivery.csv').write_text('node_id,heartbeat_seq,delay_ticks,lost\n1,1,0,0\n1,2,0,0\n')
    require('nodes=1' in call('validate', '--scenario', one).stdout, 'Single node rejected')
    call('compare', '--scenario', one, '--out', DATA/'one-node-out')
    require(all(read(DATA/'one-node-out'/v/'metrics.json')['completed'] == 4 for v in VARIANTS), 'Single-node simulation failed')


def single_run():
    unusual = DATA / "результат с пробелом ' $ `"
    run(unusual)
    require(read(unusual/'manifest.json')['status'] == 'complete', 'Run incomplete')
    metrics = read(unusual/'metrics.json')
    require(metrics['completed'] == 4 and metrics['mean_latency']['value_ticks'] == 4.5, 'Manual baseline')
    require(metrics['p95_latency']['value_ticks'] == 8 and metrics['throughput_windows'][0]['tasks_per_second'] == 200000, 'Manual P95/throughput')
    old = snapshot(unusual)
    run(unusual, code=1)
    require(snapshot(unusual) == old, 'Refused run changed output')
    unfinished = fixture('unfinished', lambda value: value.update(tasks=[{'id': 1, 'arrival_ticks': 20, 'work': 100}]))
    run(DATA/'unfinished-run', unfinished)
    require(read(DATA/'unfinished-run/metrics.json')['unfinished'] == 1, 'Incomplete task is a valid run')


def compare():
    out = DATA/'comparison'
    call('compare', '--scenario', source(), '--out', out)
    rows = table(out/'comparison.csv')
    require([r['variant'] for r in rows] == VARIANTS, 'Variant order')
    require([r['p95_latency_ticks'] for r in rows] == ['8', '6', '8', '6'], 'Manual RR/LL differences')
    inputs = []
    for name in VARIANTS:
        run_dir = out/name
        inputs.append(snapshot(run_dir/'input'))
        call('metrics', '--run-dir', run_dir)
    require(all(i == inputs[0] for i in inputs), 'Variants used different saved inputs')
    require(read(out/'comparison_manifest.json')['status'] == 'complete', 'Comparison incomplete')
    # Overlapping and empty window sets remain distinct and explicit.
    multi = fixture('multi', lambda v: v.update(measurement_windows=[{'start_ticks': 0, 'end_ticks': 4}, {'start_ticks': 4, 'end_ticks': 20}]))
    call('compare', '--scenario', multi, '--out', DATA/'multi-out')
    require(len(table(DATA/'multi-out/comparison.csv')) == 8, 'Lost throughput windows')
    empty = fixture('empty', lambda v: v.update(tasks=[], measurement_windows=[]))
    call('compare', '--scenario', empty, '--out', DATA/'empty-out')
    for row in table(DATA/'empty-out/comparison.csv'):
        require(row['mean_latency_ticks'] == '' and row['latency_reason'] == 'empty_sample' and row['throughput_reason'] == 'no_measurement_window', 'Null reasons')


def metrics():
    out = DATA/'run'
    run(out)
    before = snapshot(out)
    (out/'metrics.json').write_text('broken cached metrics')
    call('metrics', '--run-dir', out)
    require(snapshot(out) == before, 'Independent metrics did not restore derived reports')
    (out/'events.jsonl').write_bytes((out/'events.jsonl').read_bytes()[:-20])
    damaged = snapshot(out)
    call('metrics', '--run-dir', out, code=1)
    require(snapshot(out) == damaged, 'Failed replay modified reports')


def generate():
    config = read(SOURCE/'examples/generator.json')
    config['seed'] = 18446744073709551615
    config['tasks'].update(count=5, first_arrival_ticks=0, period_ticks=2, work_min=2, work_max=2)
    config['delivery'].update(min_delay_ticks=3, max_delay_ticks=3, loss_probability=1)
    config['scenario']['crashes'] = [{'node_id': 1, 'at_ticks': 3}]
    config['scenario']['blackouts'] = [{'node_id': 2, 'start_ticks': 1, 'end_ticks': 3}]
    path = DATA/'config.json'
    put(path, config)
    for name in ['a', 'b']:
        call('generate', '--config', path, '--out', DATA/name)
        call('validate', '--scenario', DATA/name/'scenario.json')
    require(snapshot(DATA/'a') == snapshot(DATA/'b'), 'Seed replay differs')
    scenario = read(DATA/'a/scenario.json')
    require([t['arrival_ticks'] for t in scenario['tasks']] == [0, 2, 4, 6, 8] and all(t['work'] == 2 for t in scenario['tasks']), 'Periodic tasks')
    rows = table(DATA/'a/delivery.csv')
    require(len(rows) == 4 and all(r['lost'] == '1' and r['delay_ticks'] == '3' for r in rows), 'Complete trace includes sends after crash')
    call('compare', '--scenario', DATA/'a/scenario.json', '--out', DATA/'generated-comparison')
    config['tasks'].update(count=0)
    config['delivery'].update(loss_probability=0)
    put(path, config)
    call('generate', '--config', path, '--out', DATA/'zero')
    require(read(DATA/'zero/scenario.json')['tasks'] == [] and all(r['lost'] == '0' for r in table(DATA/'zero/delivery.csv')), 'Empty task stream/q=0')
    for value in [-1, 0.5, True, 18446744073709551616]:
        bad = dict(config, seed=value)
        put(path, bad)
        call('generate', '--config', path, '--out', DATA/'bad', code=1)
        require(not (DATA/'bad').exists(), 'Invalid config published')
    for name in ['random-a', 'random-b']:
        call('generate', '--config', SOURCE/'examples/generator.json', '--out', DATA/name)
    require(snapshot(DATA/'random-a') == snapshot(DATA/'random-b'), 'Random finite-range seed replay')
    generated = read(DATA/'random-a/scenario.json')
    require(all(1 <= t['work'] <= 4 for t in generated['tasks']), 'Random work range')
    require(all(0 <= int(r['delay_ticks']) <= 3 for r in table(DATA/'random-a/delivery.csv')), 'Random delay range')
    mutations = [lambda v: v['delivery'].update(loss_probability=1.1),
                 lambda v: v['delivery'].update(min_delay_ticks=5, max_delay_ticks=1),
                 lambda v: v['tasks'].update(period_ticks=0), lambda v: v['tasks'].update(work_min=0),
                 lambda v: v['scenario'].update(horizon_ticks=9223372036854775808),
                 lambda v: v['scenario']['nodes'].append(v['scenario']['nodes'][0]),
                 lambda v: v['scenario'].update(nodes=[]),
                 lambda v: v['scenario'].update(crashes=[{'node_id': 999, 'at_ticks': 0}]),
                 lambda v: v['scenario'].update(measurement_windows=[{'start_ticks': 3, 'end_ticks': 3}]),
                 lambda v: v.update(unknown_field=1)]
    for mutate in mutations:
        bad = json.loads(json.dumps(config))
        mutate(bad)
        put(path, bad)
        call('generate', '--config', path, '--out', DATA/'invalid', code=1)
        require(not (DATA/'invalid').exists(), 'Malformed generator configuration published')
    config['tasks'].update(count=100)
    put(path, config)
    call('generate', '--config', path, '--out', DATA/'bad', code=1)


def batch():
    first = fixture('first')
    second = fixture('second', lambda value: value.update(tasks=[]))
    manifest = DATA/'experiments.json'
    label = 'experiment, "x"\nnext'
    put(manifest, {'schema_version': 1, 'runs': [
        {'scenario_id': label, 'repeat_id': 'b', 'scenario': str(second.relative_to(DATA))},
        {'scenario_id': label, 'repeat_id': 'a', 'scenario': str(first.relative_to(DATA))}]})
    out = DATA/'batch'
    call('batch', '--manifest', manifest, '--out', out, cwd=SOURCE)
    rows = table(out/'comparison.csv')
    require(len(rows) == 8 and all(r['scenario_id'] == label for r in rows), 'Labels/CSV quoting')
    for row in table(out/'summary.csv'):
        require(row['runs'] == '2' and row['latency_run_samples'] == '1' and row['p95_run_samples'] == '1', 'Defined-only per-run sample sizes')
        require(row['mean_run_latency_ticks'] == '4.5' and row['detection_run_samples'] == '0' and row['detection_reason'] == 'empty_sample', 'Summary per-run means')
        require(row['detection_node_observations'] == '0' and row['throughput_run_samples'] == '2', 'Separate node/run observations')
    info = read(out/'batch_manifest.json')
    require(info['status'] == 'complete' and info['runs'][0]['repeat_id'] == 'a', 'Batch order')
    one = fixture('one-crash', base='crash')
    two = fixture('two-crashes', lambda v: v['crashes'].append({'node_id': 2, 'at_ticks': 9}), base='crash')
    failure_manifest = DATA/'failure-experiments.json'
    put(failure_manifest, {'schema_version': 1, 'runs': [
        {'scenario_id': 'failure', 'repeat_id': '1', 'scenario': str(one)},
        {'scenario_id': 'failure', 'repeat_id': '2', 'scenario': str(two)}]})
    call('batch', '--manifest', failure_manifest, '--out', DATA/'failure-batch')
    fixed = next(r for r in table(DATA/'failure-batch/summary.csv') if r['variant'] == 'fixed_rr')
    require(float(fixed['mean_run_detection_ticks']) == 16.75 and fixed['detection_run_samples'] == '2' and
            fixed['detection_node_observations'] == '3', 'Run means were silently pooled over failed nodes')
    original = read(manifest)
    original['runs'].append(original['runs'][0])
    put(manifest, original)
    call('batch', '--manifest', manifest, '--out', DATA/'bad', code=1)
    require(not (DATA/'bad').exists(), 'Duplicate pair published')


def overwrite():
    out = DATA/'out'
    run(out)
    old = snapshot(out)
    result = run(out, None, '--overwrite')
    backups = list(DATA.glob('.out.previous-*'))
    require(len(backups) == 1 and snapshot(backups[0]) == old and 'Previous output:' in result.stdout, 'Overwrite must retain old output')
    require(snapshot(out) == old, 'Successful overwrite changed canonical single run')
    protected = fixture('protected')
    before = snapshot(protected.parent)
    run(protected.parent, protected, '--overwrite', code=1)
    require(snapshot(protected.parent) == before, 'Overwrite replaced input')
    call('compare', '--scenario', source(), '--out', SOURCE, '--overwrite', code=1)
    # A trailing slash is still an output directory, not its own workspace.
    run(str(DATA/'trailing') + '/')
    require((DATA/'trailing/metrics.json').exists(), 'Trailing separator output')
    link = DATA/'link'
    link.symlink_to(out, target_is_directory=True)
    run(link, None, '--overwrite', code=1)
    require(snapshot(out) == old, 'Symlink overwrite changed target')


def failures():
    out = DATA/'out'
    out.mkdir()
    (out/'sentinel').write_text('keep')
    bad = fixture('bad', lambda v: v.update(horizon_ticks=-1))
    run(out, bad, '--overwrite', code=1)
    require((out/'sentinel').read_text() == 'keep' and not list(DATA.glob('.out.previous-*')), 'Failed replacement changed old output')
    blocker = DATA/'file'
    blocker.write_text('not a directory')
    run(blocker/'child', code=1)
    require(blocker.read_text() == 'not a directory', 'Write failure corrupted unrelated file')
    call('batch', '--manifest', DATA/'missing', '--out', DATA/'missing-out', code=1)
    call('metrics', '--run-dir', DATA/'missing', code=1)
    require(not list(DATA.glob('*.work-*')), 'Workspace leaked after failure')


def determinism():
    for name in ['a', 'b']:
        call('compare', '--scenario', source('crash'), '--out', DATA/name)
    for name in VARIANTS:
        for file in ['manifest.json', 'events.jsonl', 'metrics.json', 'tasks.csv', 'attempts.csv', 'nodes.csv']:
            require((DATA/'a'/name/file).read_bytes() == (DATA/'b'/name/file).read_bytes(), f'Noncanonical {name}/{file}')
    require((DATA/'a/comparison.csv').read_bytes() == (DATA/'b/comparison.csv').read_bytes(), 'Comparison differs')


if __name__ == '__main__':
    try:
        if DATA.exists():
            shutil.rmtree(DATA)
        DATA.mkdir(parents=True)
        {'parsing': parsing, 'validate': validate, 'run': single_run, 'compare': compare,
         'metrics': metrics, 'generate': generate, 'batch': batch, 'overwrite': overwrite,
         'failures': failures, 'determinism': determinism}[GROUP]()
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
