#!/usr/bin/env python3
"""Freeze verification and full reproduction of the experimental stages 1–9.

Reads the reference data; writes only to a new --out. Simulation and metric
calculation remain in telemetry_sim and the existing study drivers.
"""
import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import run_seed_series as io

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / 'experiments/final_stage10'
MANIFESTS = {'seeds': 'series_manifest.json', 'boundary': 'boundary_manifest.json',
             'statistics': 'statistics_manifest.json', **{
                 name: name + '_manifest.json' for name in
                 ('network', 'timeout', 'kappa', 'coefficient', 'window', 'load')}}


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                    ensure_ascii=False).encode()).hexdigest()


def source_index(root):
    """Exclude output, Markdown and the lock itself, avoiding self-reference."""
    files = [root / 'CMakeLists.txt']
    for name in ('cmake', 'app', 'include', 'src', 'tools', 'tests', 'examples', 'experiments'):
        files.extend(p for p in (root / name).rglob('*') if p.is_file()
                     and '__pycache__' not in p.parts and p.suffix not in ('.md', '.pyc')
                     and p != root / 'experiments/final_stage10/lock.json')
    return {str(p.relative_to(root)): io.sha(p) for p in sorted(files)}


def check_sources(root, lock):
    io.require(source_index(root) == lock['source_sha256'],
               'Source/configuration differs from frozen lock; do not silently refresh it')


def verified_file(root, name, expected):
    path = io.contained(root, name)
    io.require(io.sha(path) == expected, 'Checksum mismatch: ' + str(path))
    return path


def contained_directory(root, name):
    path = (root / name).resolve()
    io.require(path.is_relative_to(root) and path.is_dir(), 'Missing/escaping directory: ' + name)
    return path


def entry_index(directory, entry):
    io.require(set(entry['input_sha256']) == set(io.INPUT_FILES), 'Incomplete input fingerprints')
    io.require(set(entry['results_sha256']) == set(io.VARIANTS), 'Incomplete variant fingerprints')
    for name in io.INPUT_FILES:
        verified_file(directory, entry['input_files'][name], entry['input_sha256'][name])
    runs = []
    for variant in io.VARIANTS:
        run = contained_directory(directory, entry['batch_output'] + '/' + variant)
        io.require(read(run / 'manifest.json')['status'] == 'complete', 'Incomplete native run')
        for name in io.INPUT_FILES:
            verified_file(run, 'input/' + name, entry['input_sha256'][name])
        hashes = entry['results_sha256'][variant]
        io.require(set(hashes) == set(io.RESULT_FILES), 'Incomplete result fingerprints')
        for name in io.RESULT_FILES:
            verified_file(run, name, hashes[name])
        metrics = read(run / 'metrics.json')
        io.require(metrics['invariants'] and all(metrics['invariants'].values())
                   and metrics['arrived'] == metrics['completed'] + metrics['unfinished']
                   and metrics['unfinished'] == metrics['queued'] + metrics['assigned'],
                   'Native invariants/task balance failed')
        runs.append(run)
    return {'inputs': entry['input_sha256'], 'results': entry['results_sha256']}, runs


