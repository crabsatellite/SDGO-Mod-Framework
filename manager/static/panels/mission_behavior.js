import {e,button,download,saveBar} from '/panel_helpers.js';

async function json(response){
 if(response instanceof Response){const value=await response.json();if(!response.ok)throw Error(value.error||'请求失败');return value;}
 return response;
}

function asBase64(bytes){
 let value='';
 for(let offset=0;offset<bytes.length;offset+=32768)value+=String.fromCharCode(...bytes.subarray(offset,offset+32768));
 return btoa(value);
}

export async function mount(root,ctx){
 let state=await json(await ctx.fetch('/api/mission-behavior'));
 const intro=e('section',undefined,'manager-card');
 intro.append(
  e('h2','任务行为版本'),
  e('p','OC 是默认版本，恢复已经核实的早期任务行为；未核实或 OC 没有数据的模式继续使用 G改现有行为。当前 39 个任务都会保留，奖励和倍率不在这里修改。','muted')
 );
 root.append(intro);

 const targetCard=e('section',undefined,'manager-card'),grid=e('div',undefined,'settings-grid');
 targetCard.append(e('h2','同步目标'),e('p','一次应用会更新客户端和所选服务端组内的全部战斗频道。不能只更新其中一个频道。','muted'),grid);
 const clientLabel=e('label','客户端'),client=document.createElement('select');clientLabel.append(client);
 const groupLabel=e('label','服务端组'),group=document.createElement('select');groupLabel.append(group);
 grid.append(clientLabel,groupLabel);root.append(targetCard);

 const profileCard=e('section',undefined,'manager-card'),profiles=e('fieldset',undefined,'choice-grid');
 profiles.append(e('legend','行为版本'));
 for(const [value,title,description] of [
  ['oc','OC（默认）','使用已核实的 OC 任务行为；缺失部分沿用 G改。'],
  ['gco','G改（现有）','使用服务器和客户端目前的原生任务数据。'],
 ]){
  const label=e('label',undefined,'choice-card'),input=document.createElement('input');input.type='radio';input.name='mission-profile';input.value=value;
  label.append(input,e('strong',title),e('span',description,'muted'));profiles.append(label);
 }
 profileCard.append(e('h2','选择行为'),profiles);root.append(profileCard);

 const statusCard=e('section',undefined,'manager-card'),status=e('div');
 statusCard.append(e('h2','配置状态'),status);root.append(statusCard);

 const dataCard=e('section',undefined,'manager-card'),file=document.createElement('input');file.type='file';file.accept='.bin,application/octet-stream';
 const importButton=button('导入服主任务配置',async()=>{
  const selected=file.files[0];if(!selected)throw Error('请选择服主提供的 profile.bin');
  const bytes=new Uint8Array(await selected.arrayBuffer());
  state=await ctx.request('/api/mission-behavior/import',{
   profile:asBase64(bytes),expected_revision:state.manager_bundle.revision,
  });
  const wanted=selection();file.value='';
  state=await ctx.request('/api/mission-behavior/preview',wanted);
  render();ctx.message('任务配置已导入。选择行为并应用后，重启客户端和全部战斗频道生效。');
 },ctx,'ui-btn is-blue');
 dataCard.append(
  e('h2','服主任务配置'),
  e('p','OC 需要服主提供的任务配置。配置只包含行为差异和版本校验，不包含数据库、账号或原版资源。整合包已经配好时无需再次导入。','muted'),
  file,importButton
 );
 root.append(dataCard);

 function selectedProfile(){return root.querySelector('input[name=mission-profile]:checked')?.value||'oc';}
 function selection(){
  const selectedGroup=state.battle_groups.find(item=>item.id===group.value);
  return {client_host:client.value||null,battle_hosts:selectedGroup?.host_ids||[],profile:selectedProfile()};
 }
 async function preview(){
  state=await ctx.request('/api/mission-behavior/preview',selection());renderStatus();return state;
 }
 function optionList(select,items,value,label){
  select.replaceChildren(new Option('请选择…',''),...items.map(item=>new Option(label(item),item.id)));
  select.value=value||'';
 }
 function renderStatus(){
  status.replaceChildren();
  if(state.selection_error)status.append(e('p',state.selection_error,'ui-alert is-warning'));
  const source=state.source_bundle.ready
   ?'OC 配置已就绪 · '+state.source_bundle.metadata.sha256.slice(0,12)
   :'尚无有效 OC 配置';
  status.append(e('p',source,state.source_bundle.ready?'ui-alert is-success':'ui-alert is-warning'));
  for(const target of state.targets){
   const role=target.role==='client'?'客户端':'战斗频道';
   const profile=target.profile==='oc'?'OC':target.profile==='gco'?'G改':'配置无效';
   const suffix=(target.enabled?'':' · 组件已关闭')+(target.running?' · 正在运行，应用前请关闭':'');
   status.append(e('p',role+' · '+target.name+' · '+profile+suffix,'muted'));
  }
  if(state.targets.length&&(!state.consistent.profile||!state.consistent.bundle))
   status.append(e('p','两端当前配置不一致。应用会按所选版本统一写入整组。','ui-alert is-warning'));
 }
 function render(){
  optionList(client,state.clients,state.selection.client_host,item=>item.name);
  optionList(group,state.battle_groups,state.selection.battle_group,item=>item.label);
  const radio=root.querySelector('input[name=mission-profile][value="'+state.selection.profile+'"]');
  if(radio)radio.checked=true;
  renderStatus();
 }
 render();

 for(const input of [client,group,...root.querySelectorAll('input[name=mission-profile]')])
  input.addEventListener('change',()=>preview().catch(error=>ctx.message(error.message,true)));

 const bar=saveBar(root,ctx,'任务行为',[{
  title:'客户端、服务端组和行为版本',
  snapshot:()=>JSON.stringify(selection()),
  jump:()=>profileCard.scrollIntoView({behavior:'smooth'}),
 }],[{
  label:'应用到客户端和全部战斗频道',primary:true,covers:[0],run:async()=>{
   await preview();
   state=await ctx.request('/api/mission-behavior/apply',{
    ...selection(),expected_revision:state.transaction_revision,
   });
   render();ctx.message('任务行为已统一保存。重新启动客户端和全部战斗频道后生效。');
  },
 },{
  label:'导出玩家配置包',covers:[],run:async()=>{
   await preview();
   const response=await ctx.request('/api/mission-behavior/export-client',{
    ...selection(),expected_revision:state.transaction_revision,
   });
   await download(response,'SDGO-mission-behavior-client.zip');
  },
 }],'保存后需要重启客户端和全部战斗频道。');
 root.append(bar);
}
