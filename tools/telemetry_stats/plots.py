"""Standalone scientific figures from seed-level summaries (Agg, PNG and SVG)."""
import os
from pathlib import Path
import re
import numpy as np
import run_seed_series as io

LABELS={'latency_ms':'Средняя задержка, мс','p95_ms':'Средний P95 запуска, мс',
        'detection_ms':'Обнаружение, мс','recovery_ms':'Recovery, мс',
        'fp':'Ложные исключения / запуск','late':'Crash после FP / запуск',
        'unfinished':'Не завершено задач / запуск','throughput':'Пропускная способность, задач/с'}
VARIANTS=['fixed_rr','fixed_ll','adaptive_rr','adaptive_ll']
COLORS=['#0072B2','#56B4E9','#D55E00','#CC79A7']
STYLES=['-','--','-','--'];MARKERS=['o','s','^','D']


def create_figures(out,cases,summary,contrasts,confidence=.95):
    os.environ.setdefault('MPLCONFIGDIR',str(out/'matplotlib-cache'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'svg.hashsalt':'telemetry-statistics-stage8',
                         'xtick.labelsize':9,'axes.spines.top':False,'axes.spines.right':False,'axes.titlepad':12})
    directory=out/'figures';directory.mkdir();figure_data=[];figures=[]
    lookup={(r['study'],r['case'],r['variant'],r['metric']):r for r in summary}
    def grid(rows,cols,title):
        fig,axes=plt.subplots(rows,cols,figsize=(15,3.6*rows+1.4),squeeze=False)
        fig.subplots_adjust(top=.88,bottom=.17,hspace=.55,wspace=.6);fig.suptitle(title,fontsize=15,y=.97)
        return fig,axes
    def draw(ax,study,points,metric,figure_id,panel):
        variants=VARIANTS
        if metric in ('fp','late','detection_ms'):
            variants=['fixed_rr','adaptive_rr']
            for x,case in points:
                for detector in ['fixed','adaptive']:
                    a=lookup[study,case,detector+'_rr',metric];b=lookup[study,case,detector+'_ll',metric]
                    io.require(all(a[k]==b[k] for k in ['mean','n_defined','ci_low','ci_high']),'RR/LL detector summaries differ')
        sample_notes=[];partial=False
        for variant in variants:
            index=VARIANTS.index(variant);xs=[];ys=[]
            for x,case in points:
                r=lookup[study,case,variant,metric];xs.append(x);ys.append(np.nan if r['mean'] is None else r['mean'])
                figure_data.append({'figure':figure_id,'panel':panel,'study':study,'case':case,'variant':variant,'metric':metric,
                                    'x':x,'mean':r['mean'],'ci_low':r['ci_low'],'ci_high':r['ci_high'],'n_defined':r['n_defined'],'n_expected':r['n_expected']})
                if r['ci_low'] is not None:ax.vlines(x,r['ci_low'],r['ci_high'],color=COLORS[index],linewidth=2,alpha=.7)
            counts=[lookup[study,c,variant,metric]['n_defined'] for x,c in points]
            partial|=any(lookup[study,c,variant,metric]['n_missing'] for x,c in points)
            sample_notes.append(variant.replace('fixed','F').replace('adaptive','A').replace('_','/')+': '+','.join(map(str,counts)))
            ax.plot(xs,ys,STYLES[index],marker=MARKERS[index],color=COLORS[index],markersize=4,linewidth=1.3)
        if partial:ax.text(1.025,1,'n по x →\n'+'\n'.join(sample_notes),transform=ax.transAxes,va='top',fontsize=6.5)
        ax.set_ylabel(LABELS[metric]);ax.grid(alpha=.25);ax.margins(x=.08,y=.16)
    def save(fig,figure_id,paired=False):
        handles=[Line2D([0],[0],color=COLORS[i],linestyle=STYLES[i],marker=MARKERS[i],label=VARIANTS[i].replace('_',' / ')) for i in range(4)]
        if paired:handles=[Line2D([0],[0],color=COLORS[i],linestyle=STYLES[i],marker=MARKERS[i],label=d+' LL−RR') for d,i in [('fixed',1),('adaptive',3)]]
        fig.legend(handles=handles,loc='lower center',bbox_to_anchor=(.5,.075),ncol=4,frameon=False)
        ns='/'.join(str(v) for v in sorted({info['expected'] for info in cases.values()}))
        caption=f'Точки — средние по seed; линии интервалов — {confidence*100:g}% pointwise percentile bootstrap. n = {ns}, если не подписано иначе.\n'
        caption+=('Δ = LL−RR: отрицательная разность задержек — в пользу LL. Это парные различия на одинаковых seed.' if paired else
                  'Detection/счётчики показаны по детектору (RR = LL). n=0 — пропуск, не нулевая задержка. Интервалы условны на определённые наблюдения.')
        caption+='\nВырожденный интервал постоянной выборки не доказывает отсутствия риска.'
        fig.text(.5,.025,caption,ha='center',va='bottom',fontsize=8)
        fig.canvas.draw()
        for ax in fig.axes:
            labels=[t for t in ax.get_xticklabels() if t.get_visible()]
            boxes=[t.get_window_extent(fig.canvas.get_renderer()) for t in labels]
            if any(a.x1+1>b.x0 for a,b in zip(boxes,boxes[1:])):
                plt.setp(labels,rotation=90,ha='center');fig.canvas.draw()
            boxes=[t.get_window_extent(fig.canvas.get_renderer()) for t in ax.get_xticklabels() if t.get_visible()]
            io.require(all(a.x1+1<=b.x0 for a,b in zip(boxes,boxes[1:])),'Overlapping x tick labels: '+figure_id)
        # Labels and annotations must fit the exported canvas.
        renderer=fig.canvas.get_renderer();width,height=fig.canvas.get_width_height()
        for label in [*fig.texts,*[t for ax in fig.axes for t in [ax.xaxis.label,ax.yaxis.label,ax.title,*ax.texts,*ax.get_xticklabels(),*ax.get_yticklabels()]]]:
            if not label.get_visible() or not label.get_text():continue
            b=label.get_window_extent(renderer)
            io.require(b.x0>=-2 and b.y0>=-2 and b.x1<=width+2 and b.y1<=height+2,'Clipped figure text: '+figure_id+' '+label.get_text())
        for ext in ['png','svg']:
            metadata={'Software':'telemetry statistics stage 8'} if ext=='png' else {'Date':None,'Creator':'telemetry statistics stage 8'}
            fig.savefig(directory/(figure_id+'.'+ext),dpi=180,metadata=metadata)
        figures.append(figure_id);plt.close(fig)
    studies=sorted({s for s,c in cases})
    for study in studies:
        entries=[(case,info) for (s,case),info in cases.items() if s==study];kind=entries[0][1]['kind']
        if kind=='network':
            for mode in sorted({v['metadata']['mode'] for c,v in entries}):
                widths=sorted({v['metadata']['jitter_half_width_ticks'] for c,v in entries if v['metadata']['mode']==mode})
                metrics=['fp','latency_ms','unfinished'] if mode=='healthy' else ['detection_ms','recovery_ms','unfinished']
                figure_id=study+'_'+mode;fig,axes=grid(len(widths),3,'Связь · '+mode)
                for i,width in enumerate(widths):
                    points=sorted((float(v['metadata']['loss_probability']),c) for c,v in entries if v['metadata']['mode']==mode and v['metadata']['jitter_half_width_ticks']==width)
                    for j,metric in enumerate(metrics):
                        draw(axes[i,j],study,points,metric,figure_id,f'j{width}_{metric}');axes[i,j].set_title(f'J={width/1000:g} мс');axes[i,j].set_xlabel('Вероятность потери q');axes[i,j].set_xticks([x for x,c in points])
                save(fig,figure_id)
        elif kind in ('timeout','kappa','coefficient','window'):
            field={'timeout':'tau0_ticks','kappa':'kappa','coefficient':'c','window':'window'}[kind]
            groups=sorted({(v['metadata']['mode'],v['metadata']['jitter_half_width_ticks'],v['metadata']['loss_probability']) for c,v in entries})
            for mode,width,q in groups:
                metrics=['fp','unfinished','latency_ms','p95_ms'] if mode=='healthy' else ['fp','late','detection_ms','recovery_ms','unfinished','latency_ms']
                figure_id=re.sub('[^a-z0-9_]','p',f'{study}_{mode}_j{width}_q{q}')
                fig,axes=grid(2,2 if mode=='healthy' else 3,f'{study} · {mode} · J={width/1000:g} мс, q={q:g}')
                points=sorted((float(v['metadata'][field])/(1000 if kind=='timeout' else 1),c) for c,v in entries if (v['metadata']['mode'],v['metadata']['jitter_half_width_ticks'],v['metadata']['loss_probability'])==(mode,width,q))
                for ax,metric in zip(axes.flat,metrics):
                    draw(ax,study,points,metric,figure_id,metric);ax.set_xlabel('T₀, мс' if kind=='timeout' else ('M' if kind=='window' else ('κ' if kind=='kappa' else 'c')));ax.set_xticks([x for x,c in points],[f'{x:g}' for x,c in points])
                save(fig,figure_id)
        elif kind=='load':
            for mode,network in sorted({(v['metadata']['mode'],v['metadata']['network_profile']) for c,v in entries}):
                profiles=sorted({v['metadata']['performance_profile'] for c,v in entries});figure_id=f'{study}_{network}_{mode}'
                metrics=['latency_ms','p95_ms','unfinished'] if mode=='healthy' else ['latency_ms','recovery_ms','unfinished']
                fig,axes=grid(len(profiles),3,f'Четыре узла · {network} · {mode}')
                for i,profile in enumerate(profiles):
                    points=sorted((1000000/v['metadata']['arrival_period_ticks'],c) for c,v in entries if v['metadata']['mode']==mode and v['metadata']['network_profile']==network and v['metadata']['performance_profile']==profile)
                    for j,metric in enumerate(metrics):
                        draw(axes[i,j],study,points,metric,figure_id,profile+'_'+metric);axes[i,j].set_title(profile);axes[i,j].set_xlabel('Поступление, задач/с');axes[i,j].set_xticks([x for x,c in points],[f'{x:.0f}' for x,c in points])
                save(fig,figure_id)
    # Direct paired differences are more informative than overlap of marginal CIs.
    for study in studies:
        entries=[(c,v) for (s,c),v in cases.items() if s==study]
        if entries[0][1]['kind']!='load':continue
        for mode in ['healthy','crash']:
            profiles=sorted({v['metadata']['performance_profile'] for c,v in entries});figure_id=study+'_paired_clean_'+mode
            if not any(v['metadata']['mode']==mode and v['metadata']['network_profile']=='clean' for c,v in entries):continue
            fig,axes=grid(len(profiles),2,'Парные различия LL−RR · clean · '+mode)
            for i,profile in enumerate(profiles):
                for j,metric in enumerate(['latency_ms','p95_ms']):
                    ax=axes[i,j];ax.axhline(0,color='#444444',linewidth=.8);ax.grid(alpha=.25);ax.set_title(profile);ax.set_ylabel('Δ '+LABELS[metric]);ax.set_xlabel('Поступление, задач/с')
                    for detector,index in [('fixed',1),('adaptive',3)]:
                        subset=[r for r in contrasts if r['study']==study and r['metric']==metric and r['relationship']=='algorithm' and r['left_variant']==detector+'_ll' and r['right_variant']==detector+'_rr' and cases[study,r['case']]['metadata']['network_profile']=='clean' and cases[study,r['case']]['metadata']['mode']==mode and cases[study,r['case']]['metadata']['performance_profile']==profile]
                        subset.sort(key=lambda r:1000000/cases[study,r['case']]['metadata']['arrival_period_ticks'])
                        xs=[];ys=[]
                        for r in subset:
                            x=1000000/cases[study,r['case']]['metadata']['arrival_period_ticks'];xs.append(x);ys.append(np.nan if r['mean'] is None else r['mean'])
                            if r['ci_low'] is not None:ax.vlines(x,r['ci_low'],r['ci_high'],color=COLORS[index],linewidth=2)
                            figure_data.append({'figure':figure_id,'panel':profile+'_'+metric,'study':study,'case':r['case'],'variant':detector+'_ll-minus-rr','metric':metric,'x':x,'mean':r['mean'],'ci_low':r['ci_low'],'ci_high':r['ci_high'],'n_defined':r['n_defined'],'n_expected':r['n_expected']})
                        ax.plot(xs,ys,STYLES[index],marker=MARKERS[index],color=COLORS[index]);ax.set_xticks(xs,[f'{x:.0f}' for x in xs])
            save(fig,figure_id,paired=True)
    return figures,figure_data
