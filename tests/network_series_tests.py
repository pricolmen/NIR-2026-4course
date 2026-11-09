"""Stage-2 grid/input-pairing tests against the real CLI; no result-metric code."""
import copy
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode=True
PROGRAM,SOURCE,BINARY=map(lambda p:Path(p).resolve(),sys.argv[1:4])
sys.path.insert(0,str(SOURCE/'tools'))
import run_network_series as network
import run_seed_series as seeds


def snapshot(root):return {str(p.relative_to(root)):seeds.sha(p) for p in root.rglob('*') if p.is_file()}


class NetworkSeriesTests(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory(prefix='network-series-',dir=BINARY)
        self.addCleanup(temporary.cleanup);self.root=Path(temporary.name)
        self.base=self.root/'base.json';shutil.copyfile(SOURCE/'examples/generator.json',self.base)
        self.plan=self.root/'plan.json'
        self.settings={'schema_version':1,'base_generator':'base.json','seeds':[302,301],
                       'delay_center_ticks':2,'jitter_half_width_ticks':[0,2],
                       'loss_probabilities':[0,0.2],'modes':[{'id':'healthy','crashes':[]},
                       {'id':'crash','crashes':[{'node_id':1,'at_ticks':5}]}]}
        seeds.save(self.plan,self.settings)

    def call(self,out,code=0):
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_network_series.py'),'--plan',str(self.plan),
                          '--program',str(PROGRAM),'--out',str(out)],cwd=self.root,capture_output=True,text=True,timeout=90)
        self.assertEqual(r.returncode,code,r.stdout+'\n'+r.stderr)
        return r

    def test_configs_change_only_explicit_network_and_crash_fields(self):
        before=self.base.read_bytes();plan,base_path,base,cases=network.prepare(self.plan)
        self.assertEqual(len(cases),8)
        self.assertEqual(len({c['id'] for c in cases}),8)
        self.assertEqual(before,self.base.read_bytes())
        for case in cases:
            expected=copy.deepcopy(base)
            expected['scenario']['crashes']=[] if case['mode']=='healthy' else [{'node_id':1,'at_ticks':5}]
            expected['delivery'].update(min_delay_ticks=case['min_delay_ticks'],max_delay_ticks=case['max_delay_ticks'],loss_probability=case['loss_probability'])
            self.assertEqual(case['config'],expected)
            self.assertEqual(case['min_delay_ticks']+case['max_delay_ticks'],4)

    def test_end_to_end_native_rows_pairing_and_preservation(self):
        original={self.plan:self.plan.read_bytes(),self.base:self.base.read_bytes()}
        out=self.root/"результаты ' $ `";self.call(out)
        manifest=seeds.load(out/'network_manifest.json')
        self.assertEqual(manifest['status'],'complete')
        self.assertEqual(len(manifest['cases']),8)
        self.assertEqual(manifest['checks']['input_realizations_checked'],16)
        self.assertEqual(manifest['checks']['variant_runs_checked'],64)
        self.assertEqual(manifest['comparison_rows'],64)
        self.assertEqual(manifest['summary_rows'],32)
        for name,value in manifest['checks'].items():
            if isinstance(value,bool):self.assertTrue(value,name)
        for filename in ['comparison.csv','summary.csv']:
            fields,combined=network.csv_rows(out/('network_'+filename))
            native=[]
            for case in manifest['cases']:
                _,rows=network.csv_rows(out/'cases'/case['id']/'batch'/filename)
                native.extend(rows)
            stripped=[{k:v for k,v in row.items() if k not in network.EXTRA} for row in combined]
            self.assertEqual(native,stripped)
        for p,data in original.items():self.assertEqual(p.read_bytes(),data)
        before=snapshot(out);self.call(out,code=1);self.assertEqual(snapshot(out),before)
        # Damaged saved input must stop validation before trusting any summary.
        plan,_,_,cases=network.prepare(self.plan)
        series=seeds.load(out/'cases'/cases[0]['id']/'series_manifest.json')
        path=out/'cases'/cases[0]['id']/series['entries'][0]['input_files']['delivery.csv']
        path.write_text('corrupt')
        with self.assertRaises(ValueError):network.verify_controls(out,plan,cases)

    def test_invalid_plan_is_rejected_before_output(self):
        mutations=[lambda p:p.update(seeds=[1,1]),lambda p:p.update(loss_probabilities=[0,2]),
                   lambda p:p.update(loss_probabilities=[0,False]),lambda p:p.update(jitter_half_width_ticks=[3]),
                   lambda p:p.update(jitter_half_width_ticks=[]),lambda p:p['modes'][0].update(id='../bad'),
                   lambda p:p.update(alpha=1)]
        for mutate in mutations:
            plan=copy.deepcopy(self.settings);mutate(plan);seeds.save(self.plan,plan)
            out=self.root/'bad';self.call(out,code=1);self.assertFalse(out.exists())

    def test_native_failure_is_not_a_successful_study(self):
        base=seeds.load(self.base);base['tasks']['work_min']=0;seeds.save(self.base,base)
        out=self.root/'failure';self.call(out,code=1)
        manifest=seeds.load(out/'network_manifest.json')
        self.assertEqual(manifest['status'],'failed')
        self.assertNotEqual(manifest['commands'][0]['returncode'],0)
        self.assertTrue((out/manifest['commands'][0]['stderr']).is_file())
        self.assertFalse((out/'network_summary.csv').exists())


if __name__=='__main__':unittest.main(argv=[sys.argv[0]],verbosity=2)
