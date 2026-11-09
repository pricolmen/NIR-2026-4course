"""Coefficient reuse/controls tests with real generate/batch; no metric reimplementation."""
import copy
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode=True
PROGRAM,SOURCE,BINARY=map(lambda p:Path(p).resolve(),sys.argv[1:4])
sys.path.insert(0,str(SOURCE/'tools'))
import run_seed_series as seeds
import run_coefficient_series as coefficient


def snapshot(root):return {str(p.relative_to(root)):seeds.sha(p) for p in root.rglob('*') if p.is_file()}


class CoefficientSeriesTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory(prefix='coefficient-series-',dir=BINARY);self.addCleanup(temp.cleanup)
        self.root=Path(temp.name);self.source=self.root/'source'
        self.settings={'schema_version':1,'seeds':[502,501],'c':[0,seeds.Decimal('0.5'),2],
                       'source_cases':['healthy_j1_q0','crash_j1_q0']}
        self.plan=self.root/'coefficient.json';seeds.save(self.plan,self.settings)

    def source_fixture(self):
        base=seeds.load(SOURCE/'examples/generator.json')
        base['scenario']['horizon_ticks']=80;base['scenario']['measurement_windows']=[{'start_ticks':0,'end_ticks':80}]
        seeds.save(self.root/'base.json',base)
        network={'schema_version':1,'base_generator':'base.json','seeds':[502,501],'delay_center_ticks':2,
                 'jitter_half_width_ticks':[1],'loss_probabilities':[0],
                 'modes':[{'id':'healthy','crashes':[]},{'id':'crash','crashes':[{'node_id':1,'at_ticks':35}]}]}
        seeds.save(self.root/'network.json',network)
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_network_series.py'),'--plan',str(self.root/'network.json'),
                          '--program',str(PROGRAM),'--out',str(self.source)],capture_output=True,text=True,timeout=60)
        self.assertEqual(r.returncode,0,r.stdout+'\n'+r.stderr)

    def call(self,out,code=0):
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_coefficient_series.py'),'--plan',str(self.plan),
                          '--source-network',str(self.source),'--program',str(PROGRAM),'--out',str(out)],
                         capture_output=True,text=True,timeout=90,cwd=self.root)
        self.assertEqual(r.returncode,code,r.stdout+'\n'+r.stderr)
        return r

    def test_frozen_inputs_baselines_and_native_rows(self):
        self.source_fixture();before=snapshot(self.source);plan_before=self.plan.read_bytes()
        out=self.root/"пороги ' $ `";self.call(out)
        m=seeds.load(out/'coefficient_manifest.json')
        self.assertEqual(m['status'],'complete');self.assertEqual(len(m['cases']),6)
        self.assertEqual(m['checks']['variant_runs_checked'],48)
        self.assertEqual(m['checks']['baseline_variant_runs_identical_to_stage2'],16)
        self.assertEqual(m['checks']['generation_calls'],0)
        self.assertEqual(m['checks']['fixed_control_runs_identical_to_stage2'],24)
        self.assertEqual(m['comparison_rows'],48);self.assertEqual(m['summary_rows'],24)
        for c in m['cases']:
            parent=seeds.load(self.source/'cases'/c['source_case']/'series_manifest.json')
            child=seeds.load(out/'cases'/c['id']/'series_manifest.json')
            self.assertTrue(all(r['argv'][1]!='generate' for r in child['commands']))
            for a,b in zip(parent['entries'],child['entries']):
                old=seeds.load(self.source/'cases'/c['source_case']/a['input_files']['scenario.json'])
                new=seeds.load(out/'cases'/c['id']/b['input_files']['scenario.json'])
                self.assertEqual(new,dict(old,adaptive=dict(old['adaptive'],c=c['c'])))
                self.assertEqual(b['input_sha256']['delivery.csv'],a['input_sha256']['delivery.csv'])
                self.assertEqual(new['tasks'],old['tasks'])
                self.assertEqual(new['tau0_ticks'],old['tau0_ticks'])
                self.assertEqual(new['adaptive']['c'],c['c'])
                for variant in ['fixed_rr','fixed_ll']:
                    self.assertEqual(a['results_sha256'][variant],b['results_sha256'][variant])
                if c['c']==2:self.assertEqual(a['results_sha256'],b['results_sha256'])
        for filename in ['comparison.csv','summary.csv']:
            _,combined=coefficient.csv_rows(out/('coefficient_'+filename));native=[]
            for c in m['cases']:native.extend(coefficient.csv_rows(out/'cases'/c['id']/'batch'/filename)[1])
            self.assertEqual([{k:v for k,v in row.items() if k not in coefficient.EXTRA} for row in combined],native)
        self.assertEqual(snapshot(self.source),before);self.assertEqual(self.plan.read_bytes(),plan_before)

    def test_invalid_plans_rejected_before_output(self):
        self.source_fixture()
        mutations=[lambda p:p.update(c=[]),lambda p:p.update(c=[0,seeds.Decimal('0.0'),2]),
                   lambda p:p.update(c=[True,2]),lambda p:p.update(c=[-1,2]),
                   lambda p:p.update(c=['0.5',2]),lambda p:p.update(c=[0,1]),
                   lambda p:p.update(source_cases=['../escape']),lambda p:p.update(source_cases=['missing']),
                   lambda p:p.update(seeds=[501]),lambda p:p.update(kappa=[1.5,2])]
        for mutate in mutations:
            plan=copy.deepcopy(self.settings);mutate(plan);seeds.save(self.plan,plan)
            out=self.root/'bad';self.call(out,code=1);self.assertFalse(out.exists())

    def test_conflicts_and_nested_source_preserved(self):
        self.source_fixture();before=snapshot(self.source)
        out=self.root/'exists';out.mkdir();(out/'keep').write_text('old')
        self.call(out,code=1);self.assertEqual((out/'keep').read_text(),'old')
        self.call(self.source/'nested',code=1);self.assertEqual(snapshot(self.source),before)

    def test_corrupt_source_inputs_rejected(self):
        self.source_fixture();folder=self.source/'cases/healthy_j1_q0'
        m=seeds.load(folder/'series_manifest.json');(folder/m['entries'][0]['input_files']['delivery.csv']).write_text('corrupt')
        out=self.root/'rejected';self.call(out,code=1);self.assertFalse(out.exists())

    def test_incompatible_baseline_rejected(self):
        self.source_fixture();folder=self.source/'cases/healthy_j1_q0';path=folder/'series_manifest.json'
        m=seeds.load(path);m['entries'][0]['results_sha256']['adaptive_rr']['events.jsonl']='0'*64;seeds.save(path,m)
        before=snapshot(self.source);out=self.root/'incompatible';self.call(out,code=1)
        result=seeds.load(out/'coefficient_manifest.json')
        self.assertEqual(result['status'],'failed');self.assertIn('Baseline content differs',result['error'])
        self.assertFalse((out/'coefficient_comparison.csv').exists());self.assertEqual(snapshot(self.source),before)

    def test_fixed_invariance_guard_rejects_mismatch(self):
        self.source_fixture();folder=self.source/'cases/healthy_j1_q0';path=folder/'series_manifest.json'
        m=seeds.load(path);m['entries'][0]['results_sha256']['fixed_rr']['events.jsonl']='0'*64;seeds.save(path,m)
        before=snapshot(self.source);out=self.root/'fixed-mismatch';self.call(out,code=1)
        result=seeds.load(out/'coefficient_manifest.json')
        self.assertEqual(result['status'],'failed');self.assertIn('Fixed content differs',result['error'])
        self.assertFalse((out/'coefficient_comparison.csv').exists());self.assertEqual(snapshot(self.source),before)

    def test_native_overflow_is_not_a_successful_series(self):
        self.source_fixture()
        plan=copy.deepcopy(self.settings);plan['source_cases']=['healthy_j1_q0'];plan['c']=[2,seeds.Decimal('1e100')]
        seeds.save(self.plan,plan);out=self.root/'overflow';self.call(out,code=1)
        m=seeds.load(out/'coefficient_manifest.json')
        self.assertEqual(m['status'],'failed');self.assertNotEqual(m['commands'][-1]['returncode'],0)
        self.assertTrue((out/m['commands'][-1]['stderr']).is_file())
        self.assertFalse((out/'coefficient_summary.csv').exists())


if __name__=='__main__':unittest.main(argv=[sys.argv[0]],verbosity=2)
