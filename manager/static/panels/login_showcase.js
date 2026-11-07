import {e,saveBar} from '/panel_helpers.js';

const gradeOrder=['C','CS','CR','CU','B','BS','BR','BU','A','AS','AR','AU','S','SS','SR','SU'];

async function json(response){
 if(response instanceof Response){
  const value=await response.json();
  if(!response.ok)throw Error(value.error||'请求失败');
  return value;
 }
 return response;
}

export async function mount(root,ctx){
 root.classList.add('login-showcase-panel');
 const style=e('style');
 style.textContent=`
  .login-showcase-panel .choice-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;border:0;padding:0}
  .login-showcase-panel .choice-card{display:grid;grid-template-columns:auto 1fr;gap:5px 10px;align-items:start;border:1px solid var(--ui-line-strong,#34445a);border-radius:10px;padding:14px;cursor:pointer}
  .login-showcase-panel .choice-card:has(input:checked){border-color:var(--ui-accent,#69dce8);background:#19313b}
  .login-showcase-panel .choice-card input{grid-row:1/3;margin-top:4px}.login-showcase-panel .choice-card span{grid-column:2}
  .login-showcase-panel .grade-grid{display:grid;grid-template-columns:repeat(4,minmax(70px,1fr));gap:8px 14px}
  .login-showcase-panel .grade-grid label,.login-showcase-panel .unit-row{display:flex;align-items:center;gap:8px}
  .login-showcase-panel .unit-tools{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
  .login-showcase-panel .unit-tools input{flex:1;min-width:240px}
  .login-showcase-panel .unit-list{max-height:420px;overflow:auto;border:1px solid var(--ui-line-strong,#34445a);border-radius:9px}
  .login-showcase-panel .unit-row{padding:9px 11px;border-bottom:1px solid var(--ui-line,#28384d)}
  .login-showcase-panel .unit-row:last-child{border-bottom:0}.login-showcase-panel .unit-row small{margin-left:auto}
  .login-showcase-panel .preview-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:7px;margin-top:10px}
  .login-showcase-panel .preview-item{padding:8px 10px;border-radius:7px;background:#162434;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .login-showcase-panel .mode-options[hidden]{display:none}
  @media(max-width:720px){.login-showcase-panel .choice-grid{grid-template-columns:1fr}.login-showcase-panel .grade-grid{grid-template-columns:repeat(2,1fr)}}
 `;
 root.append(style);

 let state=await load();
 let draft=state.settings?structuredClone(state.settings):null;

 const target=e('section',undefined,'manager-card'),targetGrid=e('div',undefined,'settings-grid');
 const clientLabel=e('label','客户端'),client=document.createElement('select');clientLabel.append(client);
 targetGrid.append(clientLabel);target.append(e('h2','配置目标'),e('p','这里列出当前 Manager 已添加、并安装了登录展示组件的客户端。','muted'),targetGrid);
 root.append(target);

 const enableCard=e('section',undefined,'manager-card'),enableLabel=e('label',undefined,'switch-row'),enabled=document.createElement('input');
 enabled.type='checkbox';enabled.className='ui-switch';enableLabel.append(enabled,document.createTextNode('启用登录页面机体选择'));
 enableCard.append(e('h2','功能开关'),enableLabel,e('p','关闭后保留原游戏选中的登录机体。保存后需要重新启动客户端。','muted'));root.append(enableCard);

 const modeCard=e('section',undefined,'manager-card'),modes=e('fieldset',undefined,'choice-grid');
 modes.append(e('legend','选择方式'));
 for(const [value,title,description] of [
  ['grades','按等级选择','从勾选的等级中轮换展示。'],
  ['all','所有机体','从当前已安装并登记到图鉴的全部有效机体中选择。'],
  ['recent','最近注册','按原生图鉴登记编号从新到旧选择指定数量。'],
  ['units','指定机体','按名称或编号搜索并建立明确名单。'],
 ]){
  const label=e('label',undefined,'choice-card'),input=document.createElement('input');
  input.type='radio';input.name='login-showcase-mode';input.value=value;
  label.append(input,e('strong',title),e('span',description,'muted'));modes.append(label);
 }
 modeCard.append(e('h2','登录机体来源'),modes);root.append(modeCard);

 const gradeCard=e('section',undefined,'manager-card mode-options'),gradeGrid=e('div',undefined,'grade-grid');
 const gradeControls={};
 for(const grade of gradeOrder){
  const label=e('label'),input=document.createElement('input');input.type='checkbox';input.value=grade;
  label.append(input,document.createTextNode(grade));gradeGrid.append(label);gradeControls[grade]=input;
 }
 gradeCard.append(e('h2','展示等级'),e('p','默认选择 SS、SR、SU，不包含普通 S。可以自由组合 C、B、A、S 的基础、S、R、U 子等级。','muted'),gradeGrid);
 root.append(gradeCard);

 const recentCard=e('section',undefined,'manager-card mode-options'),recentLabel=e('label','展示最近登记的机体数量'),recent=document.createElement('input');
 recent.type='number';recent.min='1';recent.max='20000';recent.step='1';recentLabel.append(recent);
 recentCard.append(e('h2','最近注册范围'),recentLabel,e('p','“最近”按客户端原生机体图鉴的登记编号倒序计算。','muted'));root.append(recentCard);

 const unitsCard=e('section',undefined,'manager-card mode-options'),unitTools=e('div',undefined,'unit-tools'),search=document.createElement('input');
 search.type='search';search.placeholder='搜索机体名称、编号或等级';
 const selectedCount=e('span',undefined,'muted'),unitList=e('div',undefined,'unit-list'),unitHint=e('p',undefined,'muted');
 unitTools.append(search,selectedCount);unitsCard.append(e('h2','指定机体'),unitTools,unitList,unitHint);root.append(unitsCard);

 const previewCard=e('section',undefined,'manager-card'),summary=e('p'),preview=e('div',undefined,'preview-grid');
 previewCard.append(e('h2','当前候选池'),summary,preview);root.append(previewCard);

 function snapshot(){return JSON.stringify(draft);}

 function mode(){
  return root.querySelector('input[name=login-showcase-mode]:checked')?.value||'grades';
 }

 function readControls(){
  if(!draft)return;
  draft.enabled=enabled.checked;
  draft.mode=mode();
  draft.grades=gradeOrder.filter(grade=>gradeControls[grade].checked);
  draft.recent_count=Number(recent.value);
 }

 function selectedUnits(){
  const byId=new Map((state.catalog||[]).map(item=>[item.id,item]));
  return draft.unit_ids.map(id=>byId.get(id)).filter(Boolean);
 }

 function candidates(){
  const catalog=state.catalog||[];
  if(draft.mode==='grades'){const wanted=new Set(draft.grades);return catalog.filter(item=>wanted.has(item.grade));}
  if(draft.mode==='recent')return catalog.slice(0,Number.isInteger(draft.recent_count)?draft.recent_count:0);
  if(draft.mode==='units')return selectedUnits();
  return catalog;
 }

 function renderPreview(){
  if(!draft)return;
  const pool=candidates();summary.textContent=`当前已安装图鉴 ${state.catalog_count||0} 台，按此配置将有 ${pool.length} 台候选机体。`;
  preview.replaceChildren(...pool.slice(0,24).map(item=>e('span',`${item.grade} · ${item.name} · ${item.id}`,'preview-item')));
  if(!pool.length)preview.append(e('p','当前组合没有可展示的机体。','ui-alert is-warning'));
  else if(pool.length>24)preview.append(e('span',`另有 ${pool.length-24} 台未在此预览中展开`,'muted'));
 }

 function renderUnitList(){
  if(!draft)return;
  const query=search.value.trim().toLocaleLowerCase('zh-CN'),selected=new Set(draft.unit_ids);
  const matches=(state.catalog||[]).filter(item=>!query||item.name.toLocaleLowerCase('zh-CN').includes(query)||String(item.id).includes(query)||item.grade.toLowerCase()===query);
  const visible=[...new Map([
   ...selectedUnits().map(item=>[item.id,item]),
   ...matches.slice(0,200).map(item=>[item.id,item]),
  ]).values()];
  unitList.replaceChildren();
  for(const item of visible){
   const label=e('label',undefined,'unit-row'),input=document.createElement('input');input.type='checkbox';input.checked=selected.has(item.id);
   input.onchange=()=>{
    if(input.checked&&!selected.has(item.id))draft.unit_ids.push(item.id);
    if(!input.checked)draft.unit_ids=draft.unit_ids.filter(id=>id!==item.id);
    renderUnitList();renderPreview();
   };
   label.append(input,e('span',item.name),e('small',`${item.grade} · ${item.id}`,'muted'));unitList.append(label);
  }
  if(!visible.length)unitList.append(e('p','没有匹配的机体。','empty-state'));
  selectedCount.textContent=`已选 ${draft.unit_ids.length} 台`;
  unitHint.textContent=matches.length>200?`找到 ${matches.length} 台；当前显示已选机体和前 200 条搜索结果。`:`找到 ${matches.length} 台。`;
 }

 function renderMode(){
  if(!draft)return;
  gradeCard.hidden=draft.mode!=='grades';recentCard.hidden=draft.mode!=='recent';unitsCard.hidden=draft.mode!=='units';
  renderPreview();
 }

 function renderState(){
  client.replaceChildren(...(state.clients||[]).map(item=>new Option(item.name,item.id)));
  if(state.host_id)client.value=state.host_id;
  if(!draft){
   for(const card of [enableCard,modeCard,gradeCard,recentCard,unitsCard,previewCard])card.hidden=true;
   target.append(e('p',state.selection_error||'没有可配置的客户端。','ui-alert is-warning'));
   return;
  }
  for(const card of [enableCard,modeCard,previewCard])card.hidden=false;
  enabled.checked=draft.enabled;
  const radio=root.querySelector(`input[name=login-showcase-mode][value="${draft.mode}"]`);
  if(radio)radio.checked=true;
  for(const grade of gradeOrder)gradeControls[grade].checked=draft.grades.includes(grade);
  recent.value=draft.recent_count;
  renderMode();renderUnitList();
 }

 async function load(hostId){
  const suffix=hostId?'?host_id='+encodeURIComponent(hostId):'';
  return json(await ctx.fetch('/api/login-showcase'+suffix));
 }

 for(const radio of root.querySelectorAll('input[name=login-showcase-mode]')){
  radio.onchange=()=>{readControls();renderMode();};
 }
 enabled.onchange=()=>{readControls();renderPreview();};
 recent.oninput=()=>{readControls();renderPreview();};
 for(const input of Object.values(gradeControls))input.onchange=()=>{readControls();renderPreview();};
 search.oninput=renderUnitList;

 const bar=saveBar(root,ctx,'登录页面机体展示',[{
  title:'启用状态、选择方式和候选范围',
  snapshot,
  jump:()=>modeCard.scrollIntoView({behavior:'smooth'}),
 }],[{
  label:'保存登录展示配置',primary:true,covers:[0],run:async()=>{
   readControls();
   state=await ctx.request('/api/login-showcase',{
    host_id:state.host_id,
    settings:draft,
    revisions:state.revisions,
    catalog_revision:state.catalog_revision,
   });
   draft=structuredClone(state.settings);renderState();
   ctx.message('登录页面机体展示配置已保存。重新启动客户端后生效。');
  },
 }],'保存后需要重新启动客户端。');
 root.append(bar);

 client.onchange=async()=>{
  const previous=state.host_id;
  if(bar.unsaved().length&&!confirm('当前客户端有未保存的登录展示修改。放弃修改并切换客户端？')){
   client.value=previous||'';return;
  }
  try{
   state=await load(client.value);draft=state.settings?structuredClone(state.settings):null;search.value='';renderState();bar.markSaved();
  }catch(error){client.value=previous||'';ctx.message(error.message,true);}
 };

 renderState();
}
