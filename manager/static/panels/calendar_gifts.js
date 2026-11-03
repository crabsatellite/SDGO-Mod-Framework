// Uses the existing Manager origin, session, revisions and service supervisor.
export async function mount(root,ctx){
 const el=(tag,text,cls)=>{const x=document.createElement(tag);if(text!==undefined)x.textContent=text;if(cls)x.className=cls;return x;};
 const act=(text,fn)=>{const b=el('button',text,'ui-btn');b.type='button';b.onclick=()=>Promise.resolve().then(fn).catch(e=>ctx.message(e.message,true));return b;};
 const input=(type,value)=>{const x=el('input');x.type=type;if(type==='checkbox')x.checked=value;else x.value=value??'';return x;};
 const select=(options,value)=>{const s=el('select');for(const [key,title] of options)s.add(new Option(title,key));s.value=value;return s;};
 const field=(title,node)=>{const label=el('label',title);label.append(node);return label;};
 const section=title=>{const s=el('section',undefined,'manager-card');s.append(el('h2',title));root.append(s);return s;};
 const api=(path,body)=>ctx.request('/api/calendar-gifts'+path,body,body===undefined?'GET':'POST');
 let state=await api(''),labels={players:{},units:{},items:{}};
 try{labels=await api('/labels');}catch{}
 const groupChoices=()=>[['all','全部现有玩家'],...Object.keys(state.private.groups).map(g=>[g,g])];
 const unitModes=[['missing_one','一台未拥有机体'],['missing_each_grade','每个等级一台未拥有机体'],['fixed','指定未拥有机体'],['none','仅送道具']];
 root.append(el('p','默认关闭。节日送一台未拥有机体；生日按 12 个等级各送一台。赠礼进入原生保管库，玩家可在有空机位时领取。','intro'));
 const status=section('本服运行'),statusText=el('p'),enabled=input('checkbox',state.policy.enabled);
 status.append(statusText,field('启用日期赠礼',enabled));enabled.onchange=()=>state.policy.enabled=enabled.checked;
 const offset=input('number',state.policy.utc_offset_minutes),catchup=input('number',state.policy.catchup_days);
 offset.min=-720;offset.max=840;catchup.min=0;catchup.max=31;
 status.append(field('时区：相对 UTC 的分钟数（北京时间为 480）',offset),field('停机后最多补发天数',catchup));
 status.append(el('p','首次开启从本服当天开始，不追发启用前的历史节日。生日和活动按年度回执核对；重启、重试和修改日期不会重复发同一年度的礼物。','muted'));
 function serviceText(){statusText.textContent='本服日期 '+state.today+' · '+(state.service?.running?(state.service.healthy?'后台任务健康':'后台任务等待就绪'):'后台任务已停止')+(state.service?.error?' · '+state.service.error:'');}
 serviceText();
 async function service(action){const r=await globalThis.fetch('/api/services/calendar.gifts',{method:'POST',headers:{'Content-Type':'application/json','X-ModManager-Token':ctx.token},body:JSON.stringify({action})});const data=await r.json();if(!r.ok)throw Error(data.error||'后台任务未完成');state.service=data;serviceText();}
 status.append(act('保存全部配置并启动任务',async()=>{await service('stop');await savePrivate();await savePolicy();await service('start');}),act('停止后台任务',()=>service('stop')));
 ctx.setInterval(async()=>{try{const r=await globalThis.fetch('/api/services/calendar.gifts');if(r.ok){state.service=await r.json();serviceText();}}catch{}},5000);
 function gifts(rule,parent){
  const mode=select(unitModes,rule.mode);parent.append(field('机体赠礼',mode));mode.onchange=()=>rule.mode=mode.value;
  const grades=el('details'),summary=el('summary','可选等级与回退');grades.append(summary);
  for(const grade of state.grades){const checked=rule.grades.some(g=>g[0]===grade.value[0]&&g[1]===grade.value[1]),box=input('checkbox',checked);box.onchange=()=>{rule.grades=rule.grades.filter(g=>g[0]!==grade.value[0]||g[1]!==grade.value[1]);if(box.checked)rule.grades.push(grade.value);};grades.append(field(grade.title,box));}
  const fallback=input('checkbox',rule.fallback);fallback.onchange=()=>rule.fallback=fallback.checked;grades.append(field('某档已集齐时，在相同基础等级中先找更稀有、再找更普通的未拥有机体',fallback));
  grades.append(el('p','随机一台时，不勾等级表示从全部可用等级中选择。每档一台时请勾选需要的等级。','muted'));parent.append(grades);
  const list=el('ul'),lookup=el('div'),q=input('search',''),results=el('select'),amount=input('number',1);amount.min=1;amount.max=9999;
  const render=()=>{list.replaceChildren();for(const uid of rule.unit_ids){const li=el('li',(labels.units[uid]||'机体名称暂未读取')+' ');li.append(act('移除',()=>{rule.unit_ids=rule.unit_ids.filter(x=>x!==uid);render();}));list.append(li);}for(const item of rule.items){const li=el('li',(labels.items[item.category_id]||'道具名称暂未读取')+' × '+item.amount+' ');li.append(act('移除',()=>{rule.items=rule.items.filter(x=>x!==item);render();}));list.append(li);}};
  let found=[];lookup.append(q,act('查找服务器中的机体或道具',async()=>{found=(await api('/catalog?q='+encodeURIComponent(q.value))).rows;results.replaceChildren(...found.map((r,i)=>new Option(r.title,String(i))));}),results,field('道具数量',amount));
  lookup.append(act('加入礼物',()=>{const chosen=found[Number(results.value)];if(!chosen)throw Error('请先查找并选择礼物');if(chosen.unit_id){if(!rule.unit_ids.includes(chosen.unit_id))rule.unit_ids.push(chosen.unit_id);rule.mode='fixed';mode.value='fixed';}else{const n=Number(amount.value);if(!Number.isInteger(n)||n<1||n>9999)throw Error('道具数量应为 1 到 9999');const prior=rule.items.find(x=>x.category_id===chosen.value);if(prior)prior.amount=n;else rule.items.push({category_id:chosen.value,amount:n});}render();}));
  parent.append(list,lookup);render();
 }
 const birthdays=section('生日默认规则'),birthEnabled=input('checkbox',state.policy.birthday.enabled),feb29=select([['feb28','平年在 2 月 28 日赠送'],['mar01','平年在 3 月 1 日赠送'],['leap_only','仅闰年的 2 月 29 日赠送']],state.policy.birthday.feb29);
 birthEnabled.onchange=()=>state.policy.birthday.enabled=birthEnabled.checked;feb29.onchange=()=>state.policy.birthday.feb29=feb29.value;
 birthdays.append(field('启用生日赠礼',birthEnabled),field('2 月 29 日生日',feb29));gifts(state.policy.birthday.rewards,birthdays);
 const events=section('节日与指定日期'),eventList=el('div');events.append(el('p','通用日历覆盖 2025–2040 年；节日当天每年一份。活动编号绑定发放历史，已有活动修改日期时请保留编号。','muted'),eventList);
 function eventEditor(event){
  const card=el('details'),summary=el('summary',event.title),title=input('text',event.title),on=input('checkbox',event.enabled);
  card.append(summary,field('活动名称',title),field('启用活动',on));title.oninput=()=>{event.title=title.value;summary.textContent=title.value;};on.onchange=()=>event.enabled=on.checked;
  const calendar=select([['festival','通用节日'],['annual','每年的指定月日'],['once','一次性日期']],event.calendar),dateFields=el('div');
  const drawDates=()=>{dateFields.replaceChildren();if(event.calendar==='festival'){const f=select([['new_year','元旦'],['spring_festival','春节'],['qingming','清明'],['labour_day','劳动节'],['dragon_boat','端午'],['mid_autumn','中秋'],['national_day','国庆']],event.festival);f.onchange=()=>event.festival=f.value;dateFields.append(f);}else{const d=input(event.calendar==='once'?'date':'text',event.calendar==='once'?event.date:event.month_day);d.placeholder='MM-DD';d.onchange=()=>{if(event.calendar==='once')event.date=d.value;else event.month_day=d.value;};dateFields.append(d);}};
  calendar.onchange=()=>{event.calendar=calendar.value;drawDates();};card.append(field('日期规则',calendar),dateFields);drawDates();
  const group=select(groupChoices(),event.group);group.onchange=()=>event.group=group.value;card.append(field('接收范围',group));gifts(event.rewards,card);
  card.append(act('删除此活动',()=>{state.policy.events=state.policy.events.filter(e=>e!==event);card.remove();}));return card;
 }
 for(const event of state.policy.events)eventList.append(eventEditor(event));
 events.append(act('添加指定日期活动',()=>{const event={id:'event_'+crypto.randomUUID().replaceAll('-','').slice(0,16),title:'新活动',enabled:true,calendar:'annual',festival:'new_year',month_day:state.today.slice(5),date:state.today,group:'all',rewards:{mode:'missing_one',grades:[],fallback:true,unit_ids:[],items:[]}};state.policy.events.push(event);const e=eventEditor(event);e.open=true;eventList.append(e);}));
 async function savePolicy(){state.policy.utc_offset_minutes=Number(offset.value);state.policy.catchup_days=Number(catchup.value);const fresh=await api('/policy',{value:state.policy,revision:state.policy_revision});state.policy_revision=fresh.policy_revision;state.today=fresh.today;ctx.message('通用规则已保存。');}
 events.append(act('保存全部通用规则',savePolicy));
 const roster=section('本服生日与玩家分组'),rosterList=el('div');roster.append(el('p','以下名单只保存在本服工作区。导出通用规则不会包含生日、玩家分组和发放记录。','muted'),rosterList);
 function drawRoster(){rosterList.replaceChildren();for(const row of state.private.birthdays){const line=el('div'),date=input('text',row.month_day),on=input('checkbox',row.enabled);date.placeholder='MM-DD';date.onchange=()=>row.month_day=date.value;on.onchange=()=>row.enabled=on.checked;line.append(el('span',(labels.players[row.account_id]||'玩家名字暂未读取')+' '),date,field('启用',on),act('移除',()=>{state.private.birthdays=state.private.birthdays.filter(r=>r!==row);drawRoster();}));rosterList.append(line);}}
 drawRoster();
 const playerSearch=input('search',''),players=el('select'),birthday=input('text','01-01');let playerRows=[];
 roster.append(playerSearch,act('按玩家名字查找',async()=>{playerRows=(await api('/players?q='+encodeURIComponent(playerSearch.value))).rows;players.replaceChildren(...playerRows.map((p,i)=>new Option(p.title,String(i))));}),players,field('生日（月-日）',birthday),act('加入本服生日',()=>{const p=playerRows[Number(players.value)];if(!p)throw Error('请选择玩家');if(state.private.birthdays.some(r=>r.account_id===p.value))throw Error('此玩家已有生日登记');state.private.birthdays.push({account_id:p.value,month_day:birthday.value,enabled:true});drawRoster();}));
 const groupName=input('text',''),groups=el('div'),groupSelect=el('select');groupName.placeholder='分组代号，例如 vip';
 function drawGroups(){groups.replaceChildren();groupSelect.replaceChildren(...Object.keys(state.private.groups).map(g=>new Option(g,g)));for(const [name,members] of Object.entries(state.private.groups)){const group=el('div');group.append(el('h3',name));for(const member of members){const row=el('span',(labels.players[member]||'玩家名字暂未读取')+' ');row.append(act('移出分组',()=>{state.private.groups[name]=members.filter(m=>m!==member);drawGroups();}));group.append(row);}groups.append(group);}}
 drawGroups();roster.append(groupName,act('建立本服玩家组',()=>{if(!/^[a-z][a-z0-9_-]{0,47}$/.test(groupName.value)||groupName.value==='all')throw Error('分组代号使用小写英文字母、数字和下划线');state.private.groups[groupName.value]??=[];drawGroups();}),groupSelect,act('把上方选中的玩家加入分组',()=>{const p=playerRows[Number(players.value)],name=groupSelect.value;if(!p||!name)throw Error('请选择玩家和分组');if(!state.private.groups[name].includes(p.value))state.private.groups[name].push(p.value);drawGroups();}),groups);
 async function savePrivate(){const fresh=await api('/private',{value:state.private,revision:state.private_revision});state.private_revision=fresh.private_revision;ctx.message('本服生日与分组已保存；新活动可在重新打开面板后选择这些分组。');}
 roster.append(act('保存本服私人名单',savePrivate),act('从旧生日登记表导入（保留已有登记）',async()=>{const fresh=await api('/import-birthdays',{revision:state.private_revision});state.private=fresh.private;state.private_revision=fresh.private_revision;drawRoster();drawGroups();ctx.message('导入 '+fresh.imported+' 条生日。');}));
 roster.append(act('导入旧生日发放记录，防止迁移后重发',async()=>{const value=await api('/import-legacy',{});ctx.message('已登记 '+value.imported+' 条旧生日回执；启动任务时核对迁移。');}));
 const statusNames={preview:'待发放预览',already_recorded:'本年度已经处理，不重复发放',legacy_recorded:'旧脚本已发过，本年度不重发',delivered:'已送入保管库',exhausted:'已集齐，未重复赠送',account_unavailable:'玩家不在当前服务器'};
 const preview=section('发放前预览'),previewDate=input('date',state.today),previewPlayer=select(Object.entries(labels.players),''),previewText=el('div');
 preview.append(field('日期',previewDate),field('玩家',previewPlayer),act('只读预览',async()=>{const value=await api('/preview',{date:previewDate.value,account_id:Number(previewPlayer.value)});previewText.replaceChildren();if(!value.events.length)previewText.append(el('p','此玩家在所选日期没有已启用的赠礼活动。'));for(const event of value.events){previewText.append(el('h3',event.title),el('p',statusNames[event.status]||event.status));for(const gift of event.gifts||[])previewText.append(el('p',(gift.unit_id?labels.units[gift.unit_id]:labels.items[gift.category_id])+' × '+gift.amount));}}),previewText);
 const history=section('最近发放'),historyText=el('div');history.append(act('查看本服回执',async()=>{const value=await api('/history');historyText.replaceChildren();if(!value.records.length)historyText.append(el('p','当前没有发放回执。'));for(const record of value.records){const id=record.event.split(':')[0],title=id==='birthday'?'生日快乐':state.policy.events.find(e=>e.id===id)?.title||'历史日期活动';historyText.append(el('p',record.date+' · '+(labels.players[record.account_id]||'已不在本服的玩家')+' · '+title+' · '+(statusNames[record.status]||record.status)));}}),historyText);
 const exports=section('分享通用规则');exports.append(act('导出通用规则（不含本服生日）',async()=>{const r=await ctx.fetch('/api/calendar-gifts/export');if(!r.ok)throw Error('导出未完成');const url=URL.createObjectURL(await r.blob()),a=el('a');a.href=url;a.download='calendar-gift-rules.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}));
}
