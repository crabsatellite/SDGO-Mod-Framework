const node=(tag,text,cls)=>{
 const value=document.createElement(tag);
 if(text!==undefined)value.textContent=text;
 if(cls)value.className=cls;
 return value;
};

const integer=(input,label,minimum=0,maximum=100)=>{
 const value=Number(input.value);
 if(!Number.isInteger(value)||value<minimum||value>maximum){
  throw new Error(label+'必须是 '+minimum+'–'+maximum+' 的整数');
 }
 return value;
};

const percentInput=(label,value=0)=>{
 const box=node('label',label),input=document.createElement('input');
 input.type='number';input.min='0';input.max='100';input.step='1';input.value=value;
 box.append(input);return {box,input};
};

const bonusEditor=(title,value={})=>{
 const card=node('section',undefined,'ability-card');
 card.append(node('h3',title));
 const grid=node('div',undefined,'ability-bonus-grid');
 const fields={
  gp_percent:percentInput('GB / G币 %',value.gp_percent??0),
  unit_exp_percent:percentInput('机体经验 %',value.unit_exp_percent??0),
  energy_save_percent:percentInput('能量节省 %',value.energy_save_percent??0),
 };
 for(const entry of Object.values(fields))grid.append(entry.box);
 card.append(grid);
 return {
  card,
  set(next={}){for(const [key,entry] of Object.entries(fields))entry.input.value=next[key]??0;},
  read(){return {
   gp_percent:integer(fields.gp_percent.input,title+' GB'),
   unit_exp_percent:integer(fields.unit_exp_percent.input,title+'机体经验'),
   energy_save_percent:integer(fields.energy_save_percent.input,title+'能量节省'),
  };},
 };
};

export function createAbilitiesEditor(root,value,options={}){
 const heading=node('div',undefined,'section-heading');
 const copy=node('div');copy.append(node('h2','副官能力'),node('p','只编辑协议已经定义的原生奖励字段。扩展能力按登记名称保存，不根据字段名猜测效果。','note'));
 const mode=document.createElement('select');
 mode.append(new Option('沿用模板能力','inherit'),new Option('自定义能力','custom'));
 const modeLabel=node('label','能力模式','compact-field');modeLabel.append(mode);
 heading.append(copy,modeLabel);root.append(heading);

 const custom=node('div',undefined,'ability-custom');
 const bonuses={
  general:bonusEditor('常规结算'),
  victory:bonusEditor('胜利结算'),
  defeat:bonusEditor('失败结算'),
 };
 const bonusGrid=node('div',undefined,'ability-grid');
 for(const entry of Object.values(bonuses))bonusGrid.append(entry.card);
 custom.append(bonusGrid);

 const extra=node('div',undefined,'ability-extra-grid');
 const pilot=percentInput('驾驶员经验 %',0);
 const missionBox=node('label','任务部件规则'),mission=document.createElement('select');
 mission.append(new Option('沿用模板','inherit'),new Option('启用','true'),new Option('停用','false'));missionBox.append(mission);
 extra.append(pilot.box,missionBox);custom.append(extra);

 const timedHeading=node('div',undefined,'inline spread');
 timedHeading.append(node('h3','定时能力'));
 const addTimed=node('button','＋ 添加时间段');addTimed.type='button';timedHeading.append(addTimed);
 const timedList=node('div',undefined,'timed-list');custom.append(timedHeading,timedList);
 let timed=[];
 const renderTimed=()=>{
  timedList.replaceChildren();
  timed.forEach((slot,index)=>{
   const row=node('div',undefined,'timed-row');
   const start=node('label','开始'),startInput=document.createElement('input');startInput.type='time';startInput.value=slot.start||'00:00';start.append(startInput);
   const end=node('label','结束'),endInput=document.createElement('input');endInput.type='time';endInput.value=slot.end||'00:00';end.append(endInput);
   const gp=percentInput('GB %',slot.gp_percent??0),unit=percentInput('机体经验 %',slot.unit_exp_percent??0),energy=percentInput('能量节省 %',slot.energy_save_percent??0);
   const remove=node('button','×');remove.type='button';remove.title='移除此时间段';
   const sync=()=>{slot.start=startInput.value;slot.end=endInput.value;slot.gp_percent=Number(gp.input.value);slot.unit_exp_percent=Number(unit.input.value);slot.energy_save_percent=Number(energy.input.value);};
   for(const input of [startInput,endInput,gp.input,unit.input,energy.input])input.addEventListener('input',sync);
   remove.onclick=()=>{timed.splice(index,1);renderTimed();};
   row.append(start,end,gp.box,unit.box,energy.box,remove);timedList.append(row);
  });
  addTimed.disabled=timed.length>=4;
 };
 addTimed.onclick=()=>{if(timed.length<4){timed.push({start:'00:00',end:'00:00',gp_percent:0,unit_exp_percent:0,energy_save_percent:0});renderTimed();}};

 const extensionDetails=node('details',undefined,'ability-extensions'),summary=node('summary','协议扩展');
 const extensionHelp=node('p','JSON 键必须是服务端已登记的扩展名称。未登记名称会由后端拒绝；界面不会把未知字段标成已证实能力。','note');
 const registered=options.registered_extensions||[];
 if(registered.length)extensionHelp.append(document.createTextNode(' 当前登记：'+registered.join('、')));
 const extensions=document.createElement('textarea');extensions.rows=5;extensions.spellcheck=false;
 extensionDetails.append(summary,extensionHelp,extensions);custom.append(extensionDetails);
 root.append(custom);

 const set=next=>{
  const configured=next&&next.mode==='custom';
  mode.value=configured?'custom':'inherit';
  const source=configured?next:{};
  for(const [key,entry] of Object.entries(bonuses))entry.set(source[key]);
  pilot.input.value=source.pilot_exp_percent??0;
  mission.value=source.mission_parts===true?'true':source.mission_parts===false?'false':'inherit';
  timed=structuredClone(source.timed||[]);
  extensions.value=JSON.stringify(source.extensions||{},null,2);
  renderTimed();refresh();
 };
 const refresh=()=>{const enabled=mode.value==='custom';custom.classList.toggle('disabled',!enabled);for(const control of custom.querySelectorAll('input,select,textarea,button'))control.disabled=!enabled;};
 mode.onchange=refresh;
 const read=()=>{
  if(mode.value==='inherit')return null;
  let extensionValue;
  try{extensionValue=JSON.parse(extensions.value.trim()||'{}');}catch{throw new Error('协议扩展必须是有效 JSON');}
  if(!extensionValue||Array.isArray(extensionValue)||typeof extensionValue!=='object')throw new Error('协议扩展必须是 JSON 对象');
  return {
   schema_version:1,
   mode:'custom',
   general:bonuses.general.read(),
   victory:bonuses.victory.read(),
   defeat:bonuses.defeat.read(),
   timed:timed.map((slot,index)=>({
    start:slot.start,end:slot.end,
    gp_percent:integer({value:slot.gp_percent},'定时能力 '+(index+1)+' GB'),
    unit_exp_percent:integer({value:slot.unit_exp_percent},'定时能力 '+(index+1)+'机体经验'),
    energy_save_percent:integer({value:slot.energy_save_percent},'定时能力 '+(index+1)+'能量节省'),
   })),
   pilot_exp_percent:integer(pilot.input,'驾驶员经验'),
   mission_parts:mission.value==='inherit'?null:mission.value==='true',
   extensions:extensionValue,
  };
 };
 set(value);
 return {read,set,snapshot:()=>JSON.stringify(read())};
}

