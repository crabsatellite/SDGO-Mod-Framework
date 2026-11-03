// Optional participation settings, inside the existing reward panel/session.
export function socialEditor(parent,api,refresh,onError){
 const e=(tag,text)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;return n;};
 const card=e('section');card.className='card';card.style.marginTop='20px';parent.append(card);
 let value,table,config;
 const field=(box,label,node)=>{const l=e('label',label);l.append(node);box.append(l);return node;};
 const input=(type,v)=>{const n=e('input');n.type=type;if(type==='checkbox'){n.checked=v;n.style.width='auto';}else n.value=v;return n;};
 const select=(values,v)=>{const n=e('select');for(const [key,title] of values)n.add(new Option(title,key));n.value=v;return n;};
 const action=(text,fn)=>{const n=e('button',text);n.type='button';n.onclick=()=>Promise.resolve().then(fn).catch(onError);return n;};
 function render(){
  card.replaceChildren(e('h2','人数倍率与周期聚会奖励'));
  card.append(e('p','可按本服规模配置。新配置默认不启用聚会补给；固定奖励不乘基础、活动或人数倍率。已有账号跨服务器共用同一规则的领取记录。'));
  const custom=field(card,'使用自定义人数倍率表',input('checkbox',table.length>0));
  const counts=e('div');counts.className='fields';card.append(counts);
  const countInputs=[];
  for(let i=1;i<=12;i++){
   const n=field(counts,i+' 名有效真人的倍率',input('number',table[i-1]??Number((1.1**(i-1)).toFixed(6))));
   n.min='.000001';n.max='100';n.step='.01';n.disabled=!custom.checked;countInputs.push(n);
  }
  custom.onchange=()=>{table=custom.checked?countInputs.map(n=>n.value):[];for(const n of countInputs)n.disabled=!custom.checked;};
  for(const n of countInputs)n.onchange=()=>{if(custom.checked)table=countInputs.map(x=>x.value);};
  const enabled=field(card,'启用周期聚会奖励',input('checkbox',config.enabled));enabled.onchange=()=>config.enabled=enabled.checked;
  for(const [key,label,min,max] of [['minimum_active_seconds','本人及计数真人的最低有效秒数',30,3600],['utc_offset_minutes','周期时区（UTC分钟偏移）',-720,840]]){
   const n=field(card,label,input('number',config[key]));n.min=min;n.max=max;n.onchange=()=>config[key]=Number(n.value);
  }
  const contribution=field(card,'本人及计数真人必须有实际战斗贡献',input('checkbox',config.require_contribution));contribution.onchange=()=>config.require_contribution=contribution.checked;
  for(const rule of config.rules){
   const row=e('fieldset');card.append(row);row.append(e('legend',rule.title));
   const title=field(row,'规则名称',input('text',rule.title));title.oninput=()=>rule.title=title.value;
   const on=field(row,'启用本条规则',input('checkbox',rule.enabled));on.onchange=()=>rule.enabled=on.checked;
   row.append(e('small','编号 '+rule.id+'（绑定领取记录；删除后用新编号重建代表另一份奖励）'));
   const cycle=field(row,'奖励周期',select([['match','每场'],['day','每日首次'],['week','每周首次']],rule.period));cycle.onchange=()=>rule.period=cycle.value;
   for(const [key,label,min,max] of [['minimum_humans','最低真人数',2,12],['gb','固定 GB',0,1000000],['mb','固定 MB',0,100000]]){
    const n=field(row,label,input('number',rule[key]));n.min=min;n.max=max;n.onchange=()=>rule[key]=Number(n.value);
   }
   for(const [mode,label] of [['pvp','适用于对战（含AI对战）'],['pve','适用于任务']]){
    const n=field(row,label,input('checkbox',rule.modes.includes(mode)));n.onchange=()=>{rule.modes=rule.modes.filter(m=>m!==mode);if(n.checked)rule.modes.push(mode);};
   }
   row.append(action('删除此规则',()=>{config.rules=config.rules.filter(r=>r!==rule);render();}));
  }
  card.append(action('添加聚会规则（初始不发币）',()=>{config.rules.push({id:'rule_'+crypto.randomUUID().replaceAll('-','').slice(0,16),title:'新聚会规则',enabled:true,minimum_humans:2,period:'week',modes:['pvp','pve'],gb:0,mb:0});render();}));
  card.append(e('p','AI和观战者不计真人。人数、参与时长与贡献来自原生结算快照，不接受玩家网页上报。周期按对局结束时间计算；回包丢失重试不会重复发放。GB补给通过原生钱包单独入账，不占单局65,535字段。'));
  card.append(action('保存人数与聚会规则',async()=>{const saved=await api('/social-rules',{rules:{human_multipliers:table,gathering:config}});refresh(saved);}));
 }
 return {update(state){value=state;table=structuredClone(value.human_multipliers||[]);config=structuredClone(value.gathering.config);render();}};
}
