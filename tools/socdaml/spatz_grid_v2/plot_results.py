#!/usr/bin/env python3
"""Render accepted machine-readable phase data; never use host runtime."""
from pathlib import Path
import argparse
import json


def plot(source, output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    data=json.loads(source.read_text())
    assert data['units']=='simulated ns' and data['clock_hz']==1_000_000_000
    keys=['hbm_input_schedule','sync_after_input','broadcast_schedule','sync_after_broadcast',
          'compute_and_required_sync','output_schedule','final_sync']
    labels=['HBM schedule','HBM completion join','Broadcast distribution','Distribution completion join',
            'Compute + required join','Output schedule','Final output join']
    colors=['#2563eb','#93c5fd','#0f766e','#5eead4','#64748b','#ea580c','#fdba74']
    phases=np.array([[data['variants'][v]['phase_ns'][k]/1000 for k in keys]
                     for v in ('direct','broadcast')])
    totals=np.array([data['variants'][v]['application_ns']/1000 for v in ('direct','broadcast')])
    assert np.allclose(phases.sum(axis=1),totals)
    plt.rcParams.update({'font.family':'DejaVu Sans','svg.fonttype':'none','font.size':10})
    fig=plt.figure(figsize=(11.2,6.4),facecolor='white')
    ax=fig.add_axes([.105,.57,.84,.31])
    left=np.zeros(2)
    for i,label in enumerate(labels):
        ax.barh([1,0],phases[:,i],left=left,height=.48,color=colors[i],label=label,linewidth=0)
        left+=phases[:,i]
    for y,total in zip([1,0],totals):
        ax.text(total+.65,y,f'{total:.3f} µs',va='center',weight='bold')
    ax.set_yticks([1,0],['Direct','Broadcast']);ax.set_xlim(0,73);ax.set_ylim(-.6,1.6)
    ax.set_xlabel('Simulated application time (µs)');ax.set_axisbelow(True)
    ax.xaxis.grid(True,color='#e2e8f0',linewidth=.7)
    for side in ('top','right','left'):ax.spines[side].set_visible(False)
    ax.spines['bottom'].set_color('#cbd5e1');ax.tick_params(axis='y',length=0)
    fig.suptitle('K=64 FP32 Spatz: complete 4×4-grid schedules',x=.105,y=.975,ha='left',weight='bold',fontsize=15)
    fig.text(.105,.925,'1 GHz · one accepted run per variant · validation excluded',color='#475569')
    fig.legend(*ax.get_legend_handles_labels(),loc='upper center',bbox_to_anchor=(.52,.515),
               ncol=3,frameon=False,fontsize=8.5)
    table_ax=fig.add_axes([.105,.085,.84,.29]);table_ax.axis('off')
    cells=[[label,f'{phases[0,i]:.3f}',f'{phases[1,i]:.3f}'] for i,label in enumerate(labels)]
    table=table_ax.table(cellText=cells,colLabels=['Adjacent phase','Direct µs','Broadcast µs'],
                         colWidths=[.64,.18,.18],loc='center',cellLoc='right')
    table.auto_set_font_size(False);table.set_fontsize(9);table.scale(1,1.22)
    for (row,col),cell in table.get_celld().items():
        cell.set_edgecolor('#e2e8f0');cell.set_linewidth(.4)
        if col==0:cell.get_text().set_ha('left')
        if row==0:cell.set_facecolor('#f1f5f9');cell.get_text().set_weight('bold')
    fig.text(.105,.015,'Direct output is serialized; broadcast output is parallel. Overall improvement covers both schedules.',
             color='#475569',fontsize=8.5)
    for suffix in ('.png','.svg'):
        fig.savefig(output.with_suffix(suffix),dpi=180,facecolor='white')
    plt.close(fig)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results',type=Path)
    parser.add_argument('--output',type=Path,required=True,help='Output stem for PNG and SVG.')
    args=parser.parse_args();plot(args.results,args.output)
