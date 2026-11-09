"""Native CLI fixtures for boundary sensitivity and sigma=0; no alternate detector."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode=True
PROGRAM,SOURCE,BINARY=map(lambda p:Path(p).resolve(),sys.argv[1:4])
sys.path.insert(0,str(SOURCE/'tools'))
import run_seed_series as io
import run_boundary_study as boundary


def snapshot(root):return {str(p.relative_to(root)):io.sha(p) for p in root.rglob('*') if p.is_file()}


class BoundaryStudyTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory(prefix='boundary-study-',dir=BINARY);self.addCleanup(temp.cleanup);self.root=Path(temp.name)
        self.plan=self.root/'plan.json';self.settings=io.load(SOURCE/'experiments/boundary_stage9/plan.json');io.save(self.plan,self.settings)

    def call(self,out,code=0):
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_boundary_study.py'),'--plan',str(self.plan),
                          '--program',str(PROGRAM),'--out',str(out)],capture_output=True,text=True,timeout=90,cwd=self.root)
        self.assertEqual(r.returncode,code,r.stdout+r.stderr);return r

    def test_explicit_fixtures_are_complete_and_physically_possible(self):
        before=self.plan.read_bytes();cases=boundary.build_cases(boundary.prepare(self.plan))
        self.assertEqual(len(cases),60);self.assertEqual(len({c['id'] for c in cases}),60)
        for c in cases:
            s=c['scenario'];rows=c['trace'];count=s['horizon_ticks']//s['heartbeat_period_ticks']
            self.assertEqual(len(rows),2*count)
            self.assertEqual([(r['node_id'],r['heartbeat_seq']) for r in rows],[(n,i) for n in [1,2] for i in range(1,count+1)])
            self.assertTrue(all(r['delay_ticks']>=0 and r['lost'] in (0,1) for r in rows))
        c=next(c for c in cases if c['id']=='warmed_adaptive_suspect_minus1')
        self.assertEqual(c['probe_ticks'],79999);self.assertGreater(c['probe_ticks'],4*c['scenario']['heartbeat_period_ticks'])
        self.assertEqual(before,self.plan.read_bytes())

    def test_full_oracles_exact_batch_replay_and_preservation(self):
        before=self.plan.read_bytes();out=self.root/"границы ' $ `";self.call(out);m=io.load(out/'boundary_manifest.json')
        self.assertEqual(m['status'],'complete');self.assertEqual(m['checks']['variant_runs'],240)
        self.assertEqual(m['checks']['equal_time_phase_checks'],28)
        self.assertEqual((m['checks']['sigma_c_comparisons'],m['checks']['sigma_outcome_comparisons']),(48,64))
        self.assertFalse(any(c['argv'][1]=='generate' for c in m['commands']))
        saved=snapshot(out);repeat=self.root/'repeat'
        r=subprocess.run([str(PROGRAM),'batch','--manifest',str(out/'batch_input.json'),'--out',str(repeat)],capture_output=True,text=True,timeout=60)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr);b=io.load(repeat/'batch_manifest.json');self.assertEqual(b['status'],'complete')
        mapping={r['scenario_id']:r['output'] for r in b['runs']}
        for c in m['cases']:
            for variant in io.VARIANTS:
                run=repeat/mapping[c['id']]/variant
                self.assertEqual({n:io.sha(run/n) for n in io.RESULT_FILES},c['results_sha256'][variant])
                self.assertEqual({n:io.sha(run/'input'/n) for n in io.INPUT_FILES},c['input_sha256'])
        self.assertEqual(snapshot(out),saved);self.assertEqual(self.plan.read_bytes(),before)

    def test_invalid_plans_rejected_before_output(self):
        changes=[lambda p:p.update(heartbeat_period_ticks=True),lambda p:p.update(offsets_ticks=[-2,0,1]),
                 lambda p:p.update(sigma_coefficients=[-1]),lambda p:p.update(sigma_coefficients=[True]),
                 lambda p:p.update(sigma_coefficients=[2,io.Decimal('2.0')]),lambda p:p.update(sigma_windows=[1,3]),
                 lambda p:p.update(sigma_windows=[io.Decimal('3.0')]),lambda p:p.update(families=['unknown']),
                 lambda p:p.update(families=['rounded_boundary','rounded_boundary']),lambda p:p.update(alpha=1)]
        for change in changes:
            p=copy.deepcopy(self.settings);change(p);io.save(self.plan,p);out=self.root/'bad'
            self.call(out,code=1);self.assertFalse(out.exists())

    def test_existing_output_is_preserved(self):
        out=self.root/'existing';out.mkdir();(out/'keep').write_text('old');before=snapshot(out)
        self.call(out,code=1);self.assertEqual(snapshot(out),before)

    def test_oracles_reject_wrong_phase_order_and_sigma(self):
        p=copy.deepcopy(self.settings);p['families']=['initial_boundaries','zero_intervals'];io.save(self.plan,p)
        cases=boundary.build_cases(boundary.prepare(self.plan));out=self.root/'oracles';self.call(out)
        m=io.load(out/'boundary_manifest.json');mapping={c['id']:c for c in m['cases']}
        c=next(c for c in cases if c['id']=='initial_failed_exact');run=out/mapping[c['id']]['batch_output']/'fixed_rr';path=run/'events.jsonl'
        records=[json.loads(l) for l in path.read_text().splitlines()]
        hb=next(e for e in records if e['kind']=='heartbeat_observed' and e['message']['node_id']==1)
        timer=next(e for e in records if e['kind']=='timeout_observed' and e['request']['node_id']==1 and e['request']['kind']=='failed' and e['request']['generation']==0)
        timer['record_no']=hb['record_no']-1;path.write_text(''.join(io.encode(e)+'\n' for e in records))
        with self.assertRaisesRegex(ValueError,'delivery must precede'):boundary.verify_case(c,'fixed_rr',run)
        c=next(c for c in cases if c['id']=='zero_intervals_c2');run=out/mapping[c['id']]['batch_output']/'adaptive_rr';path=run/'events.jsonl'
        records=[json.loads(l) for l in path.read_text().splitlines()]
        hb=next(e for e in records if e['kind']=='heartbeat_observed' and e['message']=={'heartbeat_seq':3,'node_id':1})
        hb['thresholds']['sample_std_ticks_decimal']='1';path.write_text(''.join(io.encode(e)+'\n' for e in records))
        with self.assertRaisesRegex(ValueError,'Sigma=0'):boundary.verify_case(c,'adaptive_rr',run)

    def test_native_failure_keeps_failed_manifest_and_logs(self):
        p=copy.deepcopy(self.settings);p['families']=['sigma_zero_gap'];p['sigma_coefficients']=[io.Decimal('1e100')];io.save(self.plan,p)
        out=self.root/'overflow';self.call(out,code=1);m=io.load(out/'boundary_manifest.json')
        self.assertEqual(m['status'],'failed');self.assertNotEqual(m['commands'][-1]['returncode'],0)
        self.assertTrue((out/m['commands'][-1]['stderr']).is_file());self.assertFalse((out/'boundary_checks.csv').exists())

if __name__=='__main__':unittest.main(argv=[sys.argv[0]],verbosity=2)