def study_index(root, spec):
    directory = contained_directory(root, spec['directory'])
    manifest = read(directory / MANIFESTS[spec['kind']])
    io.require(manifest['status'] == 'complete', 'Incomplete study: ' + str(directory))
    records = {}; reports = {}; runs = []
    if spec['kind'] == 'statistics':
        for name, fingerprint in manifest['artifacts_sha256'].items():
            verified_file(directory, name, fingerprint)
        # Provenance contains absolute source paths and parent manifest hashes.
        # Scientific tables and all PNG/SVG must match; provenance must be valid
        # in each tree but naturally changes after regeneration.
        reports = {name: fingerprint for name, fingerprint in manifest['artifacts_sha256'].items()
                   if name.endswith(('.csv', '.png', '.svg')) or name == 'figures.json'}
        io.require(sum(n.endswith('.png') for n in reports) == spec['figures']
                   and sum(n.endswith('.svg') for n in reports) == spec['figures'], 'Missing figures')
        io.require(manifest['counts']['variant_runs'] == 7200, 'Wrong statistical population')
        count = manifest['counts']['cases']
    else:
        if spec['kind'] == 'seeds':
            children = [('demo', directory, manifest)]
        elif spec['kind'] == 'boundary':
            children = [(case['id'], directory, {'entries': [case]}) for case in manifest['cases']]
        else:
            children = [(case['id'], contained_directory(directory, 'cases/' + case['id']), None)
                        for case in manifest['cases']]
        io.require(len({c[0] for c in children}) == len(children), 'Duplicate case ID')
        count = len(children)
        for case_id, child_root, child in children:
            child = child if child is not None else read(child_root / 'series_manifest.json')
            if spec['kind'] != 'boundary':
                io.require(child['status'] == 'complete', 'Incomplete seed series')
                io.require([e['seed'] for e in child['entries']] == child['seeds']
                           and len(set(child['seeds'])) == len(child['seeds']), 'Seed mapping invalid')
            for entry in child['entries']:
                key = case_id + '/' + str(entry.get('seed', 'deterministic_1'))
                io.require(key not in records, 'Duplicate realization')
                records[key], current = entry_index(child_root, entry)
                runs.extend(current)
        for path in sorted(directory.rglob('comparison.csv')) + sorted(directory.rglob('summary.csv')):
            reports[str(path.relative_to(directory))] = io.sha(path)
        for path in sorted(directory.glob('*.csv')):
            reports[path.name] = io.sha(path)
    io.require(count == spec['cases'] and len(runs) == spec['variant_runs'], 'Unexpected study size')
    result = {'cases': count, 'realizations': len(records), 'variant_runs': len(runs),
              'content_sha256': digest({'records': records, 'reports': reports})}
    return result, {'records': records, 'reports': reports}, runs


def inventory(root, plan):
    summary = {}; details = {}; runs = []
    for spec in plan['studies']:
        result, detail, current = study_index(root, spec)
        io.require(spec['id'] not in summary, 'Duplicate study ID')
        summary[spec['id']] = result; details[spec['id']] = detail; runs.extend(current)
    io.require(len(runs) == plan['variant_runs'], 'Wrong total run count')
    return summary, details, runs


def commands(root, output, program, python, plan):
    """Use the original generators/runners with exactly the frozen parameters."""
    result = []
    for spec in plan['studies']:
        kind = spec['kind']; out = output / spec['directory']
        if kind == 'seeds':
            command = [python, str(root / 'tools/run_seed_series.py'), '--template',
                       str(root / 'examples/generator.json'), '--seeds', '101', '202']
        elif kind == 'statistics':
            command = [python, str(root / 'tools/run_statistics.py'), '--plan',
                       str(root / spec['plan']), '--results-root', str(output)]
        else:
            script = 'run_boundary_study.py' if kind == 'boundary' else 'run_' + kind + '_series.py'
            command = [python, str(root / 'tools' / script), '--plan', str(root / spec['plan'])]
            if kind in ('timeout', 'kappa', 'coefficient', 'window'):
                command += ['--source-network', str(output / 'experiment-stage2-network')]
        if kind != 'statistics':command += ['--program', str(program)]
        result.append(command + ['--out', str(out)])
    return result


