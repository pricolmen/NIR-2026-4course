#!/usr/bin/env python3
"""Stage 8: read-only seed-level summaries, paired contrasts and scientific plots."""
import argparse
import csv
import importlib.metadata
import json
import platform
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode=True
import run_seed_series as io
from telemetry_stats.data import prepare,ingest,METRICS
from telemetry_stats.bootstrap import Bootstrap
from telemetry_stats.analysis import summaries,contrasts


def save_csv(path,rows):
    io.require(rows,'Refusing empty output table: '+str(path))
    with path.open('w',newline='',encoding='utf-8') as stream:
        writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)


def write_report(out,state,summary,paired):
    counts=state['counts'];confidence=f"{state['confidence_level']*100:g}%";ns='/'.join(map(str,sorted({r['n_expected'] for r in summary})));text=f'''# Статистическая сводка этапа 8

Основные серии этапов 2–7: {counts['studies']} серий, {counts['cases']} ячеек, {counts['variant_runs']} выполнений вариантов. Это связанные настройки с n={ns} seed на ячейку, а не общая независимая выборка. Технические replays, демонстрации этапа 1 и прежняя глава 4 не включались.

В summary.csv — {counts['summary_rows']} сводок «ячейка × вариант × показатель». В contrasts.csv — {counts['contrast_rows']} парных сводок. В paired_observations.csv сохранены исходные пары, включая пропуски; направление всегда left−right. Между разными сериями значения не объединяются. Для общих счётчиков выбрано одно окно, без суммирования повторяющихся totals между окнами.

Метод: {state['resamples']} повторных выборок целых seed с возвращением, {confidence} percentile bootstrap среднего; NumPy PCG64, bootstrap seed {state['bootstrap_seed']}. Для одной и той же упорядоченной группы seed используется один набор индексов, включая разные метрики. Для парных различий сначала вычитаются значения одинаковых seed, затем resampling выполняется по парам. Число задач/узлов не увеличивает n.

n_defined/n_missing и размеры определённых подвыборок сохранены. Detection/Recovery анализируются условно на определённые значения; это не оценка средней задержки всех физических отказов. У парных сравнений n_left_defined/n_right_defined может превышать число полных пар. Причины отсутствия значений сохранены в observations.csv.

При n=0 нет среднего/интервала; при n=1 нет SD/интервала. Constant sample даёт вырожденный эмпирический интервал; в частности, ноль FP на десяти seed не доказывает нулевой риск в генеральной совокупности. Интервалы pointwise, без поправки на множество сравнений; bootstrap при малом числе seed может плохо описывать хвосты. P-values, проверка универсального превосходства, одновременное покрытие и гарантированная точность номинального покрытия не заявляются.

Средняя latency и P95 сначала определены native-стендом внутри запуска, затем усреднены по seed. Средний P95 не равен объединённому P95 задач. При незавершённых задачах latency относится к завершённой части; FP/Late/FN и незавершённость необходимо рассматривать рядом с задержками.

Данные графиков сохранены в figure_data.csv, изображения — в figures/ в PNG и SVG. figures.json задаёт их список. Полные источники, хеши/версии и настройки — statistics_manifest.json/source_hashes.json/cases.json. Источники прочитаны без записи; generate/run/batch/metrics не вызывались. Формулы/входы/журналы стенда не менялись.
'''
    selected=[r for r in paired if r['study']=='load' and r['case']=='heterogeneous_moderate_clean_healthy' and r['relationship']=='algorithm' and r['left_variant']=='fixed_ll' and r['right_variant']=='fixed_rr' and r['metric']=='latency_ms']
    if selected:
        r=selected[0];text+=f"\nПример парного сравнения: heterogeneous/moderate/clean/healthy, fixed LL−RR, latency: {r['mean']:.3f} мс, эмпирический {confidence} интервал [{r['ci_low']:.3f}; {r['ci_high']:.3f}], n={r['n_defined']}. Это результат указанной настройки, а не универсальное преимущество LL.\n"
    text+='\nМетодическая основа: [SciPy bootstrap](https://docs.scipy.org/doc/scipy/reference/generated/scipy.stats.bootstrap.html), [NIST paired bootstrap](https://www.itl.nist.gov/div898/software/dataplot/refman1/auxillar/bootplot.htm). Метод намеренно зафиксирован как percentile, не BCa по умолчанию. Этап 9 не запускался.\n'
    (out/'report.md').write_text(text,encoding='utf-8')


