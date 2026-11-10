import {e,button,download} from '/panel_helpers.js';

const MODE_LABELS={normal:'普通对战',deathmatch:'死斗',grid:'格子战',tag:'组队接力',captain:'队长战',ai:'AI 对战',ai_battleship:'AI 战舰'};
const SERVER_LABELS={training:'训练服',beginner:'新手服',expert:'高手服',clan:'军团服'};

async function json(response){
 if(response instanceof Response){const value=await response.json();if(!response.ok)throw Error(value.error||'请求失败');return value;}
 return response;
}

function checks(options,labels,selected){
 const box=e('div',undefined,'choice-row'),inputs={};
 for(const value of options){
  const label=e('label',undefined,'inline-check'),input=document.createElement('input');
  input.type='checkbox';input.checked=selected.includes(value);inputs[value]=input;
  label.append(input,document.createTextNode(' '+(labels[value]||value)));box.append(label);
 }
 return {box,value:()=>options.filter(v=>inputs[v].checked)};
}

export async function mount(root,ctx){
 let state=await json(await ctx.fetch('/api/maps'));
 const report=(text,error)=>{result.textContent=text;ctx.message(text,error);};

 const intro=e('section',undefined,'manager-card');
 intro.append(
  e('h2','地图与任务导入'),
  e('p','把按地图包协议准备好的地图（地形、模型、出生点、天空、音乐）和任务（阶段目标、刷怪、对话、过场与横幅）登记到游戏。工具只负责导入，不修改原有地图与任务。','muted'),
  e('p','流程：登记地图包 → 调整名称、模式与服务器 → 编译 → 安装客户端包（先关闭客户端）→ 检查并应用服务端 → 重启 World/Battle 服务与客户端。','muted')
 );
 root.append(intro);

 const addCard=e('section',undefined,'manager-card'),folder=document.createElement('input');
 folder.placeholder='地图包目录（含 map.json）';folder.style.width='100%';
 addCard.append(e('h2','登记地图包'),e('p','重复登记同一 map_key 会用新文件替换旧版本，保留名称和模式等修改。','muted'),folder,
  button('登记',async()=>{const value=await ctx.request('/api/maps/add',{folder:folder.value});state=value.state;folder.value='';render();report(value.message);},ctx,'ui-btn is-blue'));
 root.append(addCard);

 const examples=e('section',undefined,'manager-card');
 examples.append(e('h2','示例地图'),e('p','从框架公开仓库下载完整的示例地图包（逐个文件校验 SHA-256）并登记到工作区，可直接编译试玩，也可作为编写地图包的参考。','muted'));
 for(const item of state.examples||[]){
  examples.append(e('p',item.name.zh+' — '+item.description),
   button('下载并登记',async()=>{const value=await ctx.request('/api/maps/examples/add',{id:item.id});state=value.state;render();report(value.message);},ctx,'ui-btn'));
 }
 root.append(examples);

 const list=e('section',undefined,'manager-card');root.append(list);

 const flow=e('section',undefined,'manager-card'),summary=e('div'),actions=e('div',undefined,'actions'),result=e('p');
 flow.append(e('h2','编译与发布'),e('p','编号在编译时按登记顺序自动分配，紧接客户端原有地图与任务之后；移除或调整顺序会让后面的地图重新编号。','muted'),summary,actions,result);
 root.append(flow);
 for(const [id,label,confirmText] of [
  ['build','编译全部启用的地图',''],
  ['install-client','安装客户端包','安装到配置的客户端目录？请先关闭该客户端。'],
  ['server/preflight','检查服务端（不写入）',''],
  ['server/apply','应用到服务端','写入配置的游戏数据库？已有不属于地图导入的同键行时会整体拒绝。'],
  ['server/readback','读回服务端',''],
 ]){
  actions.append(button(label,async()=>{
   if(confirmText&&!confirm(confirmText))return;
   const value=await ctx.request('/api/maps/'+id,{});
   if(value.state)state=value.state;
   if(id==='build')report(`已编译 ${value.report.maps.length} 张地图、${value.report.records} 条客户端记录，版本 ${value.report.revision}。`);
   else if(id==='server/preflight'){const p=value.preflight;report(`新增 ${p.insert} 行、更新 ${p.update} 行、保持 ${p.keep} 行；冲突 ${p.conflicts.length} 处${p.conflicts.length?'：'+p.conflicts.slice(0,4).map(c=>c.table+' '+c.key).join('；'):''}`,p.conflicts.length>0);}
   else if(id==='server/readback')report(value.readback.ok?'服务端行与发布物一致。':`服务端缺 ${value.readback.missing.length} 行、不一致 ${value.readback.differ.length} 行。`,!value.readback.ok);
   else report(value.message||'完成');
   render();
  },ctx,id==='build'?'ui-btn is-primary':'ui-btn'));
 }
 actions.append(button('下载客户端包',async()=>{await download(await ctx.fetch('/api/maps/client-package'),'SDGO-MapImport-Client.zip');},ctx,'ui-btn'));

 function mapCard(map,index){
  const card=e('div',undefined,'manager-subcard');
  if(map.error){card.append(e('h3',map.key),e('p',map.error,'error'));return card;}
  const title=e('h3',`${index+1}. ${map.name.zh||map.key}`);
  const zh=document.createElement('input');zh.value=map.edits.name_zh||'';zh.placeholder=map.name.zh;
  const en=document.createElement('input');en.value=map.edits.name_en||'';en.placeholder=map.name.en;
  const enabled=document.createElement('input');enabled.type='checkbox';enabled.checked=map.enabled;
  const modes=checks(state.pvp_modes,MODE_LABELS,map.modes),servers=checks(state.server_types,SERVER_LABELS,map.servers);
  const missionInputs={},missionBox=e('div',undefined,'choice-row');
  for(const file of map.available_missions){
   const info=map.missions.find(m=>m.file===file),label=e('label',undefined,'inline-check'),input=document.createElement('input');
   input.type='checkbox';input.checked=!!info;missionInputs[file]=input;
   label.append(input,document.createTextNode(' '+(info&&info.name?`${info.name}（${info.phases} 阶段）`:file)));missionBox.append(label);
  }
  const grid=e('div',undefined,'settings-grid');
  const row=(text,node)=>{const l=e('label',text);l.append(node);return l;};
  const enabledLabel=e('label',undefined,'inline-check');enabledLabel.append(enabled,document.createTextNode(' 发布这张地图'));
  grid.append(row('中文名称',zh),row('英文名称',en));
  card.append(title,e('p',`${map.key} · 场地 ${map.half_extent*2} × ${map.half_extent*2} · 物件 ${map.objects} 个`,'muted'),grid,
   enabledLabel,e('h4','对战模式'),modes.box,e('h4','开放服务器'),servers.box,e('h4','任务'),
   map.available_missions.length?missionBox:e('p','地图包内没有任务文件','muted'));
  const buttons=e('div',undefined,'actions');
  buttons.append(
   button('保存',async()=>{
    const value=await ctx.request('/api/maps/update',{key:map.key,values:{name_zh:zh.value,name_en:en.value,enabled:enabled.checked,
     modes:modes.value(),servers:servers.value(),missions:Object.keys(missionInputs).filter(f=>missionInputs[f].checked)}});
    state=value.state;render();report('已保存。重新编译、安装并应用后生效。');
   },ctx,'ui-btn is-blue'),
   button('上移',async()=>{if(index===0)return;const value=await ctx.request('/api/maps/update',{key:map.key,values:{position:index-1}});state=value.state;render();},ctx,'ui-btn is-ghost'),
   button('移除',async()=>{if(!confirm(`从工作区移除 ${map.key}？重新编译并应用后游戏内会撤下它。`))return;const value=await ctx.request('/api/maps/remove',{key:map.key});state=value.state;render();report(value.message);},ctx,'ui-btn is-ghost')
  );
  card.append(buttons);
  return card;
 }

 function render(){
  list.replaceChildren(e('h2','已登记的地图'));
  if(!state.maps.length)list.append(e('p','还没有登记地图包。','muted'));
  state.maps.forEach((map,index)=>list.append(mapCard(map,index)));
  summary.replaceChildren();
  if(!state.client_configured||!state.server_configured)summary.append(e('p','请先在模块设置中配置配套客户端目录和 WorldServer INI。','error'));
  const pub=state.publication;
  if(pub){
   const lines=pub.maps.map(m=>`${m.key}：地图编号 ${Object.entries(m.map_nos).map(([v,n])=>(v==='pvp'?'对战 ':'任务 ')+n).join('、')}${m.missions.length?'；任务 GameMode '+m.missions.map(x=>x.game_modes.join('/')).join('、'):''}`);
   summary.append(e('p',`当前发布物版本 ${pub.revision}`));for(const line of lines)summary.append(e('p',line,'muted'));
  }
  const receipt=state.client_receipt;
  summary.append(e('p',receipt?`客户端已安装版本 ${receipt.revision}${pub&&receipt.revision!==pub.revision?'（与当前发布物不同，请重新安装）':''}`:'客户端尚未安装地图包',receipt&&pub&&receipt.revision!==pub.revision?'error':'muted'));
 }
 render();
}
