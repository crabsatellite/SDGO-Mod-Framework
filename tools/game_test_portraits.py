"""Retain requested authoring art cropped from genuine native model frames.

The full frames remain disposable test output. These derived PNGs are private
content inputs, not a claim that the whole gameplay run passed.
"""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import shutil
import uuid

from PIL import Image

ROOT=Path(__file__).resolve().parents[1]


def export_portraits(run: Path,destination: Path,unit: int) -> dict:
    run=run.resolve();destination=destination.resolve()
    allowed=(ROOT/'build/native-unit-portraits').resolve()
    if (not allowed.is_relative_to((ROOT/'build').resolve()) or
            not destination.is_relative_to(allowed) or destination.exists()):
        raise ValueError('Choose a new authoring directory below build/native-unit-portraits.')
    meta=json.loads((run/'run.json').read_text('utf-8'))
    report=json.loads((run/'receipts/report.json').read_text('utf-8'))
    cases=report.get('layers',{}).get('native',{}).get('cases',{})
    if (report.get('layers',{}).get('source',{}).get('status')!='bound' or
            any(cases.get(name,{}).get('status')!='pass' for name in ('framework.bootstrap','flow.login','setup.active_unit'))):
        raise ValueError('Native portrait needs bound runtime inputs and genuine login/owned-unit assertions.')
    visual=Path(meta['client']['root'])/'sdgo/gametest/visual'
    if not visual.resolve().is_relative_to(run):raise ValueError('Native portrait source escapes its owned run.')
    names=('pages.unit_index.pose_wait','pages.unit_index.pose_ready')
    for name in names:
        if cases.get(name,{}).get('status')!='pass':raise ValueError('Original ready-pose assertions have not passed.')
    destination.parent.mkdir(parents=True,exist_ok=True)
    staging=destination.parent/('.portrait-export-'+uuid.uuid4().hex);staging.mkdir()
    pictures=[]
    try:
        for name in names:
            evidence=json.loads((visual/(name+'.json')).read_text('utf-8'))
            if (evidence.get('unit_id')!=unit or evidence.get('renderer')!='original-unit-detail-backbuffer'
                    or evidence.get('animation') not in ('wait_01','choice_01') or not evidence.get('pose_handle')):
                raise ValueError('Native portrait does not describe the requested model and original animation.')
            model_hash=meta['client']['immutable_hashes'].get(evidence.get('model'))
            if not model_hash:raise ValueError('Native model was not sealed as a runtime input.')
            x,y,width,height=evidence['viewport']
            if any(type(v) is not int for v in (x,y,width,height)) or width<=0 or height<=0:
                raise ValueError('Native model viewport is invalid.')
            frames=sorted(visual.glob(name+'-*.bmp'))
            if len(frames)<(3 if name.endswith('pose_ready') else 2):raise ValueError('Native model frames are incomplete.')
            for frame in frames:
                if frame.is_symlink() or not frame.resolve().is_relative_to(visual):raise ValueError('Linked native frame refused.')
                raw=frame.read_bytes()
                with Image.open(frame) as original:
                    box=(max(0,x),max(0,y),min(original.width,x+width),min(original.height,y+height))
                    if box[2]<=box[0] or box[3]<=box[1]:raise ValueError('Native model viewport is outside the actual backbuffer.')
                    picture=original.convert('RGB').crop(box)
                target=staging/(frame.stem+'.png');picture.save(target,'PNG')
                pictures.append({'path':target.name,'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),
                    'source_frame_sha256':hashlib.sha256(raw).hexdigest(),'animation':evidence['animation'],
                    'model':evidence['model'],'viewport':list(box),'pixels':[picture.width,picture.height]})
                pictures[-1]['model_sha256']=model_hash
        if len({p['sha256'] for p in pictures})<2:raise ValueError('Native pose output contains no distinct views.')
        manifest={'schema':1,'kind':'native-model-authoring-art','unit_id':unit,'run_id':meta['run_id'],
            'source':meta.get('source'),'renderer':'original-unit-detail-backbuffer-crop',
            'host_sha256':meta['client']['immutable_hashes'].get('GOnline.exe'),
            'native_pose_cases':{name:cases[name]['status'] for name in names},
            'gameplay_run_status':report['status'],'pictures':pictures,
            'public_tool_default_content':False,'ai_generated':False}
        (staging/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        staging.rename(destination)
        return {'directory':str(destination),'unit_id':unit,'pictures':len(pictures),'renderer':manifest['renderer']}
    except BaseException:
        if staging.exists() and staging.resolve().parent==destination.parent:shutil.rmtree(staging)
        raise


def main(argv=None):
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--unit',type=int,required=True)
    args=parser.parse_args(argv)
    print(json.dumps(export_portraits(args.run_dir,args.output,args.unit),indent=2))


if __name__=='__main__':
    main()
