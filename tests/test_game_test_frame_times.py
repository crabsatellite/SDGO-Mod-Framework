import json,struct,tempfile,unittest
from pathlib import Path
from tools.game_test_frame_times import evaluate,read_frames

class PresentEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.frames=self.root/'frames.bin';self.events=self.root/'events.jsonl'
    def write(self,gaps):
        time=0;rows=[]
        for i,gap in enumerate(gaps):
            time+=gap;rows.append((i+1,time,time,gap))
        self.frames.write_bytes(b'SDGOPRS1'+struct.pack('<IIQ',12,32,1000)+b''.join(struct.pack('<4Q',*r) for r in rows))
        events=[{'run_id':'owned','pid':12,'event':'case_begin','case':'pvp.ai_spawn','uptime_ms':1},
                {'run_id':'owned','pid':12,'event':'observation','case':'pvp.return','message':'observed normal client state=18','uptime_ms':time}]
        self.events.write_text('\n'.join(json.dumps(e) for e in events))
    def test_real_stalls_remain_in_percentiles(self):
        self.write([16]*100+[2000]+[16]*100)
        result=evaluate(self.frames,self.events,'owned',12)
        self.assertEqual(result['frame_ms']['maximum'],2000)
        self.assertEqual(result['stalls']['1000ms']['count'],1)
        self.assertEqual(result['frame_ms']['p50'],16)
    def test_wrong_process_is_rejected(self):
        self.write([16]*100)
        with self.assertRaises(ValueError):read_frames(self.frames,13)
    def test_dropped_or_partial_frames_are_rejected(self):
        self.write([16]*100);raw=self.frames.read_bytes()
        self.frames.write_bytes(raw[:-1])
        with self.assertRaises(ValueError):read_frames(self.frames,12)
        self.frames.write_bytes(raw[:24]+raw[56:])
        with self.assertRaises(ValueError):read_frames(self.frames,12)
    def test_lobby_or_incomplete_match_is_not_a_whole_battle(self):
        self.write([16]*100);self.events.write_text('')
        with self.assertRaises(ValueError):evaluate(self.frames,self.events,'owned',12)
    def test_other_run_end_cannot_complete_this_battle(self):
        self.write([16]*100)
        events=[json.loads(v) for v in self.events.read_text().splitlines()];events[1]['run_id']='foreign'
        self.events.write_text('\n'.join(json.dumps(e) for e in events))
        with self.assertRaises(ValueError):evaluate(self.frames,self.events,'owned',12)
    def test_native_end_inside_real_present_stall_keeps_censored_tail(self):
        self.write([16]*100+[3000]+[16]*100)
        events=[json.loads(v) for v in self.events.read_text().splitlines()]
        events[1]['uptime_ms']=3500
        self.events.write_text('\n'.join(json.dumps(e) for e in events))
        result=evaluate(self.frames,self.events,'owned',12)
        self.assertEqual(result['last_present_before_end_ms'],1900)
        self.assertTrue(result['end_covered_by_later_present'])

if __name__=='__main__':unittest.main()
