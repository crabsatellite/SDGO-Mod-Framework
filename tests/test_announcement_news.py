import copy
import datetime as dt
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from manager import announcement_news as news

NOW=int(dt.datetime(2026,11,4,12,tzinfo=dt.timezone.utc).timestamp())
DAY='2026-11-04'

def daily(day=DAY):return {'code':200,'data':{'date':day,'news':['测试新闻一','科技 &amp; 游戏 <b>快报</b>','测试新闻一'],'link':'https://example.com/news'}}


class NewsTests(unittest.TestCase):
    def test_defaults_are_keyless_public_sources_and_match_native_ini(self):
        settings=copy.deepcopy(news.DEFAULT);news.validate(settings)
        text=news.compile_settings(settings).decode('utf-16')
        self.assertIn('max_age_hours=48',text)
        self.assertNotIn('60s.viki.moe',text)
        for source in settings['sources']:self.assertTrue(source.startswith('https://'))

    def test_rejects_credentials_insecure_hosts_bad_limits_and_duplicate_types(self):
        for key,value in [('sources',['http://example.com']),('sources',['http://127.attacker.test']),
            ('sources',['https://name:password@example.com']),('sources',['https://example.com/?apikey=value']),
            ('categories',['news','news']),('max_age_hours',500),('refresh_minutes',True),('game_max_day',0)]:
            with self.subTest(key=key,value=value):
                settings=copy.deepcopy(news.DEFAULT);settings[key]=value
                with self.assertRaises(ValueError):news.validate(settings)
        settings=copy.deepcopy(news.DEFAULT);settings['sources']=['http://127.0.0.1:4444'];news.validate(settings)

    def test_date_attribution_unicode_escaping_deduplication_and_staleness(self):
        items=news.normalized('news',daily(),NOW,48*3600)
        self.assertEqual(len(items),2)
        self.assertEqual(items[0]['date'],DAY)
        self.assertEqual(items[1]['title'],'科技 & 游戏 快报')
        self.assertEqual(new_hash:=news.identity('news','测试新闻一','https://example.com/news'),items[0]['id'])
        self.assertTrue(new_hash.startswith('dailynews_news_'))
        self.assertEqual(news.normalized('news',daily('2026-11-01'),NOW,48*3600),[])
        self.assertEqual(news.normalized('news',daily('2026-11-06'),NOW,48*3600),[])

    def test_tech_validates_real_publication_time_and_article_links(self):
        payload={'code':200,'data':[
            {'title':'有效消息','link':'https://example.com/tech','created_at':(NOW-60)*1000},
            {'title':'未来消息','link':'https://example.com/future','created_at':(NOW+600)*1000},
            {'title':'旧消息','link':'https://example.com/old','created_at':(NOW-3*86400)*1000},
            {'title':'脚本链接','link':'javascript:alert(1)','created_at':NOW*1000}]}
        result=news.normalized('tech',payload,NOW,48*3600)
        self.assertEqual([x['title'] for x in result],['有效消息'])

    def test_epic_requires_a_live_interval_and_excludes_permanent_free_games(self):
        def item(title,start,end,price):return {'title':title,'link':'https://store.epicgames.com/'+title,
            'original_price':price,'is_free_now':True,'free_start_at':start*1000,'free_end_at':end*1000}
        payload={'code':200,'data':[item('live',NOW-6*86400,NOW+86400,100),
            item('future',NOW+86400,NOW+2*86400,100),item('expired',NOW-86400,NOW,100),
            item('permanent',NOW-86400,NOW+86400,0)]}
        result=news.normalized('epic',payload,NOW,48*3600)
        self.assertEqual([x['title'] for x in result],['live'])
        self.assertEqual(result[0]['expires_at'],NOW+86400)

    def test_history_requires_the_correct_date_and_prefers_science(self):
        payload={'code':200,'data':{'month':11,'day':4,'items':[
            {'title':'普通历史','year':'1900','link':'https://example.com/history'},
            {'title':'科学发现','year':'1950','link':'https://example.com/science'}]}}
        result=news.normalized('history',payload,NOW,48*3600)
        self.assertEqual(result[0]['title'],'1950年：科学发现')
        payload['data']['day']=3
        with self.assertRaises(ValueError):news.normalized('history',payload,NOW,48*3600)

    def test_fallback_failure_cache_expiry_and_promotion_removal(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp)/'sdgo/mods/announcements';directory.mkdir(parents=True)
            settings=copy.deepcopy(news.DEFAULT);settings['categories']=['news'];calls=[]
            def fetch(address):
                calls.append(address)
                if address.startswith(settings['sources'][0]):raise TimeoutError()
                return daily()
            first=news.refresh(directory,settings,now=NOW,fetcher=fetch)
            self.assertEqual(first['available_count'],2);self.assertEqual(len(calls),2)
            def failed(address):raise ValueError('untrusted server error')
            later=news.refresh(directory,settings,now=NOW+3600,fetcher=failed)
            self.assertEqual(later['available_count'],2);self.assertEqual(later['failed_categories'],['news'])
            self.assertEqual(news.public(directory,settings,now=NOW+3*86400)['available_count'],0)
            # Exclusive refresh leases are shared with the native runtime.
            with news.lease(news.cache_directory(directory)/'.refresh.lock'):
                with self.assertRaises(ValueError):news.refresh(directory,settings,now=NOW,fetcher=fetch)

    def test_no_source_markup_or_control_characters_become_executable_content(self):
        self.assertEqual(news.clean('<script>alert(1)</script>标题'),'alert(1) 标题')
        for value in ['\0','\ud800',123]:
            with self.assertRaises(ValueError):news.clean(value)
        for raw in [b'{"code":200,"code":0}',b'{"number":NaN}',b'['*2000+b']'*2000,b'"\xff"']:
            with self.assertRaises(ValueError):news.parse_payload(raw)


if __name__=='__main__':unittest.main()
