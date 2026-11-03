"""Refresh public festival dates from the Hong Kong Observatory; no player data."""
import argparse
import concurrent.futures
import datetime as dt
import json
from pathlib import Path
import re
import urllib.request


def fetch(year):
    base='https://www.hko.gov.hk/tc/gts/time/calendar/text/files/T'+str(year)+'c.txt'
    text=urllib.request.urlopen(base,timeout=30).read().decode('utf-8-sig')
    days=[]
    for line in text.splitlines():
        match=re.match(r'(\d{4})年(\d+)月(\d+)日\s+(\S+)\s+',line)
        if match:days.append((dt.date(*(int(match[i]) for i in (1,2,3))),match[4],line))
    expected=366 if dt.date(year,12,31).timetuple().tm_yday==366 else 365
    if len(days)!=expected:raise ValueError('incomplete HKO calendar '+str(year))
    starts={name:day for day,name,_ in days if name in ('正月','五月','八月')}
    if set(starts)!={'正月','五月','八月'}:raise ValueError('missing festival month '+str(year))
    qingming=[day for day,_,line in days if '清明' in line]
    if len(qingming)!=1:raise ValueError('missing Qingming '+str(year))
    dates={'new_year':dt.date(year,1,1),'spring_festival':starts['正月'],
           'qingming':qingming[0],'labour_day':dt.date(year,5,1),
           'dragon_boat':starts['五月']+dt.timedelta(days=4),
           'mid_autumn':starts['八月']+dt.timedelta(days=14),'national_day':dt.date(year,10,1)}
    return str(year),{key:value.isoformat() for key,value in dates.items()}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--first',type=int,default=2025);parser.add_argument('--last',type=int,default=2040)
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'owner/calendar_gifts/festival_dates.json')
    args=parser.parse_args()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:years=dict(pool.map(fetch,range(args.first,args.last+1)))
    value={'schema':1,'source':'Hong Kong Observatory Gregorian-Lunar calendar, UTC+08:00',
           'source_url':'https://www.hko.gov.hk/tc/gts/time/conversion.htm','years':dict(sorted(years.items()))}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('Public festival calendars generated:',len(years))


if __name__=='__main__':main()
