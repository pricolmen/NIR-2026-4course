"""Statistics and plot integration tests. Enabled explicitly with analysis dependencies."""
import copy
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import numpy as np

sys.dont_write_bytecode=True
PROGRAM,SOURCE,BINARY=map(lambda p:Path(p).resolve(),sys.argv[1:4])
sys.path.insert(0,str(SOURCE/'tools'))
import run_seed_series as io
from telemetry_stats.bootstrap import Bootstrap
from telemetry_stats.analysis import contrasts
from telemetry_stats.data import METRICS


def snapshot(root):return {str(p.relative_to(root)):io.sha(p) for p in root.rglob('*') if p.is_file()}


class BootstrapTests(unittest.TestCase):
    def test_exact_small_resampling_reference(self):
        boot=Bootstrap(5,4,.95);boot.indices[(11,22)]=np.array([[0,0],[0,1],[1,0],[1,1]])
        r=boot.summarize([(22,3),(11,1)],2)
        self.assertEqual(r['mean'],2);self.assertAlmostEqual(r['sd'],math.sqrt(2))
        self.assertAlmostEqual(r['ci_low'],1.075);self.assertAlmostEqual(r['ci_high'],2.925)

    def test_missing_singleton_and_constant_not_zero_imputation(self):
        boot=Bootstrap(2,100,.95)
        r=boot.summarize([(1,None),(2,None)],2);self.assertIsNone(r['mean']);self.assertIsNone(r['ci_low']);self.assertEqual(r['n_missing'],2)
        r=boot.summarize([(1,8),(2,None)],2);self.assertEqual(r['mean'],8);self.assertIsNone(r['sd']);self.assertIsNone(r['ci_low'])
        r=boot.summarize([(1,0),(2,0)],2);self.assertEqual(r['ci_reason'],'constant_sample');self.assertEqual(r['ci_low'],0)
        with self.assertRaises(ValueError):boot.summarize([(1,1),(1,2)],2)
        with self.assertRaises(ValueError):boot.summarize([(1,1)],2)
        with self.assertRaises(ValueError):boot.summarize([(1,math.inf)],1)

    def test_order_does_not_change_resampling(self):
        values=[(4,10),(2,5),(7,9),(1,30)]
        a=Bootstrap(42,1000,.95).summarize(values,4)
        b=Bootstrap(42,1000,.95).summarize(list(reversed(values)),4);self.assertEqual(a,b)

    def test_paired_difference_uses_intersection_of_defined_seeds(self):
        records=[]
        for variant in io.VARIANTS:
            values=[10,None,50] if variant=='adaptive_rr' else [7,20,None]
            for seed,value in enumerate(values,1):
                records.append({'study':'n','case':'c','variant':variant,'seed':seed,
                                'values':{k:value for k in METRICS},'scenario_sha256':'same','delivery_sha256':'trace',
                                'window_start_ticks':0,'window_end_ticks':10,'scenario':{}})
        cases={('n','c'):{'kind':'network','metadata':{},'expected':3}}
        rows,pairs=contrasts(records,cases,Bootstrap(4,100,.95))
        r=next(r for r in rows if r['left_variant']=='adaptive_rr' and r['metric']=='latency_ms')
        self.assertEqual(r['mean'],3);self.assertEqual(r['n_defined'],1)
        self.assertEqual((r['n_left_defined'],r['n_right_defined']),(2,2));self.assertIsNone(r['ci_low'])
        records[-1]['delivery_sha256']='other'
        with self.assertRaisesRegex(ValueError,'traces differ'):contrasts(records,cases,Bootstrap(4,100,.95))


class StatisticsIntegrationTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory(prefix='statistics-',dir=BINARY);self.addCleanup(temp.cleanup);self.root=Path(temp.name)
        self.plan=self.root/'analysis.json'
        self.settings={'schema_version':1,'bootstrap_seed':12,'resamples':100,'confidence_level':io.Decimal('.95'),
                       'window_index':0,'studies':[{'id':'network','kind':'network','directory':'primary'}]}
        io.save(self.plan,self.settings)

    def fixture(self):
        base=io.load(SOURCE/'examples/generator.json');base['scenario']['horizon_ticks']=80
        base['scenario']['measurement_windows']=[{'start_ticks':0,'end_ticks':80}];io.save(self.root/'base.json',base)
        plan={'schema_version':1,'base_generator':'base.json','seeds':[802,801],'delay_center_ticks':2,
              'jitter_half_width_ticks':[1],'loss_probabilities':[0],
              'modes':[{'id':'healthy','crashes':[]},{'id':'crash','crashes':[{'node_id':1,'at_ticks':35}]}]}
        io.save(self.root/'network.json',plan)
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_network_series.py'),'--plan',str(self.root/'network.json'),'--program',str(PROGRAM),'--out',str(self.root/'primary')],capture_output=True,text=True,timeout=40)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def call(self,out,code=0):
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_statistics.py'),'--plan',str(self.plan),
                          '--results-root',str(self.root),'--out',str(out)],capture_output=True,text=True,timeout=90)
        self.assertEqual(r.returncode,code,r.stdout+r.stderr);return r

    def test_native_import_figures_repeat_and_source_preservation(self):
        self.fixture();before=snapshot(self.root/'primary');out=self.root/"сводка ' $ `";self.call(out)
        m=io.load(out/'statistics_manifest.json');self.assertEqual(m['status'],'complete')
        self.assertEqual(m['native_commands_called'],0);self.assertEqual(m['counts']['variant_runs'],16)
        self.assertEqual(m['counts']['summary_rows'],96);self.assertEqual(m['counts']['figures'],2)
        self.assertEqual(snapshot(self.root/'primary'),before)
        self.call(self.root/'repeat');r=io.load(self.root/'repeat/statistics_manifest.json')
        self.assertEqual(m['artifacts_sha256'],r['artifacts_sha256'])
        # Saved figure data and both export formats must exist and render.
        from PIL import Image
        for name in io.load(out/'figures.json'):
            with Image.open(out/'figures'/(name+'.png')) as im:self.assertGreater(im.width,1000);self.assertGreater(im.height,500)
            self.assertIn('<svg',(out/'figures'/(name+'.svg')).read_text())
        saved=snapshot(out);self.call(out,code=1);self.assertEqual(snapshot(out),saved)

    def test_invalid_plan_and_duplicate_sources_before_output(self):
        self.fixture()
        mutations=[lambda p:p.update(resamples=0),lambda p:p.update(bootstrap_seed=True),
                   lambda p:p.update(window_index=-1),lambda p:p.update(confidence_level=1),
                   lambda p:p['studies'].append({'id':'again','kind':'network','directory':'primary'}),
                   lambda p:p['studies'][0].update(directory='../escape')]
        for mutate in mutations:
            p=copy.deepcopy(self.settings);mutate(p);io.save(self.plan,p)
            out=self.root/'bad';self.call(out,code=1);self.assertFalse(out.exists())

    def test_corrupted_native_values_fail_without_success_tables(self):
        self.fixture();folder=self.root/'primary/cases/healthy_j1_q0';series=io.load(folder/'series_manifest.json')
        path=folder/series['entries'][0]['batch_output']/'fixed_rr/metrics.json';path.write_text('{}')
        out=self.root/'corrupt';self.call(out,code=1);m=io.load(out/'statistics_manifest.json')
        self.assertEqual(m['status'],'failed');self.assertIn('checksum mismatch',m['error']);self.assertFalse((out/'summary.csv').exists())

if __name__=='__main__':unittest.main(argv=[sys.argv[0]],verbosity=2)