const valueText=value=>{
 if(value===null)return 'null';
 if(value===undefined)return '—';
 if(Array.isArray(value))return value.join('、')||'—';
 if(typeof value==='object')return JSON.stringify(value);
 return String(value);
};

const fact=(label,value)=>{
 const row=node('div',undefined,'protocol-fact');
 row.append(node('span',label),node('strong',valueText(value)));return row;
};

export function renderPackPreview(root,result){
 root.replaceChildren();
 const preview=result?.preview||result||{};
 const title=node('div',undefined,'preview-heading');
 const image=document.createElement('img');image.className='portrait';image.alt='分享包头像预览';
 const url=result?.portrait_url||preview.portrait_url;
 if(url)image.src=url.startsWith('/api/')?'/api/providers/operators'+url:url;
 const copy=node('div');copy.append(node('h2',preview.name||'副官分享包'),node('p',(preview.format||'未知格式')+' · v'+(preview.format_version??'—'),'note'));
 title.append(image,copy);root.append(title);
 const grid=node('div',undefined,'protocol-facts');
 grid.append(
  fact('包 SHA-256',result?.pack_sha256||preview.sha256),
  fact('资源模板',preview.source_template),
  fact('可选模板',preview.source_templates),
  fact('语音文件',preview.voice_files),
  fact('语音事件',Array.isArray(preview.voice_events)?preview.voice_events.length:preview.voice_events),
  fact('资源文件',preview.files?Object.keys(preview.files).length:undefined),
  fact('基础依赖',preview.base_dependencies?.length||0),
  fact('分发范围',preview.distribution?.scope),
 );
 root.append(grid);
 if(preview.suppressed_audio_dependencies?.length){
  root.append(node('p','有 '+preview.suppressed_audio_dependencies.length+' 项语音引用未随包提供，编译时会清空，不使用目标客户端的其他语言语音。','notice warning'));
 }
 if(preview.resources?.source_missing_dependencies?.length){
  const missing=node('details',undefined,'notice warning');
  missing.append(node('summary','来源客户端缺少 '+preview.resources.source_missing_dependencies.length+' 项资源'),node('p',preview.resources.source_missing_dependencies.join('、'),'mono-wrap'));
  root.append(missing);
 }
 if(preview.base_dependencies?.length){
  const warning=node('details',undefined,'notice warning');warning.append(node('summary','此包使用目标客户端的 '+preview.base_dependencies.length+' 项原生 fallback'));
  warning.append(node('p',preview.base_dependencies.join('、'),'mono-wrap'));root.append(warning);
 }
 if(preview.resources?.profile){
  const details=node('details');details.append(node('summary','资源角色 profile'),node('pre',JSON.stringify(preview.resources.profile,null,2),'protocol-json'));root.append(details);
 }
 if(preview.abilities)root.append(fact('能力层','包含'));
 if(preview.acquisition)root.append(fact('获取层','包含'));
 if(preview.storefront)root.append(fact('商城预填层','包含'));
 const raw=node('details');raw.append(node('summary','完整预览 JSON'),node('pre',JSON.stringify(preview,null,2),'protocol-json'));root.append(raw);
}

