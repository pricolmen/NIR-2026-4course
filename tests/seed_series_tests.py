"""Focused CLI/provenance checks for the seed-series organizer; stdlib only."""
import csv
from decimal import Decimal
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

PROGRAM, SOURCE, BINARY = map(lambda s: Path(s).resolve(), sys.argv[1:4])
spec = importlib.util.spec_from_file_location('seed_series', SOURCE/'tools/run_seed_series.py')
organizer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(organizer)


def read(p): return json.loads(p.read_text(), parse_float=Decimal)
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def snapshot(p): return {str(f.relative_to(p)): digest(f) for f in p.rglob('*') if f.is_file()}


class SeedSeriesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='seed-series-', dir=BINARY)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.template = self.root/'template.json'
        shutil.copyfile(SOURCE/'examples/generator.json', self.template)

    def command(self, *args, code=0):
        r = subprocess.run([sys.executable, str(SOURCE/'tools/run_seed_series.py'),
                            '--program', str(PROGRAM), *map(str, args)],
                           cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, code, r.stdout+'\n'+r.stderr)
        return r

    def generate(self, name='original', seeds=(202, 101)):
        path = self.root/name
        self.command('--template', self.template, '--seeds', *seeds, '--out', path)
        return path

    def test_generated_and_reused_inputs_mapping_results(self):
        template_bytes = self.template.read_bytes()
        first = self.generate("серия ' $ `")
        before = snapshot(first)
        original = read(first/'series_manifest.json')
        self.assertEqual(original['status'], 'complete')
        self.assertEqual(original['distinct_input_sets'], 2)
        self.assertEqual(original['seeds'], [202, 101])
        self.assertEqual(sum(c['argv'][1]=='generate' for c in original['commands']), 2)
        self.assertTrue(original['source']['files_sha256'])
        self.assertTrue(original['build_info']['simulation_available'])
        batch = read(first/'batch_input.json')
        self.assertEqual(set(batch), {'schema_version', 'runs'})
        for entry in original['entries']:
            config = read(first/entry['generation_config'])
            self.assertEqual(config, dict(read(self.template), seed=entry['seed']))
            self.assertEqual(read(first/entry['generation_manifest'])['seed'], entry['seed'])
            output = first/entry['batch_output']
            expected_number = '000001' if entry['seed']==101 else '000002'
            self.assertEqual(output.name, expected_number)
            self.assertEqual(set(entry['results_sha256']), set(organizer.VARIANTS))
            for variant in organizer.VARIANTS:
                for name in organizer.INPUT_FILES:
                    self.assertEqual((output/variant/'input'/name).read_bytes(), (first/entry['input_files'][name]).read_bytes())
        replay = self.root/'replay'
        r = self.command('--reuse-series', first, '--out', replay)
        self.assertIn('not new independent realizations', r.stdout)
        replayed = read(replay/'series_manifest.json')
        self.assertFalse(any(c['argv'][1]=='generate' for c in replayed['commands']))
        self.assertEqual(sum(c['argv'][1]=='batch' for c in replayed['commands']), 1)
        for a,b in zip(original['entries'], replayed['entries']):
            self.assertEqual(a['input_sha256'], b['input_sha256'])
            self.assertEqual(a['results_sha256'], b['results_sha256'])
            self.assertTrue(b['replay_comparison']['content_equal'])
        self.assertEqual(self.template.read_bytes(), template_bytes)
        self.assertEqual(snapshot(first), before)
        self.assertEqual((first/'batch/comparison.csv').read_bytes(), (replay/'batch/comparison.csv').read_bytes())
        self.assertEqual((first/'batch/summary.csv').read_bytes(), (replay/'batch/summary.csv').read_bytes())
        with (first/'batch/summary.csv').open() as stream:
            self.assertTrue(all(row['runs']=='2' for row in csv.DictReader(stream)))

    def test_output_conflicts_preserve_existing_files(self):
        out = self.root/'existing';out.mkdir();(out/'keep').write_text('old results')
        before = snapshot(out)
        self.command('--template', self.template, '--seeds', 1, '--out', out, code=1)
        self.assertEqual(snapshot(out), before)
        link = self.root/'dangling';target = self.root/'must-not-create'
        link.symlink_to(target, target_is_directory=True)
        self.command('--template', self.template, '--seeds', 1, '--out', link, code=1)
        self.assertTrue(link.is_symlink())
        self.assertFalse(target.exists())

    def test_seed_validation_and_extremes(self):
        for seeds in [('1','1'), ('-1',), ('0.5',), ('18446744073709551616',)]:
            out = self.root/'bad'
            self.command('--template', self.template, '--seeds', *seeds, '--out', out, code=2)
            self.assertFalse(out.exists())
        out = self.generate(seeds=(0, 18446744073709551615))
        self.assertEqual(read(out/'series_manifest.json')['seeds'], [0,18446744073709551615])

    def test_reuse_rejects_corrupted_inputs(self):
        first = self.generate(seeds=(1,))
        state = read(first/'series_manifest.json')
        (first/state['entries'][0]['input_files']['delivery.csv']).write_text('corrupt')
        out = self.root/'bad-replay'
        self.command('--reuse-series', first, '--out', out, code=1)
        self.assertFalse(out.exists())

    def test_explicit_detector_overrides_freeze_workload(self):
        first = self.generate(seeds=(1,));before = snapshot(first)
        patch = self.root/'patch.json';patch.write_text('{"tau0_ticks":30,"kappa":3,"adaptive":{"c":0}}')
        out = self.root/'patched'
        self.command('--reuse-series', first, '--detector-overrides', patch, '--out', out)
        old=read(first/'series_manifest.json')['entries'][0];new=read(out/'series_manifest.json')['entries'][0]
        a=read(first/old['input_files']['scenario.json']);b=read(out/new['input_files']['scenario.json'])
        expected=organizer.apply_overrides(a, read(patch))
        self.assertEqual(b, expected)
        self.assertEqual(a['tasks'], b['tasks'])
        self.assertEqual(old['input_sha256']['delivery.csv'],new['input_sha256']['delivery.csv'])
        self.assertFalse(any(c['argv'][1]=='generate' for c in read(out/'series_manifest.json')['commands']))
        self.assertEqual(snapshot(first), before)
        for bad in [{'tasks':[]}, {'adaptive':{'alpha':1}}]:
            patch.write_text(json.dumps(bad));rejected=self.root/'invalid-patch'
            self.command('--reuse-series', first, '--detector-overrides', patch, '--out', rejected, code=1)
            self.assertFalse(rejected.exists())
        self.command('--reuse-series', first, '--out', first/'nested', code=1)
        self.assertEqual(snapshot(first), before)

    def test_decimal_preservation_and_strict_json(self):
        value={'seed':18446744073709551615,'work':Decimal('1.0000000000000000000000000000001'),
               'x':[Decimal('0.12345678901234567890123456789'),True,None]}
        path=self.root/'exact.json';organizer.save(path,value)
        self.assertEqual(organizer.load(path),value)
        for invalid in ['{"seed":1,"seed":2}', '{"x":NaN}']:
            path.write_text(invalid)
            with self.assertRaises(ValueError):organizer.load(path)

    def test_native_failure_is_not_complete(self):
        value=read(self.template);value['tasks']['work_min']=0
        organizer.save(self.template, value)
        out=self.root/'failure'
        self.command('--template',self.template,'--seeds',1,'--out',out,code=1)
        state=read(out/'series_manifest.json')
        self.assertEqual(state['status'],'failed')
        self.assertNotEqual(state['commands'][-1]['returncode'],0)
        self.assertTrue((out/state['commands'][-1]['stderr']).is_file())
        self.assertFalse((out/'batch').exists())

    def test_constant_templates_do_not_claim_distinct_realizations(self):
        value=read(self.template);value['tasks'].update(work_min=2,work_max=2)
        value['delivery'].update(min_delay_ticks=0,max_delay_ticks=0,loss_probability=0)
        self.template.write_text(json.dumps(value))
        out=self.root/'constant'
        r=self.command('--template',self.template,'--seeds',1,2,'--out',out)
        self.assertEqual(read(out/'series_manifest.json')['distinct_input_sets'],1)
        self.assertIn('identical inputs',r.stdout)


if __name__=='__main__':
    unittest.main(argv=[sys.argv[0]],verbosity=2)