def execute(args):
    root=args.results_root.expanduser().resolve();plan_path=args.plan.expanduser().resolve();plan,sources=prepare(plan_path,root)
    # Import optional analysis dependencies before creating output.
    import numpy
    import matplotlib
    requested=args.out.expanduser();io.require(not requested.exists() and not requested.is_symlink(),'Output exists; choose a new directory')
    out=requested.resolve();io.require(all(not out.is_relative_to(source) for study,source,m in sources),'Output must be outside primary sources')
    out.mkdir(parents=True);shutil.copyfile(plan_path,out/'plan.json')
    state={'schema_version':1,'status':'incomplete','bootstrap_seed':plan['bootstrap_seed'],'resamples':plan['resamples'],
           'confidence_level':float(plan['confidence_level']),'method':'seed-level percentile bootstrap; paired complete cases; pointwise',
           'window_index':plan['window_index'],'plan_sha256':io.sha(plan_path),'invocation':[sys.executable,*sys.argv],
           'results_root':str(root),'python':platform.python_version(),'platform':platform.platform(),
           'packages':{name:importlib.metadata.version(name) for name in ['numpy','matplotlib','pillow']},
           'source_manifests':{s['id']:str(p) for s,p,m in sources},'native_commands_called':0}
    manifest=out/'statistics_manifest.json';watched={plan_path:io.sha(plan_path)}
    for p in [Path(__file__).resolve(),Path(io.__file__).resolve(),*sorted((Path(__file__).parent/'telemetry_stats').glob('*.py'))]:watched[p.resolve()]=io.sha(p)
    state['analysis_source_sha256']={str(p):h for p,h in watched.items()};io.save(manifest,state)
    try:
        records,cases,source_hashes,checks=ingest(plan,sources);watched.update(source_hashes)
        bootstrap=Bootstrap(plan['bootstrap_seed'],plan['resamples'],float(plan['confidence_level']))
        summary=summaries(records,cases,bootstrap);paired,pair_records=contrasts(records,cases,bootstrap)
        observations=[]
        for r in sorted(records,key=lambda r:(r['study'],r['case'],r['variant'],r['seed'])):
            observations.append({**{k:v for k,v in r.items() if k not in ('values','reasons','scenario')},**r['values'],
                                 **{k+'_reason':r['reasons'].get(k,'') for k in METRICS}})
        save_csv(out/'observations.csv',observations);save_csv(out/'summary.csv',summary)
        save_csv(out/'contrasts.csv',paired);save_csv(out/'paired_observations.csv',pair_records)
        io.save(out/'cases.json',[{'study':s,'case':c,**v} for (s,c),v in sorted(cases.items())])
        from telemetry_stats.plots import create_figures
        figures,figure_data=create_figures(out,cases,summary,paired,float(plan['confidence_level']))
        save_csv(out/'figure_data.csv',figure_data);io.save(out/'figures.json',figures)
        for p,h in watched.items():io.require(io.sha(p)==h,'Source changed during analysis: '+str(p))
        io.save(out/'source_hashes.json',{str(p):h for p,h in sorted(source_hashes.items())})
        state['counts']={'studies':len(sources),'cases':len(cases),'variant_runs':len(records),'native_metric_checks':checks,
                         'summary_rows':len(summary),'contrast_rows':len(paired),'pair_observation_rows':len(pair_records),
                         'figures':len(figures),'source_files_verified':len(source_hashes),
                         'undefined_summary_groups':sum(r['n_defined']==0 for r in summary),
                         'partly_defined_summary_groups':sum(0<r['n_defined']<r['n_expected'] for r in summary)}
        write_report(out,state,summary,paired)
        artifacts=[p for p in out.rglob('*') if p.is_file() and p!=manifest and 'matplotlib-cache' not in p.parts]
        state['artifacts_sha256']={str(p.relative_to(out)):io.sha(p) for p in sorted(artifacts)}
        state['status']='complete';io.save(manifest,state)
    except BaseException as error:
        state['status']='failed';state['error']=str(error);io.save(manifest,state);raise
    print('Saved statistics:',out);print(state['counts']);return 0


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',type=Path,required=True);parser.add_argument('--results-root',type=Path,default=Path('results'))
    parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    try:return execute(args)
    except KeyboardInterrupt:print('Interrupted; partial analysis marked failed.',file=sys.stderr);return 130
    except (OSError,ValueError,KeyError,TypeError,ArithmeticError,ImportError) as error:
        print('Statistics failed: '+str(error),file=sys.stderr);return 1

if __name__=='__main__':sys.exit(main())
