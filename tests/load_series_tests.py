"""Four-node generation/pairing tests through the native CLI."""
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
import run_load_series as load


def snapshot(root):return {str(p.relative_to(root)):seeds.sha(p) for p in root.rglob('*') if p.is_file()}


class LoadSeriesTests(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory(prefix='load-series-',dir=BINARY);self.addCleanup(temp.cleanup)
        self.root=Path(temp.name);self.base=self.root/'base.json';self.plan=self.root/'plan.json'
        base=seeds.load(SOURCE/'examples/generator.json');base['scenario']['horizon_ticks']=80
        base['scenario']['measurement_windows']=[{'start_ticks':0,'end_ticks':80}];base['tasks'].update(count=8,period_ticks=6)
        base['scenario']['nodes']=[{'id':i,'performance_work_per_second':1000000} for i in range(1,5)]
        seeds.save(self.base,base)
        self.settings={'schema_version':1,'base_generator':'base.json','seeds':[702,701],
          'performance_profiles':[{'id':'equal','nodes':base['scenario']['nodes']},
            {'id':'heterogeneous','nodes':[{'id':i,'performance_work_per_second':p} for i,p in enumerate([500000,750000,1000000,1750000],1)]}],
          'loads':[{'id':'moderate','period_ticks':6},{'id':'high','period_ticks':3}],
          'networks':[{'id':'clean','min_delay_ticks':2,'max_delay_ticks':2,'loss_probability':0},
                      {'id':'noisy','min_delay_ticks':0,'max_delay_ticks':4,'loss_probability':seeds.Decimal('0.2')}],
          'modes':[{'id':'healthy','crashes':[]},{'id':'crash','crashes':[{'node_id':3,'at_ticks':35}]}]}
        seeds.save(self.plan,self.settings)

    def call(self,out,code=0):
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_load_series.py'),'--plan',str(self.plan),
                          '--program',str(PROGRAM),'--out',str(out)],cwd=self.root,capture_output=True,text=True,timeout=90)
        self.assertEqual(r.returncode,code,r.stdout+'\n'+r.stderr);return r

    def test_only_explicit_fields_change_and_equal_capacities(self):
        before=self.base.read_bytes();plan,_,base,cases=load.prepare(self.plan);self.assertEqual(len(cases),16)
        for case in cases:
            expected=copy.deepcopy(base)
            expected['scenario']['nodes']=next(p['nodes'] for p in plan['performance_profiles'] if p['id']==case['performance_profile'])
            expected['scenario']['crashes']=[] if case['mode']=='healthy' else [{'node_id':3,'at_ticks':35}]
            expected['tasks']['period_ticks']=case['arrival_period_ticks']
            expected['delivery']={k:v for k,v in next(n for n in plan['networks'] if n['id']==case['network_profile']).items() if k!='id'}
            self.assertEqual(case['config'],expected);self.assertEqual(case['total_performance_work_per_second'],4000000)
        self.assertEqual(self.base.read_bytes(),before)

    def test_native_rows_pairing_replay_and_preservation(self):
        original={p:p.read_bytes() for p in [self.plan,self.base]};out=self.root/"узлы ' $ `";self.call(out)
        m=seeds.load(out/'load_manifest.json');self.assertEqual(m['status'],'complete')
        self.assertEqual(len(m['cases']),16);self.assertEqual(m['checks']['variant_runs_checked'],128)
        self.assertEqual(m['checks']['generation_calls'],32);self.assertEqual(m['checks']['paired_trace_sets'],4)
        self.assertEqual(m['checks']['paired_work_sets'],2);self.assertEqual(m['comparison_rows'],128);self.assertEqual(m['summary_rows'],64)
        self.assertTrue(all(v for v in m['checks'].values() if isinstance(v,bool)))
        for filename in ['comparison.csv','summary.csv']:
            _,rows=load.csv_rows(out/('load_'+filename));native=[]
            for case in m['cases']:native.extend(load.csv_rows(out/'cases'/case['id']/'batch'/filename)[1])
            self.assertEqual([{k:v for k,v in r.items() if k not in load.EXTRA} for r in rows],native)
        saved=out/'cases'/m['cases'][-1]['id'];before=snapshot(out);replay=self.root/'replay'
        r=subprocess.run([sys.executable,str(SOURCE/'tools/run_seed_series.py'),'--reuse-series',str(saved),'--program',str(PROGRAM),'--out',str(replay)],capture_output=True,text=True,timeout=30)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        a=seeds.load(saved/'series_manifest.json');b=seeds.load(replay/'series_manifest.json')
        self.assertFalse(any(c['argv'][1]=='generate' for c in b['commands']))
        for x,y in zip(a['entries'],b['entries']):
            self.assertEqual(x['input_sha256'],y['input_sha256']);self.assertEqual(x['results_sha256'],y['results_sha256'])
        self.assertEqual(snapshot(out),before);self.call(out,code=1);self.assertEqual(snapshot(out),before)
        for p,data in original.items():self.assertEqual(p.read_bytes(),data)
        plan,_,_,cases=load.prepare(self.plan);folder=out/'cases'/cases[0]['id'];series=seeds.load(folder/'series_manifest.json')
        (folder/series['entries'][0]['input_files']['delivery.csv']).write_text('corrupt')
        with self.assertRaisesRegex(ValueError,'checksum'):load.verify_controls(out,plan,cases)

    def test_invalid_plans_before_output(self):
        def unequal_lost(p):
            p['performance_profiles'][1]['nodes'][1]['performance_work_per_second']=1250000
            p['performance_profiles'][1]['nodes'][2]['performance_work_per_second']=500000
        mutations=[lambda p:p.update(seeds=[1,1]),lambda p:p['loads'][0].update(period_ticks=0),
                   lambda p:p['loads'][0].update(period_ticks=100),lambda p:p['networks'][0].update(loss_probability=True),
                   lambda p:p['networks'][0].update(min_delay_ticks=5),lambda p:p['performance_profiles'][0].update(nodes=[]),
                   lambda p:p['performance_profiles'][1]['nodes'][0].update(performance_work_per_second=0),unequal_lost,
                   lambda p:p['modes'][1]['crashes'][0].update(node_id=9),lambda p:p['loads'][0].update(id='../escape'),
                   lambda p:p.update(alpha=1)]
        for mutate in mutations:
            plan=copy.deepcopy(self.settings);mutate(plan);seeds.save(self.plan,plan);out=self.root/'bad'
            self.call(out,code=1);self.assertFalse(out.exists())

    def test_native_failure_keeps_failed_logs(self):
        base=seeds.load(self.base);base['tasks']['work_min']=0;seeds.save(self.base,base)
        out=self.root/'failed';self.call(out,code=1);m=seeds.load(out/'load_manifest.json')
        self.assertEqual(m['status'],'failed');self.assertNotEqual(m['commands'][0]['returncode'],0)
        self.assertTrue((out/m['commands'][0]['stderr']).is_file());self.assertFalse((out/'load_summary.csv').exists())

    def drift_fixture(self):
        p=copy.deepcopy(self.settings);p['networks']=p['networks'][1:];p['performance_profiles']=p['performance_profiles'][:1];p['modes']=p['modes'][:1]
        seeds.save(self.plan,p);out=self.root/'drift';self.call(out)
        plan,_,_,cases=load.prepare(self.plan);folder=out/'cases'/cases[1]['id'];manifest=folder/'series_manifest.json';m=seeds.load(manifest)
        return out,plan,cases,folder,manifest,m

    def test_cross_load_work_drift_rejected(self):
        out,plan,cases,folder,manifest,m=self.drift_fixture();e=m['entries'][0];path=folder/e['input_files']['scenario.json'];s=seeds.load(path)
        s['tasks'][0]['work']=seeds.Decimal('3.141592653589793');seeds.save(path,s);e['input_sha256']['scenario.json']=seeds.sha(path);seeds.save(manifest,m)
        with self.assertRaisesRegex(ValueError,'Task work changed'):load.verify_controls(out,plan,cases)

    def test_cross_load_trace_drift_rejected(self):
        out,plan,cases,folder,manifest,m=self.drift_fixture();e=m['entries'][0];path=folder/e['input_files']['delivery.csv'];lines=path.read_text().splitlines()
        fields=lines[1].split(',');fields[2]=str((int(fields[2])+1)%5);lines[1]=','.join(fields);path.write_text('\n'.join(lines)+'\n')
        e['input_sha256']['delivery.csv']=seeds.sha(path);seeds.save(manifest,m)
        with self.assertRaisesRegex(ValueError,'Delivery changed'):load.verify_controls(out,plan,cases)

if __name__=='__main__':unittest.main(argv=[sys.argv[0]],verbosity=2)