def execute(args):
    plan = read(SETTINGS / 'plan.json'); lock = read(SETTINGS / 'lock.json')
    io.require(plan['schema_version'] == lock['schema_version'] == 1, 'Invalid final schema')
    check_sources(ROOT, lock)
    source = args.results_root.expanduser().resolve()
    io.require(not args.out.exists() and not args.out.is_symlink(), 'Output exists; choose a new directory')
    out = args.out.expanduser().resolve()
    io.require(not out.is_relative_to(source) or out.parent == source,
               'Use a new top-level results directory, not a child of an existing study')
    io.require(not args.replay_metrics or args.reproduce, '--replay-metrics requires --reproduce')
    program = args.program.expanduser().resolve() if args.program else None
    io.require(not args.reproduce or (program and os.access(program, os.X_OK)), 'Executable required for reproduction')
    if args.reproduce:
        for line in (ROOT / 'tools/requirements-statistics.txt').read_text().splitlines():
            if not line.strip() or line.startswith('#'):continue
            name, expected = line.split('==')
            io.require(importlib.metadata.version(name) == expected, 'Statistics package version mismatch: ' + name)
    # Validate frozen references before creating output or launching any process.
    baseline, details, _ = inventory(source, plan)
    io.require(baseline == lock['studies'], 'Reference dataset differs from frozen lock')
    out.mkdir(parents=True)
    state = {'schema_version': 1, 'status': 'incomplete', 'mode': 'reproduce' if args.reproduce else 'verify',
             'reference_root': str(source), 'lock_sha256': io.sha(SETTINGS / 'lock.json'),
             'python': sys.version, 'platform': platform.platform(), 'studies': baseline, 'commands': [],
             'native_variant_runs': 0, 'metrics_replays': 0, 'reference_unchanged': False}
    manifest = out / 'final_manifest.json'; started = time.monotonic()
    io.save(manifest, state)
    try:
        io.save(out / 'reference_index.json', details)
        if args.reproduce:
            state['program'] = str(program); state['program_sha256'] = io.sha(program)
            state['build_info'] = json.loads(subprocess.check_output([str(program), '--build-info'], text=True))
            target = out / 'reproduction'
            with (out / 'reproduction.log').open('w', encoding='utf-8') as log:
                for command in commands(ROOT, target, program, sys.executable, plan):
                    print('Running', Path(command[1]).name, flush=True)
                    log.write('COMMAND ' + json.dumps(command) + '\n'); log.flush()
                    rc = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode
                    state['commands'].append({'argv': command, 'returncode': rc});io.save(manifest, state)
                    io.require(rc == 0, 'Reproduction failed; see reproduction.log')
            repeated, repeated_details, runs = inventory(target, plan)
            io.save(out / 'reproduction_index.json', repeated_details)
            io.require(repeated == baseline, 'Regenerated inputs/results/tables/figures differ from reference')
            state['native_variant_runs'] = len(runs); state['exact_content_match'] = True
            if args.replay_metrics:
                with (out / 'metrics_replay.log').open('w', encoding='utf-8') as log:
                    for run in runs:
                        command = [str(program), 'metrics', '--run-dir', str(run)]
                        log.write('COMMAND ' + json.dumps(command) + '\n');log.flush()
                        rc = subprocess.run(command,
                                            cwd=ROOT, stdout=log, stderr=subprocess.STDOUT).returncode
                        io.require(rc == 0, 'Metrics replay failed: ' + str(run))
                        state['metrics_replays'] += 1
                after, _, _ = inventory(target, plan)
                io.require(after == repeated, 'Metrics replay changed native content')
            io.require(io.sha(program) == state['program_sha256'], 'Executable changed during reproduction')
        after, _, _ = inventory(source, plan)
        io.require(after == baseline, 'Reference content changed during verification')
        check_sources(ROOT, lock); state['reference_unchanged'] = True; state['status'] = 'complete'
    except BaseException as error:
        state['status'] = 'failed';state['error'] = str(error);raise
    finally:
        state['elapsed_seconds'] = time.monotonic() - started;io.save(manifest, state)
    print('PASS: frozen experiments verified;', state['native_variant_runs'], 'reproduced runs;',
          state['metrics_replays'], 'metrics replays. Report:', manifest)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results-root', type=Path, default=Path('results'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--program', type=Path)
    parser.add_argument('--reproduce', action='store_true')
    parser.add_argument('--replay-metrics', action='store_true')
    args = parser.parse_args()
    try:return execute(args)
    except KeyboardInterrupt:print('Interrupted; partial output is not a completed verification.', file=sys.stderr);return 130
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, importlib.metadata.PackageNotFoundError) as error:
        print('Final verification failed: ' + str(error), file=sys.stderr);return 1


if __name__ == '__main__':sys.exit(main())
