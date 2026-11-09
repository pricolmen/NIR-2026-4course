#!/usr/bin/env python3
"""Organize saved seed realizations using telemetry_sim generate and batch only."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
from decimal import Decimal

VARIANTS = ('fixed_rr', 'fixed_ll', 'adaptive_rr', 'adaptive_ll')
RESULT_FILES = ('events.jsonl', 'metrics.json', 'tasks.csv', 'attempts.csv', 'nodes.csv')
INPUT_FILES = ('scenario.json', 'delivery.csv')
MAX_SEED = (1 << 64) - 1


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError('Non-finite JSON number: ' + value)


def load(path):
    # Preserve decimal literals; never narrow generator work/ranges to binary64.
    return json.loads(path.read_text(encoding='utf-8'), parse_float=Decimal,
                      object_pairs_hook=unique_object, parse_constant=reject_constant)


def encode(value):
    """JSON serialization retaining Decimal numbers as numbers, not strings."""
    if isinstance(value, Decimal):
        require(value.is_finite(), 'Non-finite decimal')
        return str(value)
    if isinstance(value, dict):
        return '{' + ','.join(json.dumps(k, ensure_ascii=False) + ':' + encode(v)
                              for k, v in value.items()) + '}'
    if isinstance(value, (tuple, list)):
        return '[' + ','.join(encode(v) for v in value) + ']'
    return json.dumps(value, ensure_ascii=False, allow_nan=False)


def save(path, value):
    temporary = path.with_name('.' + path.name + '.tmp')
    temporary.write_text(encode(value) + '\n', encoding='utf-8')
    temporary.replace(path)


def seed_value(text):
    if not text.isascii() or not text.isdecimal() or not 0 <= int(text) <= MAX_SEED:
        raise argparse.ArgumentTypeError('seed must be a decimal uint64 (0..18446744073709551615)')
    return int(text)


def contained(root, name):
    require(isinstance(name, str) and name and not Path(name).is_absolute(), 'Expected relative manifest path')
    path = (root / name).resolve()
    require(path.is_relative_to(root) and path.is_file(), 'Missing/escaping manifest file: ' + name)
    return path


def source_info(root):
    info = {'directory': str(root), 'binary_source_identity': 'not embedded; source snapshot is recorded separately'}
    try:
        for key, arguments in [('commit', ['rev-parse', 'HEAD']), ('status', ['status', '--porcelain']),
                               ('diff_sha256', ['diff', '--binary', 'HEAD'])]:
            r = subprocess.run(['git', '-C', str(root), *arguments], capture_output=True, check=True)
            info[key] = hashlib.sha256(r.stdout).hexdigest() if key == 'diff_sha256' else r.stdout.decode().strip()
    except (OSError, subprocess.CalledProcessError) as error:
        info['git_unavailable'] = str(error)
    paths = [root / 'CMakeLists.txt']
    for folder in ('cmake', 'app', 'include', 'src'):
        paths.extend(p for p in (root / folder).rglob('*') if p.is_file())
    info['files_sha256'] = {str(p.relative_to(root)): sha(p) for p in sorted(paths) if p.is_file()}
    return info


def overrides(path):
    patch = load(path)
    require(isinstance(patch, dict) and patch, 'Detector overrides must be a nonempty object')
    require(set(patch) <= {'tau0_ticks', 'kappa', 'adaptive'}, 'Only tau0_ticks, kappa and adaptive may be overridden')
    if 'adaptive' in patch:
        require(isinstance(patch['adaptive'], dict) and patch['adaptive'] and
                set(patch['adaptive']) <= {'window', 'min_intervals', 'c'}, 'Invalid adaptive overrides')
    return patch


def apply_overrides(scenario, patch):
    updated = copy.deepcopy(scenario)
    for key, value in patch.items():
        if key == 'adaptive':
            updated[key].update(value)
        else:
            updated[key] = value
    # All non-detector fields, particularly tasks/delivery_trace, are unchanged.
    require({k: v for k, v in updated.items() if k not in patch} ==
            {k: v for k, v in scenario.items() if k not in patch}, 'Non-detector configuration changed')
    return updated


def execute(args):
    program = args.program.expanduser().resolve()
    require(program.is_file() and os.access(program, os.X_OK), 'Executable is unavailable: ' + str(program))
    requested_out = args.out.expanduser()
    require(not requested_out.exists() and not requested_out.is_symlink(),
            'Output already exists; choose a new directory: ' + str(requested_out))
    out = requested_out.resolve()
    patch = overrides(args.detector_overrides) if args.detector_overrides else {}
    template = args.template.expanduser().resolve() if args.template else None
    previous_root = args.reuse_series.expanduser().resolve() if args.reuse_series else None
    watched = {program: sha(program)}
    if template:
        config = load(template)
        require(isinstance(config, dict) and 'seed' in config, 'Template must contain seed')
        watched[template] = sha(template)
        seeds = args.seeds
        scenario_id = args.scenario_id or template.stem
        previous = None
    else:
        require(previous_root.is_dir() and not out.is_relative_to(previous_root),
                'Reuse output must be outside the original series')
        prior_file = previous_root / 'series_manifest.json'
        previous = load(prior_file)
        require(previous.get('schema_version') == 1 and previous.get('status') == 'complete', 'Source series is not complete')
        require(previous.get('entries'), 'Source series has no inputs')
        seeds = []
        for entry in previous['entries']:
            seed = entry['seed']
            require(type(seed) is int and 0 <= seed <= MAX_SEED and seed not in seeds, 'Invalid/duplicate saved seed')
            seeds.append(seed)
            for name in INPUT_FILES:
                p = contained(previous_root, entry['input_files'][name])
                require(sha(p) == entry['input_sha256'][name], 'Saved input checksum mismatch: ' + str(p))
                watched[p] = sha(p)
            for key in ('generation_config', 'generation_manifest'):
                p = contained(previous_root, entry[key])
                require(sha(p) == entry[key + '_sha256'], 'Generation provenance checksum mismatch: ' + str(p))
                watched[p] = sha(p)
        watched[prior_file] = sha(prior_file)
        scenario_id = None
    require(seeds and len(seeds) == len(set(seeds)), 'An explicit nonempty list of distinct seeds is required')
    require(scenario_id is None or scenario_id and '\0' not in scenario_id, 'Invalid scenario ID')
    if args.detector_overrides:
        watched[args.detector_overrides.resolve()] = sha(args.detector_overrides.resolve())
    out.mkdir(parents=True, exist_ok=False)
    (out / 'logs').mkdir()
    (out / 'configs').mkdir()
    (out / 'inputs').mkdir()
    state = {'schema_version': 1, 'status': 'incomplete', 'mode': 'generated' if template else 'reused',
             'realization_note': 'different seeds generate candidate independent realizations; reuse is a deterministic re-execution, not a new independent sample',
             'seeds': seeds, 'parameter_overrides': patch, 'entries': [], 'commands': [],
             'invocation': [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]],
             'wrapper_sha256': sha(Path(__file__).resolve()), 'program': str(program), 'program_sha256': watched[program],
             'python': platform.python_version(), 'platform': platform.platform(),
             'source': source_info(args.source_dir.resolve())}
    manifest = out / 'series_manifest.json'
    def checkpoint(): save(manifest, state)
    def call(*arguments):
        index = len(state['commands']) + 1
        stdout = Path('logs') / ('%04d.stdout.log' % index)
        stderr = Path('logs') / ('%04d.stderr.log' % index)
        record = {'argv': [str(program), *map(str, arguments)], 'cwd': str(out),
                  'stdout': str(stdout), 'stderr': str(stderr), 'returncode': None}
        state['commands'].append(record); checkpoint()
        started = time.perf_counter()
        with (out / stdout).open('w', encoding='utf-8') as output, (out / stderr).open('w', encoding='utf-8') as errors:
            r = subprocess.run(record['argv'], cwd=out, stdout=output, stderr=errors, text=True)
        record.update(returncode=r.returncode, seconds=time.perf_counter() - started); checkpoint()
        require(r.returncode == 0, 'CLI failed (exit %s); see %s' % (r.returncode, out / stderr))
        return out / stdout
    try:
        checkpoint()
        state['build_info'] = load(call('--build-info'))
        require(state['build_info'].get('program') == 'telemetry_sim' and state['build_info'].get('simulation_available') is True,
                'Executable does not report the required simulator CLI')
        if template:
            shutil.copyfile(template, out / 'template.json')
            state['template_source'] = str(template); state['template_sha256'] = watched[template]
        else:
            (out / 'provenance').mkdir()
            shutil.copyfile(previous_root / 'series_manifest.json', out / 'provenance/source_series_manifest.json')
            state['reuse_source'] = str(previous_root)
            state['reuse_manifest_sha256'] = watched[previous_root / 'series_manifest.json']
        if patch:
            shutil.copyfile(args.detector_overrides, out / 'detector_overrides.json')
        for index, seed in enumerate(seeds):
            repeat = 'seed_%020d' % seed
            directory = out / 'inputs' / ('seed_%d' % seed)
            if template:
                configuration = copy.deepcopy(config); configuration['seed'] = seed
                cp = out / 'configs' / ('seed_%d.json' % seed); save(cp, configuration)
                require(load(cp) == dict(config, seed=seed), 'Template settings changed beyond seed')
                call('generate', '--config', cp, '--out', directory)
                gm = directory / 'generation_manifest.json'; gc = directory / 'generator_config.json'
                require(gc.read_bytes() == cp.read_bytes() and load(gm)['status'] == 'complete' and load(gm)['seed'] == seed,
                        'Generator seed/configuration provenance mismatch')
                entry = {'seed': seed, 'scenario_id': scenario_id, 'repeat_id': repeat}
            else:
                source = previous['entries'][index]
                directory.mkdir()
                for name in INPUT_FILES:
                    shutil.copyfile(contained(previous_root, source['input_files'][name]), directory / name)
                provenance = out / 'provenance' / ('seed_%d' % seed); provenance.mkdir()
                gc = provenance / 'generator_config.json'; gm = provenance / 'generation_manifest.json'
                for key, target in [('generation_config', gc), ('generation_manifest', gm)]:
                    shutil.copyfile(contained(previous_root, source[key]), target)
                if patch:
                    shutil.copyfile(directory / 'scenario.json', provenance / 'source_scenario.json')
                    save(directory / 'scenario.json', apply_overrides(load(directory / 'scenario.json'), patch))
                entry = {'seed': seed, 'scenario_id': source['scenario_id'], 'repeat_id': source['repeat_id'],
                         'source_input_sha256': source['input_sha256']}
            entry.update(input_files={n: str((directory / n).relative_to(out)) for n in INPUT_FILES},
                         input_sha256={n: sha(directory / n) for n in INPUT_FILES},
                         generation_config=str(gc.relative_to(out)), generation_config_sha256=sha(gc),
                         generation_manifest=str(gm.relative_to(out)), generation_manifest_sha256=sha(gm))
            if previous:
                require(entry['input_sha256']['delivery.csv'] == source['input_sha256']['delivery.csv'], 'Frozen delivery changed')
                require(load(directory / 'scenario.json')['tasks'] == load(contained(previous_root, source['input_files']['scenario.json']))['tasks'],
                        'Frozen tasks changed')
                if not patch:
                    require(entry['input_sha256'] == source['input_sha256'], 'Replay input bytes changed')
            state['entries'].append(entry); checkpoint()
        batch = {'schema_version': 1, 'runs': [{'scenario_id': e['scenario_id'], 'repeat_id': e['repeat_id'],
                                                'scenario': e['input_files']['scenario.json']} for e in state['entries']]}
        save(out / 'batch_input.json', batch)
        call('batch', '--manifest', out / 'batch_input.json', '--out', out / 'batch')
        bm = load(out / 'batch/batch_manifest.json')
        require(bm['status'] == 'complete' and len(bm['runs']) == len(seeds), 'Incomplete/wrong batch')
        mapping = {(r['scenario_id'], r['repeat_id']): r for r in bm['runs']}
        require(len(mapping) == len(seeds), 'Duplicate batch mapping')
        for index, entry in enumerate(state['entries']):
            row = mapping[entry['scenario_id'], entry['repeat_id']]
            relative = Path('batch') / row['output']
            require(not relative.is_absolute() and (out / relative).resolve().is_relative_to(out), 'Invalid batch output path')
            directory = out / relative
            require(load(directory / 'comparison_manifest.json')['status'] == 'complete', 'Incomplete comparison')
            require(sorted(p.name for p in directory.iterdir() if p.is_dir() and p.name != 'input') == sorted(VARIANTS), 'Expected exactly four variants')
            for name in INPUT_FILES:
                require(sha(contained(out, entry['input_files'][name])) == entry['input_sha256'][name], 'Batch modified its input')
                require(sha(directory / 'input' / name) == entry['input_sha256'][name], 'Shared comparison input mismatch')
            entry['batch_output'] = str(relative)
            entry['results_sha256'] = {}
            for variant in VARIANTS:
                run = directory / variant
                require(load(run / 'manifest.json')['status'] == 'complete', 'Incomplete variant')
                for name in INPUT_FILES:
                    require(sha(run / 'input' / name) == entry['input_sha256'][name], 'Variant inputs differ: ' + variant)
                entry['results_sha256'][variant] = {name: sha(run / name) for name in RESULT_FILES}
            if previous and not patch:
                same_binary = previous['program_sha256'] == state['program_sha256']
                equal = entry['results_sha256'] == previous['entries'][index]['results_sha256']
                entry['replay_comparison'] = {'same_binary': same_binary, 'content_equal': equal}
                if same_binary:
                    require(equal, 'Saved-input results differ on the same executable')
        state['distinct_input_sets'] = len({tuple(e['input_sha256'][n] for n in INPUT_FILES) for e in state['entries']})
        for path, checksum in watched.items(): require(sha(path) == checksum, 'Source/program changed during execution: ' + str(path))
        state['status'] = 'complete'; checkpoint()
    except BaseException as error:
        state['status'] = 'failed'; state['error'] = str(error); checkpoint(); raise
    print('Saved seed series:', out)
    print('Mode:', state['mode'], '; seeds:', ', '.join(map(str, seeds)), '; input sets:', state['distinct_input_sets'])
    print('Verified: 4 variants per seed, exact saved input hashes. Mapping: series_manifest.json')
    if previous:
        print('Saved inputs reused without generation. This is re-execution, not new independent realizations.')
    elif state['distinct_input_sets'] < len(seeds):
        print('Some seeds produced identical inputs; do not count identical inputs as distinct observed realizations.')
    print('This infrastructure smoke run does not establish algorithm superiority.')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--template', type=Path, help='Generator JSON template; only seed is changed')
    mode.add_argument('--reuse-series', type=Path, help='Previous complete series directory; no generate calls')
    parser.add_argument('--seeds', nargs='+', type=seed_value)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='New directory; never overwritten')
    parser.add_argument('--scenario-id', help='Batch grouping label (generation mode only)')
    parser.add_argument('--detector-overrides', type=Path, help='Explicit detector-only JSON patch (reuse mode only)')
    parser.add_argument('--source-dir', type=Path, default=Path(__file__).resolve().parents[1], help='Source tree recorded for provenance')
    args = parser.parse_args()
    if args.template and (not args.seeds or args.detector_overrides):
        parser.error('--template requires --seeds and does not accept --detector-overrides')
    if args.reuse_series and (args.seeds or args.scenario_id):
        parser.error('--reuse-series preserves saved seeds and IDs; omit --seeds/--scenario-id')
    if args.seeds and len(args.seeds) != len(set(args.seeds)):
        parser.error('Duplicate seeds are not allowed')
    try:
        return execute(args)
    except KeyboardInterrupt:
        print('Interrupted; completed source inputs preserved.', file=sys.stderr); return 130
    except (OSError, ValueError, KeyError, TypeError, ArithmeticError, subprocess.SubprocessError) as error:
        print('Seed series failed: ' + str(error), file=sys.stderr); return 1

if __name__ == '__main__':
    sys.exit(main())
