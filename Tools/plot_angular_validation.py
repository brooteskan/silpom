"""Plot frozen engine visibility curves and separate ambient integration errors.

Requires NumPy and Matplotlib. No fitting or selection is performed here.
"""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    for name in ('editor','evaluation','output'):cli.add_argument(name,type=Path)
    a=cli.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    engine=json.loads((a.editor/'visual-validation.json').read_text())
    dense=json.loads((a.evaluation/'quality.json').read_text())['dense_ambient']
    fig,axes=plt.subplots(1,3,figsize=(12,3.7),sharey=True,layout='constrained')
    for ax,row in zip(axes,engine['moving']['receivers']):
        for key,label in (('old','Previous four caps'),('new','Angular-width four caps')):
            ax.plot(row['offsets_degrees'],row[key]['visibility'],label=label)
        ax.set(title='Wall patch '+str(row['center']),xlabel='Light yaw offset (degrees)',ylim=(-.02,1.02))
        ax.grid(alpha=.25)
    axes[0].set_ylabel('Diffuse visibility / matched unshadowed baseline')
    axes[-1].legend(fontsize=8)
    fig.suptitle('Engine receiver transitions — route/stability check, not teacher ground truth')
    for extension in ('png','svg'):fig.savefig(a.output/('receiver-transitions.'+extension),dpi=160)
    plt.close(fig)
    fig,axes=plt.subplots(1,2,figsize=(12,5.5),layout='constrained')
    for ax,state in zip(axes,('off','on')):
        image=plt.imread(a.editor/f'environment-{state}.png')
        ax.imshow(image[80:650,0:720]);ax.axis('off');ax.set_title('Diffuse accessibility '+state)
    fig.suptitle('Same wall, material and studio skylight — direct lights off')
    fig.savefig(a.output/'ambient-comparison.png',dpi=160);plt.close(fig)
    rows=dense['rows'];x=np.arange(len(rows));fig,ax=plt.subplots(figsize=(10,4),layout='constrained')
    for key,label,dx in (('teacher_32','Teacher at runtime 32 directions',-.24),
                        ('model_5_32','New model at runtime 32 directions',0),
                        ('model_5_8192','New model at 8192 directions',.24)):
        error=np.array([r[key]-r['teacher_8192'] for r in rows])*100
        ax.bar(x+dx,error,width=.24,label=label)
    ax.axhline(0,color='black',linewidth=.7)
    ax.set(xticks=x,xticklabels=[f'Receiver {i//2+1}\n{r["footprint"]:g}-texel footprint' for i,r in enumerate(rows)],
           ylabel='Signed error vs 8192-direction teacher (percentage points)',
           title='Selected receivers: angular quadrature and representation errors')
    ax.legend(fontsize=8);ax.grid(axis='y',alpha=.25)
    for extension in ('png','svg'):fig.savefig(a.output/('ambient-integration.'+extension),dpi=160)
    plt.close(fig)


if __name__=='__main__':main()
