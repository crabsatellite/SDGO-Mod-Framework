import {e,button,field} from '/panel_helpers.js';

export async function mountNews(root,ctx){
 const api=(path='',body)=>ctx.request('/api/announcements/news'+path,body,body?'POST':'GET');
 let data=await api();const card=e('section',undefined,'manager-card');root.append(card);
 card.append(e('h2','每天变化的资讯'),e('p','登录页显示简报，游戏内在大厅逐条展示。已读标题不会再次投放；超过有效期停止展示。客户端后台获取，不需要 Manager 常驻。','muted'));
 const options=e('div',undefined,'settings-grid');card.append(options);
 function bind(key,title,type='number'){
  const f=field(title,data.settings[key],type);f.input.oninput=()=>data.settings[key]=type==='checkbox'?f.input.checked:Number(f.input.value);options.append(f.box);return f.input;
 }
 bind('enabled','启用动态资讯','checkbox');bind('login_enabled','登录页显示新闻简报','checkbox');bind('game_enabled','游戏内显示资讯提示','checkbox');bind('game_lobby_only','仅在大厅显示资讯','checkbox');
 const categories=e('fieldset');for(const [key,title] of [['news','每日新闻'],['tech','科技资讯'],['epic','游戏限免'],['history','历史上的今天']]){
  const f=field(title,data.settings.categories.includes(key),'checkbox');f.input.onchange=()=>{data.settings.categories=f.input.checked?[...data.settings.categories,key]:data.settings.categories.filter(x=>x!==key);};categories.append(f.box);
 }card.append(categories);
 const schedule=e('div',undefined,'settings-grid');card.append(schedule);
 for(const [key,title] of [['start_time','资讯每天开始时间'],['end_time','资讯每天结束时间']]){const f=field(title,data.settings[key],'time');f.input.oninput=()=>data.settings[key]=f.input.value;schedule.append(f.box);}
 function choices(key,values){const box=e('fieldset');for(const [value,title] of values){const f=field(title,data.settings[key].includes(value),'checkbox');f.input.onchange=()=>{data.settings[key]=f.input.checked?[...data.settings[key],value]:data.settings[key].filter(x=>x!==value);};box.append(f.box);}card.append(box);}
 choices('weekdays',['一','二','三','四','五','六','日'].map((title,i)=>[i,'周'+title]));
 choices('game_triggers',[['login','登录成功'],['lobby','进入大厅'],['timer','定时轮播'],['room','进入房间'],['battle_start','战斗开始'],['battle_end','战斗结束']]);
 card.append(e('p','资讯沿用下方公告时区；相同开始和结束时间表示全天，支持跨午夜时段。勾选“仅在大厅”时，提示会等待回到大厅再展示。','muted'));
 for(const [key,title,min,max] of [['refresh_minutes','后台刷新间隔（分钟）',30,1440],['max_age_hours','最长缓存有效时间（小时）',1,72],
  ['login_news_count','每份简报的新闻条数',0,5],['login_tech_count','每份简报的科技资讯条数',0,5],['login_duration_seconds','登录简报展示秒数',5,120],
  ['game_max_day','大厅每日最多提示条数',1,20],['game_max_session','每次启动最多提示条数',1,10],['game_interval_seconds','大厅两条提示间隔（秒）',30,86400],
  ['game_duration_seconds','大厅每页展示秒数',3,60]]){const input=bind(key,title);input.min=min;input.max=max;}
 const source=e('details'),sourceSummary=e('summary','资讯来源（免费公共实例／自建接口）');source.append(sourceSummary);
 const label=e('label','每行一个来源根地址，最多三个，按顺序回退'),addresses=e('textarea');addresses.rows=3;addresses.style.width='100%';addresses.value=data.settings.sources.join('\n');label.append(addresses);source.append(label);
 source.append(e('p','来源使用兼容 60s API 的 HTTPS 接口。公共实例可能限流或不可用，正式运行也可使用自建实例。不会上传账号、机库或对局数据。','muted'));card.append(source);
 const status=e('p',undefined,'muted'),preview=e('div');card.append(status);
 function showStatus(){
  const names={news:'每日新闻',tech:'科技资讯',epic:'游戏限免',history:'历史上的今天'};
  const failed=data.failed_categories.length?'；部分内容暂未更新：'+data.failed_categories.map(k=>names[k]||'资讯').join('、'):'';
  status.textContent=`缓存中有 ${data.available_count} 条有效资讯。${data.last_attempt?'最近尝试：'+new Date(data.last_attempt*1000).toLocaleString('zh-CN',{hour12:false}):'尚未获取资讯'}${failed}。`;
  preview.replaceChildren();for(const item of data.items.slice(0,12)){
   const row=e('p'),link=e('a',item.title);if(String(item.url).startsWith('https://')){link.href=item.url;link.target='_blank';link.rel='noopener noreferrer';}
   row.append(link,e('small',' · '+item.date+' · '+item.source,'muted'));preview.append(row);
  }
 }
 const actions=e('div',undefined,'actions');card.append(actions);
 actions.append(button('保存资讯设置',async()=>{
  data.settings.sources=addresses.value.split(/\r?\n/).map(x=>x.trim().replace(/\/$/, '')).filter(Boolean);
  data=await api('/settings',{settings:data.settings,revision:data.revision});showStatus();ctx.message('动态资讯设置已保存，客户端自动载入。');
 },ctx,'primary'),button('立即获取并预览',async()=>{ctx.message('正在后台获取资讯…');data=await api('/refresh',{});showStatus();ctx.message('资讯缓存已更新。');},ctx));
 const services=e('p',undefined,'muted');card.append(services);
 actions.append(button('启动 Manager 定时更新',()=>globalService('start'),ctx),button('停止 Manager 定时更新',()=>globalService('stop'),ctx));
 // Service routes belong to the shared Manager API, outside the provider prefix.
 async function globalService(action){const response=await fetch('/api/services/announcements.news',{method:action?'POST':'GET',headers:action?{'Content-Type':'application/json','X-ModManager-Token':ctx.token}: {},body:action?JSON.stringify({action}):undefined});const value=await response.json();if(!response.ok)throw Error(value.error||'资讯服务操作未完成');services.textContent=(value.running?'Manager 定时更新正在运行':'Manager 定时更新已停止')+(value.error?'：'+value.error:'')+'。客户端也可自行后台获取。';}
 card.append(e('p','以下仅预览缓存内容；游戏会按已读记录、每日上限及大厅状态选择实际展示内容。新闻沿用来源日期，限免按活动起止时间判断。','muted'),preview);
 showStatus();await globalService();ctx.setInterval(async()=>{try{const fresh=await api();data={...fresh,settings:data.settings,revision:data.revision};showStatus();await globalService();}catch{}},10000);
}
