"""Run summaries and explicit paired contrasts; never pool seeds across studies."""
from collections import defaultdict
import run_seed_series as io
from .data import METRICS

WITHIN=[('adaptive_rr','fixed_rr'),('adaptive_ll','fixed_ll'),('fixed_ll','fixed_rr'),('adaptive_ll','adaptive_rr')]
BASELINE={'timeout':('tau0_ticks',20000),'kappa':('kappa',2),
          'coefficient':('c',2),'window':('window',3)}


def grouped(records):
    result=defaultdict(dict)
    for r in records:
        key=(r['study'],r['case'],r['variant'])
        io.require(r['seed'] not in result[key],'Duplicate seed')
        result[key][r['seed']]=r
    return result


def summaries(records,cases,bootstrap):
    groups=grouped(records);rows=[]
    for (study,case,variant),values in sorted(groups.items()):
        expected=cases[study,case]['expected']
        bounds={(r['window_start_ticks'],r['window_end_ticks']) for r in values.values()}
        io.require(len(bounds)==1,'Measurement windows differ within group')
        for metric,(native,unit) in METRICS.items():
            estimate=bootstrap.summarize([(seed,r['values'][metric]) for seed,r in values.items()],expected)
            rows.append({'study':study,'case':case,'variant':variant,'metric':metric,'unit':unit,**estimate})
    return rows


def check_pair(left,right,relationship):
    io.require(set(left)==set(right),'Unmatched seed records in paired contrast')
    for seed in left:
        a=left[seed];b=right[seed]
        io.require(a['delivery_sha256']==b['delivery_sha256'],'Paired traces differ')
        io.require((a['window_start_ticks'],a['window_end_ticks'])==(b['window_start_ticks'],b['window_end_ticks']),'Paired windows differ')
        if relationship=='algorithm':io.require(a['scenario_sha256']==b['scenario_sha256'],'Algorithm inputs differ')
        else:
            x=a['scenario'];y=b['scenario']
            if relationship=='tau0_ticks':x={k:v for k,v in x.items() if k!='tau0_ticks'};y={k:v for k,v in y.items() if k!='tau0_ticks'}
            elif relationship=='kappa':x={k:v for k,v in x.items() if k!='kappa'};y={k:v for k,v in y.items() if k!='kappa'}
            elif relationship in ('c','window'):
                x=dict(x,adaptive={k:v for k,v in x['adaptive'].items() if k!=relationship})
                y=dict(y,adaptive={k:v for k,v in y['adaptive'].items() if k!=relationship})
            elif relationship=='performance':x={k:v for k,v in x.items() if k!='nodes'};y={k:v for k,v in y.items() if k!='nodes'}
            elif relationship=='arrival_period':
                x=dict(x,tasks=[{k:v for k,v in t.items() if k!='arrival_ticks'} for t in x['tasks']])
                y=dict(y,tasks=[{k:v for k,v in t.items() if k!='arrival_ticks'} for t in y['tasks']])
            else:raise ValueError('Unknown paired relationship')
            io.require(x==y,'Non-study inputs differ in paired contrast')


def contrast_specs(cases):
    specs=[]
    for (study,case),info in sorted(cases.items()):
        for left,right in WITHIN:specs.append((study,case,left,case,right,'algorithm'))
        kind=info['kind'];meta=info['metadata']
        if kind in BASELINE:
            field,baseline=BASELINE[kind]
            # Reference levels are predeclared original settings, not selected from outcomes.
            if meta[field]!=baseline:
                matches=[c for (s,c),other in cases.items() if s==study and other['metadata'].get('source_case')==meta['source_case'] and other['metadata'].get(field)==baseline]
                io.require(len(matches)==1,'Unique reference parameter cell missing')
                for variant in io.VARIANTS:specs.append((study,case,variant,matches[0],variant,field))
        if kind=='load':
            for relationship,field,baseline in [('performance','performance_profile','equal'),('arrival_period','load_id','moderate')]:
                if meta[field]==baseline:continue
                fields=['mode','network_profile','load_id'] if relationship=='performance' else ['mode','network_profile','performance_profile']
                matches=[c for (s,c),other in cases.items() if s==study and other['metadata'].get(field)==baseline and all(other['metadata'].get(k)==meta[k] for k in fields)]
                io.require(len(matches)==1,'Unique reference load/performance cell missing')
                for variant in io.VARIANTS:specs.append((study,case,variant,matches[0],variant,relationship))
    return specs


def contrasts(records,cases,bootstrap):
    groups=grouped(records);rows=[];pairs=[]
    for study,case,left_variant,reference,right_variant,relationship in contrast_specs(cases):
        left=groups[study,case,left_variant];right=groups[study,reference,right_variant];check_pair(left,right,relationship)
        expected=cases[study,case]['expected']
        base={'study':study,'case':case,'left_variant':left_variant,'reference_case':reference,
              'right_variant':right_variant,'relationship':relationship,'direction':'left_minus_right'}
        for metric,(native,unit) in METRICS.items():
            observations=[];n_left=0;n_right=0
            for seed in sorted(left):
                a=left[seed]['values'][metric];b=right[seed]['values'][metric]
                n_left+=a is not None;n_right+=b is not None
                value=None if a is None or b is None else a-b
                observations.append((seed,value))
                pairs.append({**base,'metric':metric,'unit':unit,'seed':seed,'left_value':a,'right_value':b,'difference':value,
                              'pair_reason':'' if value is not None else 'undefined_left_or_right'})
            rows.append({**base,'metric':metric,'unit':unit,'n_left_defined':n_left,'n_right_defined':n_right,
                         **bootstrap.summarize(observations,expected)})
    return rows,pairs
