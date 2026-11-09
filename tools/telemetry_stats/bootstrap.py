"""Seed-level percentile bootstrap; shared resamples preserve paired differences."""
import hashlib
import math
import numpy as np


class Bootstrap:
    def __init__(self,seed,resamples,confidence):
        self.seed=seed;self.resamples=resamples;self.confidence=confidence
        self.indices={};self.cache={}

    def summarize(self,observations,expected):
        """observations contains (seed, optional value); null never becomes zero."""
        ordered=sorted(observations)
        if len({s for s,v in ordered})!=len(ordered):raise ValueError('Duplicate seed in statistical group')
        if len(ordered)!=expected:raise ValueError('Missing seed record; different from undefined metric')
        defined=[(s,float(v)) for s,v in ordered if v is not None]
        if any(not math.isfinite(v) for s,v in defined):raise ValueError('Non-finite statistical observation')
        seeds=tuple(s for s,v in defined);values=tuple(v for s,v in defined);n=len(values)
        key=(seeds,values)
        if key not in self.cache:
            a=np.asarray(values,dtype=np.float64)
            result={'n_defined':n,'mean':None,'sd':None,'minimum':None,'maximum':None,
                    'ci_low':None,'ci_high':None,'ci_reason':'no_defined_runs'}
            if n:
                result.update(mean=float(a.mean()),minimum=min(values),maximum=max(values),ci_reason='one_defined_run')
            if n>=2:
                result['sd']=float(a.std(ddof=1))
                if all(v==values[0] for v in values):
                    result.update(ci_low=values[0],ci_high=values[0],ci_reason='constant_sample')
                else:
                    if seeds not in self.indices:
                        digest=hashlib.sha256((str(self.seed)+':'+','.join(map(str,seeds))).encode()).digest()
                        rng=np.random.Generator(np.random.PCG64(int.from_bytes(digest[:16],'big')))
                        self.indices[seeds]=rng.integers(0,n,size=(self.resamples,n))
                    estimates=a[self.indices[seeds]].mean(axis=1)
                    tail=(1-self.confidence)/2
                    low,high=np.quantile(estimates,[tail,1-tail],method='linear')
                    result.update(ci_low=float(low),ci_high=float(high),ci_reason='percentile_bootstrap')
            self.cache[key]=result
        return {**self.cache[key],'n_expected':expected,'n_missing':expected-n}
