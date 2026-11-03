import {icon, enhance, chip, led, applyArt} from '/ui.js';
const $=id=>document.getElementById(id);
const ROLES={client:'客户端',battle:'对战服务器',agent:'中继服务器',world:'世界服务器'};
const OWNER_KEY='sdgo-manager-owner-tools';
let state, token, hostId, panelCleanup=()=>{}, activePanel=null, archiveView=false, navigation=0;
function element(tag, text, cls){const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(cls)node.className=cls;return node;}
function message(text,error=false){$('manager-message').textContent=text;$('manager-message').classList.toggle('error',error);}
async function request(path,body,method='POST',extra={}){
 const response=await fetch(path,body===undefined?{}:{method,headers:{'Content-Type':'application/json','X-ModManager-Token':token,...extra},body:JSON.stringify(body)});
 const type=response.headers.get('Content-Type')||'';
 if(!type.includes('json')){if(!response.ok)throw Error('请求失败');return response;}
 const result=await response.json();if(!response.ok)throw Error(result.error||'请求失败');return result;
}
function button(text,fn,cls=''){const b=element('button',text,cls);b.onclick=()=>Promise.resolve().then(fn).catch(e=>message(e.message,true));return b;}
function clear(){panelCleanup();panelCleanup=()=>{};$('content').replaceChildren();message('');}
function selectedHost(){return state.hosts.find(h=>h.id===hostId)||state.hosts[0];}
function developer(){return state.mode==='developer';}
// Owner tools (shop, operators, rewards, background work) only matter once a server is managed.
function hasServer(){return state.hosts.some(h=>h.role!=='client');}
function ownerTools(){return developer()||hasServer()||localStorage.getItem(OWNER_KEY)==='1';}
function accept(value){state=value;hostId=selectedHost()?.id;renderFrame();}
function renderFrame(){
 const dev=developer(),owner=ownerTools();
 $('change-mode').textContent=dev?'收起开发信息':'显示开发信息';$('eyebrow').textContent=dev?'开发调试':'服主与维护';
 $('host-select').replaceChildren(...state.hosts.map(h=>{const o=new Option(h.name+' · '+(dev?h.role:ROLES[h.role]),h.id);o.selected=h.id===hostId;return o;}));
 $('host-select').hidden=!state.hosts.length;
 $('nav-mods').classList.toggle('selected',activePanel===null&&!archiveView);$('nav-archive').classList.toggle('selected',activePanel===null&&archiveView);$('nav-services').classList.toggle('selected',activePanel==='services');
 $('nav-updates').classList.toggle('selected',activePanel==='updates');
 $('provider-nav').replaceChildren();for(const panel of state.panels||[]){const item=button(panel.title,()=>showProvider(panel.id),'nav-item ui-nav-item'+(activePanel===panel.id?' selected':''));item.prepend(icon(panel.id));$('provider-nav').append(item);}
 $('owner-group').hidden=!owner;$('owner-show').hidden=owner;$('owner-hide').hidden=dev||hasServer();
 document.body.classList.toggle('is-developer',dev);$('ui-kit-link').hidden=!dev;enhance();
}
function supportActions(context){
 const box=element('section',undefined,'support-box'),actions=element('div',undefined,'actions ai-actions');
 box.append(element('h3','遇到问题？'),element('p','复制一份不含密码的说明，交给 DeepSeek 等 AI 帮你配置、排查，或整理成 Bug 报告。','muted'));
 for(const [kind,label] of [['configure','复制给 AI 配置 / 排查'],['bug','发现 Bug，复制报告']]){const support=button(label,async()=>{const value=await request('/api/ai-context',{...context,kind});try{await navigator.clipboard.writeText(value.text);message('已复制，可以粘贴给 DeepSeek 或其他 AI。');}catch{const d=document.createElement('dialog'),t=document.createElement('textarea');t.value=value.text;d.append(element('h2','给 AI 的说明'),t,button('关闭',()=>d.close()));document.body.append(d);d.showModal();t.select();}},'quiet');support.prepend(icon(kind==='bug'?'bug':'copy'));actions.append(support);}
 box.append(actions);return box;
}
function importButton(host){
 const file=document.createElement('input');file.type='file';file.accept='.zip';file.hidden=true;
 file.onchange=async()=>{try{const bytes=new Uint8Array(await file.files[0].arrayBuffer());let text='';for(let i=0;i<bytes.length;i+=8192)text+=String.fromCharCode(...bytes.subarray(i,i+8192));const result=await request('/api/import',{host:host.id,package:btoa(text)});accept(result.state);showMods(archiveView);message('配置包已导入，用原来的方式重新打开游戏即可。');}catch(error){message(error.message,true);}};
 const importer=button('导入配置包',()=>file.click(),'ui-btn is-ghost is-small');importer.title='导入服主发给你的客户端配置包（ZIP）';importer.prepend(icon('import'));
 const wrap=element('span');wrap.append(file,importer);return wrap;
}
function categorySwitches(host){
 const box=element('details',undefined,'manager-card batch-switches');box.append(element('summary','批量开关'));
 box.append(element('p','一次关闭这个目录里的全部游戏扩展或联机功能，单个功能的开关保持不变。','muted'));
 const row=element('div',undefined,'batch-row');
 for(const [key,title] of [['gameplay','全部游戏扩展'],['network','全部联机功能']]){const label=element('label',title),input=document.createElement('input');input.type='checkbox';input.className='ui-switch';input.checked=host.categories[key];
  input.onchange=async()=>{try{accept(await request('/api/category',{host:host.id,category:key,enabled:input.checked,revision:host.global_revision}));showMods();message('已保存，重新打开对应游戏或服务器后生效。');}catch(e){input.checked=!input.checked;message(e.message,true);}};label.prepend(input);row.append(label);}
 box.append(row);if(!host.categories.gameplay||!host.categories.network)box.open=true;return box;
}
function showMods(isArchive=false){
 ++navigation;
 archiveView=isArchive===true;activePanel=null;renderFrame();clear();$('page-title').textContent=archiveView?'修复归档':'游戏功能';const root=$('content'),host=selectedHost();document.querySelector('.host-tools').hidden=false;
 if(!host){const blank=element('section',undefined,'empty-state'),art=element('div',undefined,'ui-art');applyArt(art,'squad');blank.append(art,element('small','欢迎使用 SDGO','eyebrow ui-eyebrow'),element('h2','从你的游戏目录开始'),element('p','选择已经安装好的客户端，就能看到可用的 Mod。游戏仍按原来的方式启动。'),button('选择游戏目录',openPath,'ui-btn is-primary is-large'));root.append(blank);return;}
 const listed=archiveView?host.archive:host.mods,enabled=listed.filter(mod=>mod.enabled).length;
 const summary=element('section',undefined,'hangar-summary'),info=element('div',undefined,'summary-info');
 info.append(element('strong',host.name),chip(ROLES[host.role]||host.role,'blue'),element('span',archiveView?listed.length+' 项自动修复':'已启用 '+enabled+' / '+listed.length,'muted'));
 summary.append(info);if(host.role==='client'&&!archiveView)summary.append(importButton(host));root.append(summary);
 root.append(element('p',archiveView?'这些修复随框架自动生效，不需要设置。排查问题时可以查看说明；如需临时关闭，先点左下角“显示开发信息”。':'打开或关闭功能后，重新启动对应的游戏或服务器生效。','intro'));
 const grid=element('div',undefined,'mod-grid');
 let order=0;
 for(const mod of listed){
  const card=element('section',undefined,'mod-card ui-boot'+(mod.enabled?'':' is-off')+(mod.blocked_by.length?' is-blocked':'')),top=element('div',undefined,'mod-top'),name=element('div');card.style.setProperty('--i',order++);
  name.append(element('h2',mod.title));if(developer())name.append(element('span',mod.id+(mod.version?' · v'+mod.version:''),'mod-code'));top.append(name);
  const toggle=document.createElement('input');toggle.type='checkbox';toggle.className='ui-switch';toggle.checked=mod.enabled;toggle.ariaLabel=(mod.enabled?'关闭 ':'启用 ')+mod.title;toggle.title=mod.enabled?'已启用':'已关闭';
  const locked=archiveView&&!developer();
  toggle.onchange=async()=>{try{const next=await request('/api/toggle',{host:host.id,mod:mod.id,enabled:toggle.checked,revision:{global:host.global_revision,manifest:mod.revision}});accept(next);showMods(archiveView);message('已保存，重新启动对应游戏或服务器后生效。');}catch(e){toggle.checked=!toggle.checked;message(e.message,true);}};
  top.append(locked?chip(mod.enabled?'自动生效':'已关闭',mod.enabled?'green':undefined):toggle);card.append(top,element('p',mod.description||'暂无说明。'));
  const foot=element('div',undefined,'mod-foot'),notes=element('div',undefined,'mod-meta');
  if(mod.blocked_by.length)notes.append(chip('已被批量开关关闭','amber'));
  if(!mod.redistributable)notes.append(chip('仅本地 · 禁止导出','amber'));
  const configurable=!archiveView&&mod.settings_count>0;
  const open=button(configurable?'设置':'说明',()=>showSettings(host.id,mod.id),configurable?'ui-btn is-blue is-small':'ui-btn is-ghost is-small');open.prepend(icon(configurable?'settings':'archive'));
  foot.append(notes,open);card.append(foot);grid.append(card);
 }
 if(!listed.length)root.append(element('section',archiveView?'这个目录没有需要归档的自动修复。':'这个目录还没有安装可管理的功能。可以导入服主给你的配置包，或添加你的服务器目录。','empty-state'));
 else root.append(grid);
 if(!archiveView&&listed.length)root.append(categorySwitches(host));
}
async function showSettings(host,mod){
 const data=await request('/api/settings?'+new URLSearchParams({host,mod}));clear();$('page-title').textContent=data.contract.title;
 const current=selectedHost(),entry=[...(current?.mods||[]),...(current?.archive||[])].find(m=>m.id===mod);
 const root=$('content');const back=button(archiveView?'返回修复归档':'返回功能列表',()=>showMods(archiveView),'back');back.prepend(icon('back'));root.append(back);
 const meta=element('div',undefined,'mod-meta detail-meta');if(entry){meta.append(chip(entry.enabled?'已启用':'已关闭',entry.enabled?'green':undefined));if(entry.version)meta.append(chip('版本 '+entry.version));}if(developer())meta.append(element('code',mod,'field-help'));root.append(meta);
 root.append(element('p',data.contract.description||'调整此功能的设置。','intro'));
 if(data.contract.provider&&ownerTools()){const panel=(state.panels||[]).find(p=>p.id===data.contract.provider);if(panel){const link=button('打开服主管理：'+panel.title,()=>showProvider(panel.id),'ui-btn is-ghost is-small');link.prepend(icon(panel.id));root.append(link);}}
 const box=element('section',undefined,'manager-card settings-plate'),fields=element('div',undefined,'settings-grid'),controls={};
 let restart=false;
 for(const field of data.contract.settings){const label=element('label',field.label),input=document.createElement(field.type==='choice'?'select':'input');input.dataset.key=field.id;
  if(field.type==='choice')for(const option of field.options)input.append(new Option(option,option));
  if(field.type!=='choice')input.type=field.type==='boolean'?'checkbox':['integer','number'].includes(field.type)?'number':'text';if(field.type==='boolean')input.className='ui-switch';
  if(field.type==='boolean')input.checked=data.values[field.id];else input.value=data.values[field.id]??'';
  if(field.minimum!==undefined)input.min=field.minimum;if(field.maximum!==undefined)input.max=field.maximum;
  if(field.type==='number')input.step='any';label.append(input);
  if(field.activation==='hot_reload')label.append(element('span','保存后游戏运行中即可生效','field-help'));else restart=true;
  if(field.audience==='developer')label.classList.add('is-developer-field');
  if(developer())label.append(element('code',field.file+' · '+field.key,'field-help'));fields.append(label);controls[field.id]=input;
 }
 if(data.contract.settings.length){
  box.append(fields);const actions=element('div',undefined,'save-row');
  actions.append(button('保存设置',async()=>{const values={};for(const field of data.contract.settings){const c=controls[field.id];values[field.id]=field.type==='boolean'?c.checked:['integer','number'].includes(field.type)?Number(c.value):c.value;}const result=await request('/api/settings',{host,mod,values,revisions:data.revisions});data.revisions=result.revisions;message(restart?'已保存。重新启动对应游戏或服务器后生效。':'已保存。');},'primary'));
  if(restart)actions.append(element('span','保存后需重新启动对应游戏或服务器','muted'));box.append(actions);
 }else box.append(element('p',data.contract.presentation==='automatic_fix'?'这项修复自动工作，不需要设置。':'这个功能没有需要填写的设置，打开开关即可使用。','muted'));
 root.append(box,supportActions({host,mod}));
}
async function showProvider(id){
 const sequence=++navigation;const config=await request('/api/providers/'+id+'/config');if(sequence!==navigation)return;clear();activePanel=id;renderFrame();$('page-title').textContent=config.title;document.querySelector('.host-tools').hidden=true;
 const root=$('content'),card=element('section',undefined,'manager-card'),details=element('details');details.open=!config.configured;details.append(element('summary',config.configured?'目录与配置位置':'先选择游戏目录与现有配置'));
 const grid=element('div',undefined,'settings-grid'),inputs={};for(const [key,title] of Object.entries(config.fields)){if(key==='demo'&&!developer())continue;const label=element('label',title),input=document.createElement('input'),line=element('div',undefined,'path-row');input.value=config.config[key]||'';line.append(input);label.append(line);if(key!=='demo')line.append(button('浏览…',async()=>{const value=await request('/api/pick',{kind:['server_ini','rules','previous','catalog','patterns','lookup','profile','module_binary','symbol_fragment','cover'].includes(key)?'file':'directory'});if(value.path)input.value=value.path;},'quiet'));grid.append(label);inputs[key]=input;}
 details.append(grid,button('保存位置设置',async()=>{const data=Object.fromEntries(Object.entries(inputs).map(([k,v])=>[k,v.value.trim()]));await request('/api/providers/'+id+'/config',data);await showProvider(id);},'primary'));card.append(details);root.append(card);
 if(id==='rewards'&&config.configured){const actions=element('div',undefined,'actions');for(const [action,title] of [['start','启动钱包服务'],['stop','停止钱包服务']])actions.append(button(title,async()=>{const value=await request('/api/services/rewards.wallet',{action});message(value.error|| (value.running?'钱包服务正在运行':'钱包服务已停止'));}));actions.append(button('检查安装 / 更新',async()=>{const value=await request('/api/providers/rewards/api/rewards/install-plan',{});const blocked=value.plan.blockers||[];message(blocked.length?'需要先处理：'+blocked.join('；'):'检查通过。请确认选中的对战服务器已关闭。');if(blocked.length)return;card.append(button('按检查结果安装',async()=>{if(!confirm('按刚才检查的计划安装到选中的服务器？不会停止其他游戏服务。'))return;await request('/api/providers/rewards/api/rewards/install',{review_id:value.review_id});message('安装完成。启动对应服务器后，再查看面板运行状态。');},'primary'));}));card.append(actions);}
 if(id==='operators'&&config.configured)card.append(button('更新本机客户端的副官数据',async()=>{const current=await request('/api/providers/operators/api/operators');await request('/api/providers/operators/api/operators/update-client',{},'POST',{'X-Catalog-Revision':current.revision});message('副官数据已更新，重新打开这个客户端后生效。');}));
 if(!config.configured){root.append(element('p','先选择已有配置的位置。这一步不会导入或重置数据库。','intro'),supportActions({provider:id}));return;}
 const mount=element('section',undefined,'provider-root');root.append(mount);
 const timers=new Set(),listeners=[];const ctx={token,request:(path,body,method='POST',headers={})=>request('/api/providers/'+id+path,body,method,headers),fetch:async(path,options={})=>{const headers=new Headers(options.headers);headers.set('X-ModManager-Token',token);return fetch(String(path).startsWith('/api/providers/')?path:'/api/providers/'+id+path,{...options,headers});},setInterval:(fn,ms)=>{const t=setInterval(fn,ms);timers.add(t);return t;},onWindow:(name,fn)=>{window.addEventListener(name,fn);listeners.push([name,fn]);},message};
 message('正在读取…');const module=await import('/panels/'+id+'.js');if(sequence!==navigation)return;const dispose=await module.mount(mount,ctx);if(sequence===navigation){message('');mount.dataset.ready='1';root.append(supportActions({provider:id}));}
 panelCleanup=()=>{timers.forEach(clearInterval);listeners.forEach(([name,fn])=>window.removeEventListener(name,fn));if(dispose)dispose();};
}
async function openPath(){try{const value=await request('/api/pick',{kind:'directory'});if(!value.path)return;accept(await request('/api/hosts',{path:value.path}));showMods();}catch(error){message(error.message,true);$('path-title').textContent='添加客户端或服务器目录';$('host-path').value='';$('path-dialog').showModal();}}
async function showServices(){
 ++navigation;activePanel='services';clear();renderFrame();$('page-title').textContent='后台任务';document.querySelector('.host-tools').hidden=true;
 const data=await request('/api/services'),root=$('content');
 root.append(element('p','服主功能的后台服务由 Manager 统一启动和停止。退出 Manager 后服务会停止，下次打开时自动恢复已启用的任务。','intro'));
 if(!data.services.length&&!data.resources.length)root.append(element('section','目前没有后台任务。','empty-state'));
 for(const item of data.services){
  const card=element('section',undefined,'manager-card'),title=element('h2',item.title,'service-title');title.prepend(led(item.running?(item.healthy===false?'warn':'on'):(item.error?'error':'off')));
  card.append(title,element('p',(item.running?(item.healthy===false?'运行中，等待健康回报':'正在运行'):'已停止')+(item.desired?' · 打开 Manager 时自动恢复':''),'muted'));
  if(item.error)card.append(element('p',item.error,'error'));
  card.append(button(item.desired?'停止任务':'启动任务',async()=>{await request('/api/services/'+item.id,{action:item.desired?'stop':'start'});await showServices();},'primary'));
  if(developer())card.append(element('code',item.id+' · owner='+item.owner,'field-help'));
  root.append(card);
 }
 if(developer())for(const item of data.resources){const card=element('section',undefined,'manager-card');card.append(element('h2',item.id==='shop.bridge'?'商城业务通道':'内部业务通道'),element('p',item.running?'按需运行，由 Manager 管理':'已停止','muted'));root.append(card);}
}
$('nav-services').onclick=()=>showServices().catch(e=>message(e.message,true));
async function showUpdates(){
 ++navigation;activePanel='updates';clear();renderFrame();$('page-title').textContent='更新';document.querySelector('.host-tools').hidden=true;
 const root=$('content'),card=element('section',undefined,'manager-card');root.append(card);
 let pending=false;
 const refresh=async()=>{if(pending)return;pending=true;try{
  const u=await request('/api/updates');card.replaceChildren(element('h2','版本更新'),element('p','Stable 提供正式功能；Beta 同时更新正式与测试功能。游戏或服务器运行时先下载，关闭后自动安装。','intro'));
  const channels=element('div',undefined,'actions');
  for(const [id,label] of [['stable','只更新 Stable（默认）'],['beta','更新 Beta 功能']]){const b=button(label,async()=>{await request('/api/updates',{action:'channel',channel:id});await refresh();},u.channel===id?'ui-btn is-primary':'ui-btn is-ghost');b.setAttribute('aria-pressed',String(u.channel===id));channels.append(b);}
  const auto=element('input');auto.type='checkbox';auto.className='ui-switch';auto.checked=u.automatic;auto.onchange=async()=>{try{await request('/api/updates',{action:'automatic',enabled:auto.checked});}catch(e){message(e.message,true)}};
  const autoLabel=element('label',undefined,'update-auto');autoLabel.append(auto,document.createTextNode('自动检查并安装更新'));card.append(channels,autoLabel);
  card.append(element('p','Manager '+u.current_version+(u.latest_version?' · 可用版本 '+u.latest_version:''),'muted'),element('p',u.message,'update-status'));
  if(u.error)card.append(element('p',u.error,'error'));
  const actions=element('div',undefined,'actions');actions.append(button('检查更新',async()=>{await request('/api/updates',{action:'check'});},'ui-btn is-blue'));
  if(u.latest_version)actions.append(button('立即更新',async()=>{await request('/api/updates',{action:'install'});},'ui-btn is-primary'));
  if(u.manager_restart)actions.append(button('重启 Manager 完成更新',async()=>{await request('/api/updates',{action:'restart'});message('Manager 正在重启，新页面会自动打开。');},'ui-btn is-primary'));
  card.append(actions);
  for(const h of u.hosts||[])card.append(element('p',h.name+' · '+({installed:'已更新',waiting:'等待关闭',newer:'已安装较新版本',error:'暂未更新'}[h.status]||h.status)+(h.message?'：'+h.message:''),h.status==='error'?'error':'muted'));
  if(u.notes){const details=element('details');details.append(element('summary','版本说明'),element('pre',u.notes,'release-notes'));card.append(details);}
  card.append(element('p','保留 Mod 开关、自定义参数、机体资源和本地数据。Beta 通道不会安装开发诊断工具。','muted'));enhance();
 }finally{pending=false;}};
 await refresh();const timer=setInterval(()=>refresh().catch(e=>message(e.message,true)),3000);panelCleanup=()=>clearInterval(timer);
}
$('nav-updates').onclick=()=>showUpdates().catch(e=>message(e.message,true));
$('create-entrypoints').onclick=async()=>{try{const result=await request('/api/entrypoints',{});message(`已创建 ${result.created} 个快捷入口，保留 ${result.kept} 个已有入口。`);}catch(e){message(e.message,true);}};
$('nav-mods').onclick=()=>showMods(false);$('nav-archive').onclick=()=>showMods(true);$('host-select').onchange=()=>{hostId=$('host-select').value;showMods(archiveView);};$('add-host').onclick=openPath;
$('owner-show').onclick=()=>{localStorage.setItem(OWNER_KEY,'1');renderFrame();};
 $('owner-hide').onclick=()=>{localStorage.removeItem(OWNER_KEY);if(activePanel!==null){showMods(false);}else renderFrame();};
$('confirm-host').onclick=async event=>{event.preventDefault();try{accept(await request('/api/hosts',{path:$('host-path').value.trim()}));$('path-dialog').close();showMods();}catch(e){message(e.message,true);}};
$('change-mode').onclick=async()=>{try{accept(await request('/api/mode',{mode:state.mode==='standard'?'developer':'standard'}));if(activePanel==='updates')await showUpdates();else if(activePanel==='services'&&ownerTools())await showServices();else if(activePanel&&activePanel!=='services'&&ownerTools())await showProvider(activePanel);else showMods(archiveView);}catch(e){message(e.message,true);}};
$('exit-manager').onclick=async()=>{if(!confirm('请先保存正在编辑的内容。退出管理器？游戏不会关闭。'))return;try{await request('/api/exit',{});$('content').replaceChildren(element('section','管理器已退出，可以关闭本页。','empty-state'));}catch(e){message(e.message,true);}};
try{const initial=await request('/api/state');token=initial.session_token;accept(initial);showMods();}catch(e){message(e.message,true);}
