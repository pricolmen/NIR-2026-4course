"""Tampering and provenance controls for the final experiment verifier."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_experiments as final


class VerificationTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='experiment-verification-')
        self.addCleanup(temp.cleanup);self.root = Path(temp.name).resolve()
        self.directory = self.root / 'seed-demo';self.directory.mkdir()
        self.entry = {'seed': 101, 'input_files': {}, 'input_sha256': {},
                      'results_sha256': {}, 'batch_output': 'batch/runs/1'}
        for name in final.io.INPUT_FILES:
            p = self.directory / name;p.write_text('explicit input ' + name)
            self.entry['input_files'][name] = name;self.entry['input_sha256'][name] = final.io.sha(p)
        for variant in final.io.VARIANTS:
            run = self.directory / self.entry['batch_output'] / variant
            (run / 'input').mkdir(parents=True)
            for name in final.io.INPUT_FILES:
                (run / 'input' / name).write_bytes((self.directory / name).read_bytes())
            self.write(run / 'manifest.json', {'status': 'complete'})
            for name in final.io.RESULT_FILES:
                if name == 'metrics.json':
                    self.write(run / name, {'invariants': {'balance': True}, 'arrived': 2,
                                           'completed': 1, 'unfinished': 1, 'queued': 0, 'assigned': 1})
                else:(run / name).write_text('native ' + name)
            self.entry['results_sha256'][variant] = {n: final.io.sha(run / n) for n in final.io.RESULT_FILES}
        self.manifest = {'status': 'complete', 'seeds': [101], 'entries': [self.entry]}
        self.path = self.directory / 'series_manifest.json';self.write(self.path, self.manifest)
        self.spec = {'directory': 'seed-demo', 'kind': 'seeds', 'cases': 1, 'variant_runs': 4}

    def write(self, path, value):path.write_text(json.dumps(value))

    def test_valid_then_corrupted_input_snapshot_and_output(self):
        _, _, runs = final.study_index(self.root, self.spec)
        self.assertEqual(len(runs), 4)
        p = runs[0] / 'input/scenario.json';before = p.read_bytes();p.write_text('corrupt')
        with self.assertRaisesRegex(ValueError, 'Checksum'):final.study_index(self.root, self.spec)
        p.write_bytes(before)
        (runs[0] / 'events.jsonl').write_text('corrupt')
        with self.assertRaisesRegex(ValueError, 'Checksum'):final.study_index(self.root, self.spec)

    def test_missing_variant_duplicate_seed_and_incomplete_run(self):
        manifest = copy.deepcopy(self.manifest)
        del manifest['entries'][0]['results_sha256']['fixed_ll'];self.write(self.path, manifest)
        with self.assertRaisesRegex(ValueError, 'Incomplete variant'):final.study_index(self.root, self.spec)
        manifest = copy.deepcopy(self.manifest);manifest['seeds'] *= 2;manifest['entries'] *= 2
        self.write(self.path, manifest)
        with self.assertRaisesRegex(ValueError, 'Seed mapping'):final.study_index(self.root, self.spec)
        self.write(self.path, self.manifest)
        self.write(self.directory / self.entry['batch_output'] / 'fixed_rr/manifest.json', {'status': 'failed'})
        with self.assertRaisesRegex(ValueError, 'Incomplete native'):final.study_index(self.root, self.spec)

    def test_provenance_changes_allowed_but_scientific_changes_detected(self):
        first, _, _ = final.study_index(self.root, self.spec)
        run = self.directory / self.entry['batch_output'] / 'fixed_rr'
        self.write(run / 'manifest.json', {'status': 'complete', 'program': '/another/build'})
        second, _, _ = final.study_index(self.root, self.spec)
        self.assertEqual(first, second)
        (run / 'tasks.csv').write_text('changed result')
        self.manifest['entries'][0]['results_sha256']['fixed_rr']['tasks.csv'] = final.io.sha(run / 'tasks.csv')
        self.write(self.path, self.manifest)
        third, _, _ = final.study_index(self.root, self.spec)
        self.assertNotEqual(first, third)

    def test_escaping_paths_rejected(self):
        outside = self.root / 'outside';outside.write_text('x')
        manifest = copy.deepcopy(self.manifest)
        manifest['entries'][0]['input_files']['scenario.json'] = '../outside';self.write(self.path, manifest)
        with self.assertRaisesRegex(ValueError, 'escaping'):final.study_index(self.root, self.spec)

    def test_source_freeze_detects_modification_addition_and_removal(self):
        source = self.root / 'source';source.mkdir();(source / 'CMakeLists.txt').write_text('config')
        (source / 'src').mkdir();p = source / 'src/model.cpp';p.write_text('model')
        lock = {'source_sha256': final.source_index(source)};final.check_sources(source, lock)
        p.write_text('changed')
        with self.assertRaisesRegex(ValueError, 'frozen lock'):final.check_sources(source, lock)
        p.write_text('model');q = source / 'src/new.cpp';q.write_text('new')
        with self.assertRaisesRegex(ValueError, 'frozen lock'):final.check_sources(source, lock)
        q.unlink();p.unlink()
        with self.assertRaisesRegex(ValueError, 'frozen lock'):final.check_sources(source, lock)

    def test_preflight_errors_do_not_overwrite_existing_output(self):
        import argparse
        from unittest.mock import patch
        out = self.root / 'old';out.mkdir();(out / 'keep').write_text('retain')
        args = argparse.Namespace(out=out, results_root=self.root, reproduce=False,
                                  replay_metrics=False, program=None)
        with patch.object(final, 'check_sources'):
            with self.assertRaisesRegex(ValueError, 'Output exists'):final.execute(args)
        self.assertEqual((out / 'keep').read_text(), 'retain')


if __name__ == '__main__':unittest.main(verbosity=2)
