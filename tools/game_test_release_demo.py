"""Record bounded, silent native page demonstrations from a prepared GameTest.

Uses the existing private-desktop backbuffer recorder. A failed native run is
never published as a demonstration. Review and crop the resulting MKV before
exporting it; account names and deployment details must stay out of public media.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse,json,os,re,shutil,sys,time
if str(Path(__file__).resolve().parents[1])not in sys.path:sys.path.insert(0,str(Path(__file__).resolve().parents[1]))


def transition(event,run_id,first_case,last_case):
    if event.get('run_id')!=run_id:return None
    if event.get('event')=='case_begin'and event.get('case')==first_case:return 'start'
    if event.get('event')=='case_end'and event.get('case')==last_case:return 'stop'
    return None


def record(run_dir,output,first_case='pages.hangar.read',last_case='pages.hangar.cancel',name='hangar-manager',limit=20,timeout=600):
    from tools import game_test
    if not re.fullmatch('[a-z][a-z0-9-]{0,63}',name)or not 5<=limit<=45:raise ValueError('Use a simple scene name and a 5..45 second recording limit')
    metadata=game_test.validate_metadata(run_dir.resolve())
    if not metadata['plan'].get('quiet'):raise ValueError('Release recordings require a hidden, muted native run')
    ffmpeg=shutil.which('ffmpeg')
    if not ffmpeg:raise ValueError('ffmpeg is required')
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    trigger=output/'record.on';events=Path(metadata['client']['events'])
    settings={'SDGO_GAME_TEST_RECORD_DIR':str(output),'SDGO_GAME_TEST_RECORD_FFMPEG':ffmpeg,
              'SDGO_GAME_TEST_RECORD_FPS':'12','SDGO_GAME_TEST_REVIEW_HOLD_MS':'3000'}
    previous={key:os.environ.get(key)for key in settings};os.environ.update(settings)
    started=None;finished=False;seen=0
    try:
        with ThreadPoolExecutor(max_workers=1)as pool:
            native=pool.submit(game_test.run_prepared,run_dir,timeout)
            while not native.done():
                if events.exists():
                    lines=events.read_text(encoding='utf-8').splitlines()
                    for line in lines[seen:]:
                        try:event=json.loads(line)
                        except json.JSONDecodeError:break
                        seen+=1
                        action=transition(event,metadata['run_id'],first_case,last_case)
                        if action=='start'and started is None:
                            trigger.write_text(name+'\n',encoding='ascii');started=time.monotonic();print('Recording native scene: '+name,flush=True)
                        if action=='stop':
                            trigger.unlink(missing_ok=True);finished=True
                if started and time.monotonic()-started>=limit:trigger.unlink(missing_ok=True);finished=True
                time.sleep(.1)
            report=native.result();trigger.unlink(missing_ok=True)
        clips=list(output.glob(name+'_*.mkv'))
        result={'status':'ready_for_visual_review'if report['status']=='pass'and started and finished and clips else'failed',
                'native_status':report['status'],'run_id':metadata['run_id'],'first_case':first_case,'last_case':last_case,
                'fps_limit':12,'seconds_limit':limit,'clips':[str(p)for p in clips]}
        (output/'recording.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        if result['status']=='failed':raise RuntimeError(result)
        return result
    finally:
        trigger.unlink(missing_ok=True)
        for key,value in previous.items():
            if value is None:os.environ.pop(key,None)
            else:os.environ[key]=value


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__.splitlines()[0]);parser.add_argument('--run-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--first-case',default='pages.hangar.read')
    parser.add_argument('--last-case',default='pages.hangar.cancel');parser.add_argument('--name',default='hangar-manager')
    args=parser.parse_args();print(json.dumps(record(args.run_dir,args.output,args.first_case,args.last_case,args.name),indent=2))
