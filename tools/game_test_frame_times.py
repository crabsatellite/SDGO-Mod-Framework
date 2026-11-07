"""Read actual quiet-lane Present intervals, bound to native battle events."""
from pathlib import Path
import argparse,json,math,struct

def percentile(values,fraction):
    ordered=sorted(values);position=(len(ordered)-1)*fraction
    low=math.floor(position);high=math.ceil(position)
    return ordered[low]+(ordered[high]-ordered[low])*(position-low)

def read_frames(path,expected_pid):
    raw=Path(path).read_bytes()
    if len(raw)<24 or raw[:8]!=b'SDGOPRS1':raise ValueError('Invalid Present timing header')
    pid,stride,frequency=struct.unpack_from('<IIQ',raw,8)
    if pid!=expected_pid or stride!=32 or frequency<=0 or (len(raw)-24)%stride:
        raise ValueError('Present identity, frequency or complete record length is invalid')
    rows=list(struct.iter_unpack('<4Q',raw[24:]))
    for index,row in enumerate(rows):
        sequence,uptime,qpc,gap=row
        if sequence!=index+1 or not uptime or not qpc or not gap:raise ValueError('Missing or invalid Present records')
        if index and (uptime<rows[index-1][1] or qpc<=rows[index-1][2] or gap!=qpc-rows[index-1][2]):
            raise ValueError('Present sequence or QPC interval is inconsistent')
    return frequency,rows

def evaluate(path,events_path,run_id,pid):
    events=[json.loads(line) for line in Path(events_path).read_text(encoding='utf-8-sig').splitlines()]
    own=[e for e in events if e.get('run_id')==run_id and e.get('pid')==pid]
    starts=[e['uptime_ms'] for e in own if e.get('event')=='case_begin' and e.get('case')=='pvp.ai_spawn']
    ends=[e['uptime_ms'] for e in own if e.get('event')=='observation' and e.get('case')=='pvp.return' and e.get('message')=='observed normal client state=18']
    if len(starts)!=1 or len(ends)!=1 or ends[0]<=starts[0]:raise ValueError('An exact whole-battle start/end pair is required')
    frequency,frames=read_frames(path,pid)
    # Reject intervals straddling the opening boundary; keep every complete
    # interval within battle, including genuine stalls and long frame gaps.
    selected=[r for r in frames if r[1]<=ends[0] and r[1]-r[3]*1000/frequency>=starts[0]]
    has_after_end=any(r[1]>=ends[0] for r in frames)
    if len(selected)<30 or (not has_after_end and ends[0]-selected[-1][1]>1000):
        raise ValueError('Incomplete battle frame coverage')
    values=[r[3]*1000/frequency for r in selected]
    duration=(ends[0]-starts[0])/1000
    return {'schema_version':1,'run_id':run_id,'pid':pid,'status':'complete','source':'successful native D3D9 Present invocations',
            'qpc_frequency':frequency,'battle_seconds':duration,'interval_count':len(values),
            'frame_ms':{'p50':percentile(values,.5),'p95':percentile(values,.95),'p99':percentile(values,.99),'maximum':max(values),'mean':sum(values)/len(values)},
            'stalls':{str(threshold)+'ms':{'count':sum(v>threshold for v in values),'per_minute':sum(v>threshold for v in values)*60/duration}
                      for threshold in [33.333,50,100,250,1000]},
            'last_present_before_end_ms':ends[0]-selected[-1][1],
            'end_covered_by_later_present':has_after_end,
            'boundary_note':'Completed Present intervals are used for percentiles. Any no-Present tail at the native end boundary is separately reported and is not counted as a zero-duration frame.'}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--run-dir',type=Path,required=True);args=parser.parse_args()
    meta=json.loads((args.run_dir/'run.json').read_text(encoding='utf-8-sig'))
    folder=args.run_dir/'client/sdgo/gametest'
    result=evaluate(folder/'present-timings.bin',folder/'events.jsonl',meta['run_id'],meta['execution']['game_pid'])
    (folder/'frame-time-report.json').write_bytes((json.dumps(result,indent=2)+'\n').encode())
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
