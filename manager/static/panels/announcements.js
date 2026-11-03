import {e,button,field,download} from '/panel_helpers.js';

export async function mount(root,ctx){
 root.classList.add('announcement-panel');
 const style=e('style');style.textContent='.announcement-panel fieldset{display:flex;flex-wrap:wrap;gap:10px 20px;border:1px solid var(--ui-line-strong,#2d415a);padding:12px;margin:8px 0 18px}.announcement-panel fieldset label{display:flex;align-items:center;gap:8px;min-width:130px}.announcement-panel fieldset input{width:17px;height:17px;margin:0}.announcement-panel h3{margin-top:20px;margin-bottom:8px}.announcement-panel .notice-rule{margin:16px 0}.announcement-panel .notice-rule>summary{font-size:18px;font-weight:700;cursor:pointer}.announcement-panel .notice-rule[open]>summary{margin-bottom:20px}';root.append(style);
 const api=(body)=>ctx.request('/api/announcements',body,body?'POST':'GET');
 let data=await api();
 root.append(e('p','规则内的日期、星期、每日时段和次数限制同时满足时，任一选中的事件可以触发。开始与结束时间相同表示全天；跨午夜时按开始日的星期计算。','intro'));
 root.append(e('p','登录位置使用原生网页公告，游戏内位置使用左侧 GM 文字公告。无需 Manager 常驻。保存后约一秒内载入；次数按本客户端累计，两处共享上限，0 表示不限。','muted'));
 const zone=field('时区（UTC 偏移分钟，例如日本 540、中国 480）',data.policy.utc_offset_minutes,'number');root.append(zone.box);
 const list=e('div');root.append(list);
 const triggers=[['enter','进入该公告位置'],['timer','在时段内定时'],['login','登录成功'],['lobby','进入大厅'],['room','进入房间'],['battle_start','战斗开始'],['battle_end','战斗结束']];
 function bind(parent,r,key,label,type='text'){const f=field(label,r[key],type);if(type==='number'){f.input.min=key==='duration_seconds'||key==='interval_seconds'?'3':'0';f.input.max=key==='duration_seconds'?'120':key==='interval_seconds'?'86400':key==='priority'?'100':'1000000';}f.input.oninput=()=>r[key]=type==='checkbox'?f.input.checked:type==='number'?Number(f.input.value):f.input.value;parent.append(f.box);}
 function checks(parent,r,key,options){const group=e('fieldset');for(const [value,title] of options){const label=e('label'),input=e('input');input.type='checkbox';input.checked=r[key].includes(value);input.onchange=()=>{r[key]=input.checked?[...r[key],value]:r[key].filter(v=>v!==value);};label.append(input,document.createTextNode(title));group.append(label);}parent.append(group);}
 function render(expand){list.replaceChildren();for(const [index,r] of data.policy.rules.entries()){
  const card=e('details',undefined,'manager-card notice-rule'),heading=e('summary',r.title);card.open=expand?r.id===expand:index===0;card.append(heading);
  bind(card,r,'enabled','启用此公告','checkbox');bind(card,r,'title','标题');
  const body=e('label','正文（最多 960 字，游戏内自动分行翻页）'),text=e('textarea');text.value=r.text;text.maxLength=960;text.rows=5;text.style.width='100%';text.oninput=()=>r.text=text.value;body.append(text);card.append(body);
  card.append(e('h3','展示位置'));checks(card,r,'surfaces',[['login','登录界面公告'],['game','游戏内左侧公告']]);
  card.append(e('h3','触发事件（任一满足）'));checks(card,r,'triggers',triggers);
  card.append(e('h3','星期'));checks(card,r,'weekdays',['一','二','三','四','五','六','日'].map((n,i)=>[i,'周'+n]));
  const grid=e('div',undefined,'settings-grid');card.append(grid);
  for(const [key,title,type] of [['start_date','开始日期（可留空）','date'],['end_date','结束日期（可留空）','date'],['start_time','每天开始','time'],['end_time','每天结束','time'],['duration_seconds','每页展示秒数（3–120）','number'],['interval_seconds','同条公告最短间隔秒数','number'],['max_session','每次启动最多展示次数','number'],['max_day','每日最多展示次数','number'],['max_total','累计最多展示次数','number'],['priority','优先级（0–100，越大越先）','number']])bind(grid,r,key,title,type);
  card.append(button('复制',()=>{const clone=structuredClone(r);clone.id='notice_'+crypto.randomUUID().replaceAll('-','').slice(0,16);clone.title+='（副本）';data.policy.rules.splice(index+1,0,clone);render(clone.id);},ctx),button('上移',()=>{if(index){[data.policy.rules[index-1],data.policy.rules[index]]=[r,data.policy.rules[index-1]];render(r.id);}},ctx),button('删除',()=>{data.policy.rules.splice(index,1);render();},ctx));list.append(card);
 }}
 render();
 root.append(button('新增公告',()=>{data.policy.rules.push({id:'notice_'+crypto.randomUUID().replaceAll('-','').slice(0,16),enabled:true,title:'新公告',text:'在这里输入公告内容',surfaces:['game'],triggers:['enter'],weekdays:[0,1,2,3,4,5,6],start_time:'00:00',end_time:'00:00',start_date:'',end_date:'',duration_seconds:10,interval_seconds:60,max_session:1,max_day:0,max_total:0,priority:0});render(data.policy.rules.at(-1).id);},ctx),button('保存全部公告规则',async()=>{data.policy.utc_offset_minutes=Number(zone.input.value);data=await api({policy:data.policy,revision:data.revision});ctx.message('公告规则已保存，游戏自动载入。');},ctx,'primary'));
 root.append(e('p','登录前无法识别账号，次数按客户端设备保存。修改标题和内容不会清零次数；需要独立次数时复制为一条新公告。登录公告使用 Windows 网页控件；首次安装或更新 Mod 后重新打开客户端。','muted'));
 root.append(button('导出已保存的公告配置包',async()=>{const response=await ctx.fetch('/api/announcements/export');if(!response.ok)throw Error('公告导出未完成');await download(response,'SDGO-announcements.zip');},ctx));
 root.append(e('p','配置包包含公告 Mod、规则和登录页，保留本地展示计数。其他客户端关闭游戏后，可从 Manager 的“导入配置包”安装。','muted'));
}