export function renderDeveloperProtocol(root,protocol){
 root.replaceChildren();
 const head=node('div',undefined,'section-heading');
 const copy=node('div');copy.append(node('h2','副官分享协议开发者信息'),node('p','资源包是独立客户端内容；Manager 与框架发布物只处理协议和配置，不捆绑实际素材。','note'));head.append(copy);root.append(head);
 const layers=protocol?.layers||protocol?.layer_versions||{resources:1,abilities:1,acquisition:1,storefront:1};
 const cards=node('div',undefined,'protocol-grid');
 for(const [key,title,text] of [
  ['resources','资源层','声明台词表、七个图像角色、出击语音及完整依赖。'],
  ['abilities','能力层','使用已登记的能力 schema；未知扩展名称 fail-closed。'],
  ['acquisition','获取层','携带发放方式与掉落草稿，导入后仍需保存和应用。'],
  ['storefront','商城层','只预填商品草稿字段，创建时保持未上架。'],
 ]){
  const card=node('section',undefined,'protocol-card');card.append(node('h3',title),node('p',text,'note'),node('code','layer version '+(layers[key]??'—')));cards.append(card);
 }
 root.append(cards);
 if(Array.isArray(protocol?.sections)){
  for(const section of protocol.sections){
   const card=node('section',undefined,'protocol-doc');card.append(node('h3',section?.title||'协议说明'));
   const lines=Array.isArray(section?.lines)?section.lines:[];
   if(lines.length){const list=node('ul');for(const line of lines)list.append(node('li',String(line)));card.append(list);}
   root.append(card);
  }
 }
 const resources=node('section',undefined,'protocol-doc');
 resources.append(node('h3','资源格式与命名空间'),node('p','原生资源只允许 gdrs/数字.gdt、sprs/数字.spr、txrs/数字.txr、sdrs/数字.zsd。导入编译后改写到每个新副官自己的客户端资源命名空间，不覆盖模板默认文件。'));
 resources.append(node('p','台词表记录为 710 bytes；语音路径是 offset 502 与 602 的两个独立 50-wchar 字段。完整副官合同跟踪 39 个 row/variant 角色，同时保留表内其他有效事件。'));
 const api=node('section',undefined,'protocol-doc');api.append(node('h3','Manager API'));
 const list=node('ul');
 for(const line of [
  'GET /api/operators/protocol — 读取静态协议与版本；GET /api/operators — 读取当前目录的扩展登记。',
  'POST /api/operators/import-preview — 上传 ZIP/base64 或选择本机路径，只做预览和规范化。',
  'POST /api/operators/import — 按预览 SHA 创建独立副官草稿。',
  'GET /api/operators/packs/{sha}/portrait — 读取包头像预览。',
  'GET /api/operators/{id}/share — 导出当前配置的 .opack。',
 ])list.append(node('li',line));
 api.append(list);root.append(resources,api);
 const extension=node('section',undefined,'protocol-doc');extension.append(node('h3','扩展方法'),node('p','version 1 的 required_features 必须为空。能力扩展使用目录中已登记的稳定命名空间，并由受信任的原生 Mod 通过 SDK 提供处理器；素材包不能自行登记或执行扩展。未知层版本、命名空间或处理器应 fail-closed。'));root.append(extension);
 const raw=node('details');raw.append(node('summary','服务器返回的协议描述'),node('pre',JSON.stringify(protocol||{},null,2),'protocol-json'));root.append(raw);
}
