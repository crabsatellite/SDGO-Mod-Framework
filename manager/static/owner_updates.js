// The same Manager owns program updates, owner publication and player bindings.
export async function mountOwnerUpdates(ctx){
 const {root,request,element,button,message}=ctx;
 const lines=value=>value.split(/\r?\n/).map(s=>s.trim()).filter(Boolean);
 const field=(label,value='',multiline=false)=>{const input=element(multiline?'textarea':'input');input.value=value;input.className='ui-input';if(multiline){input.rows=3;input.style.width='100%';}const wrap=element('label',undefined,'field');wrap.style.display='grid';wrap.style.gap='6px';wrap.style.marginBottom='12px';wrap.append(element('span',label),input);return {wrap,input};};
 const run=fn=>async()=>{try{await fn();}catch(e){message(e.message,true);}};
 const program=element('section',undefined,'manager-card');
 const upstream=await request('/api/updates');
 const repo=field('Manager 程序上游（GitHub：owner/repository）',upstream.repository);
 program.append(element('h2','程序上游'),element('p','基础整合包可作为起点。进入 DIY 后，配置、脚本、素材与发布版本由服主维护；切换程序上游不会解除玩家绑定。','muted'),repo.wrap,
  button('保存程序上游',run(async()=>{await request('/api/updates',{action:'source',repository:repo.input.value.trim()});message('已保存程序上游，维护方式与玩家绑定保持不变。');}),'ui-btn is-blue'));
 root.append(program);
 const runtimeCard=element('section',undefined,'manager-card');
 runtimeCard.append(element('h2','完整客户端更新'),element('p','由 Manager 校验并同步原版程序、资源归档和配套 Mod。登录与画面设置、仅本地内容和数据库保留；服主 DIY 目录需要自行审阅维护。','muted'));
 const runtimeRows=element('div');runtimeCard.append(runtimeRows);root.prepend(runtimeCard);
 let runtimeDisposed=false;
 async function refreshRuntime(){
  const state=await request('/api/client-updates');if(runtimeDisposed)return;
  runtimeRows.replaceChildren();
  if(state.hosts.some(h=>h.source)){
   const automatic=element('input');automatic.type='checkbox';automatic.checked=state.automatic;
   automatic.onchange=run(async()=>{await request('/api/services/manager.client_updates',{action:automatic.checked?'start':'stop'});});
   const label=element('label');label.append(automatic,document.createTextNode('Manager 运行期间自动同步完整客户端；游戏运行时等待关闭'));runtimeRows.append(label);
  }
  for(const host of state.hosts){
   const row=element('div',undefined,'manager-card'),r=host.result||{};
   row.append(element('h3',host.name),element('p',host.source?.label||host.error||'由服主提供的客户端包配置完整更新来源','muted'));
   if(host.source){
    if(host.recovery_required)row.append(button('恢复未完成更新',run(async()=>{await request('/api/client-updates',{action:'recover',host:host.id});await refreshRuntime();}),'ui-btn is-blue'));
    const busy=['queued','working'].includes(r.status),check=button('检查完整客户端更新',run(async()=>{await request('/api/client-updates',{action:'check',host:host.id});await refreshRuntime();}),'ui-btn is-blue');check.disabled=busy;row.append(check);
    if(r.status==='available')row.append(element('p',r.changed_files+' 个文件需要更新，'+r.retired_files+' 个旧文件将保留到恢复目录。','muted'),button('确认更新完整客户端',run(async()=>{await request('/api/client-updates',{action:'install',host:host.id,digest:r.digest});await refreshRuntime();}),'ui-btn is-primary'));
    if(r.status==='restart_required')row.append(button('重启 Manager 完成客户端更新',run(async()=>{await request('/api/client-updates',{action:'restart',host:host.id});message('正在交接，更新完成后 Manager 会重新打开。');}),'ui-btn is-primary'));
    const label={queued:'已排队',working:'正在校验和同步',current:'完整客户端已是当前版本',installed:'完整客户端已同步并核验',recovered:'原文件已恢复',waiting:'等待游戏关闭',error:'更新未完成',restart_required:'等待重启 Manager'}[r.status]||'';
    row.append(element('p',label+(r.message?' · '+r.message:'')+(r.commit?' · '+r.commit.slice(0,8):''),r.status==='error'?'error':'muted'));
   }
   runtimeRows.append(row);
  }
 }
 await refreshRuntime();
 const runtimeTimer=setInterval(()=>refreshRuntime().catch(e=>message(e.message,true)),2000);
 const card=element('section',undefined,'manager-card');root.append(card);
 let data=await request('/api/owner-updates'),chosen=ctx.hostId||data.hosts[0]?.id,disposed=false;
 let resultNode=null,lastResult='';
 const statusNames={queued:'已排队',working:'处理中',published:'公开发布已核验',available:'有可用版本',installed:'已同步',waiting:'等待客户端关闭',error:'操作未完成'};
 function result(row){const r=row?.result||{};if(!resultNode)return;const key=JSON.stringify(r);if(key===lastResult)return;lastResult=key;resultNode.textContent=(statusNames[r.status]||'尚未执行')+(r.revision?' · 整合包版本 '+r.revision:'')+(r.message?' · '+r.message:'');resultNode.className=r.status==='error'?'error':'muted';}
 async function reload(){data=await request('/api/owner-updates');if(!disposed)draw();}
 function draw(){
  card.replaceChildren(element('h2','整合包维护与服主发布'));
  if(!data.hosts.length){card.append(element('p','先添加客户端或服务端目录，再选择维护方式。','muted'));return;}
  if(!data.hosts.some(h=>h.id===chosen))chosen=data.hosts[0].id;
  const select=element('select');select.className='ui-input';
  for(const h of data.hosts){const o=element('option',h.name+' · '+({starter:'基础整合包',owner:'服主 DIY',player:'跟随服主'}[h.mode]||h.mode));o.value=h.id;select.append(o);}
  select.value=chosen;select.onchange=()=>{chosen=select.value;draw();};card.append(select);
  const h=data.hosts.find(h=>h.id===chosen),modes=element('select');modes.className='ui-input';modes.style.marginTop='12px';
  for(const [id,title]of [['starter','基础整合包（默认）：接受上游运行更新'],['owner','服主 DIY：自行接管维护与发布'],...(h.role==='client'?[['player','玩家：仅跟随指定负责人']]:[])]){const o=element('option',title);o.value=id;modes.append(o);}
  modes.value=h.mode;card.append(modes,element('p','DIY 和玩家目录不会被程序上游自动覆盖。明确切回基础模式会解除原负责人绑定；已有文件不会因此立即改变。','muted'));
  card.append(element('p','默认跟随基础整合包。服主需要自行维护时，明确选择「服主 DIY」并保存；已绑定负责人的玩家继续跟随该负责人。','muted'),button('保存维护方式',run(async()=>{if(modes.value==='player'){message('请在下方校验并绑定负责人更新地址。');return;}await request('/api/owner-updates',{action:'configure',host:h.id,mode:modes.value});await reload();message('维护边界已保存。');}),'ui-btn is-blue'));
  if(h.role==='client'){
   const feeds=field('负责人更新地址（HTTPS 清单或 git+https://仓库.git#分支，每行一个）',(h.source?.feeds||[]).join('\n'),true),bindingBox=element('div');
   card.append(element('h3','跟随负责人'),element('p','私有 Git 源复用本机已有的 Git HTTPS 凭据；不在绑定文件中保存服主令牌。可填写同一负责人的备用源。','muted'),feeds.wrap,bindingBox);
   let resolved=null;const autoBind=element('input');autoBind.type='checkbox';autoBind.checked=true;const autoBindLabel=element('label');autoBindLabel.append(autoBind,document.createTextNode('绑定后自动跟随（Manager 运行期间）'));card.append(autoBindLabel);
   card.append(button('查看发布者',run(async()=>{resolved=await request('/api/owner-updates',{action:'resolve',feeds:lines(feeds.input.value)});bindingBox.replaceChildren(element('p',resolved.source.label+' · 整合包版本 '+resolved.revision+' · 发布者 '+resolved.source.publisher_id.slice(0,12)),button('确认绑定此负责人',run(async()=>{await request('/api/owner-updates',{action:'configure',host:h.id,mode:'player',source:resolved.source,automatic:autoBind.checked});await reload();message('已绑定，后续运行文件仅从该负责人获取。');}),'ui-btn is-primary'));}),'ui-btn is-blue'));
   if(h.mode==='player'){
    card.append(element('p','当前负责人：'+h.source.label+'；已安装整合包版本：'+(h.installed_revision||'尚未安装'),'muted'));
    const actions=element('div',undefined,'actions');
    for(const [action,label]of [['check','检查负责人更新'],['install','同步负责人版本']])actions.append(button(label,run(async()=>{await request('/api/owner-updates',{action,host:h.id});}),'ui-btn is-blue'));
    const auto=element('input');auto.type='checkbox';auto.checked=h.automatic;auto.onchange=run(async()=>{await request('/api/owner-updates',{action:'automatic',host:h.id,enabled:auto.checked});message('自动跟随设置已保存。');});const autoLabel=element('label');autoLabel.append(auto,document.createTextNode('自动检查并同步负责人版本'));actions.append(autoLabel);
    card.append(actions,element('p','负责人源不可用时保留当前版本；不会改用官方整合包。私人登录、画面设置和仅本地 Mod 不随服主包分发。','muted'));
   }
  }
  if(h.mode==='owner'){
   const review=element('div');card.append(element('h3','审阅上游组件'),element('p','先在上方检查上游版本，再预览差异。只采用你勾选的组件，已声明的独立配置仍保留；选中的程序和脚本将使用上游版本。','muted'),button('预览上游差异',run(async()=>{
    const plan=await request('/api/updates',{action:'runtime_plan',host:h.id});review.replaceChildren();const selected=new Set();
    const apply=button('采用所选组件',run(async()=>{const r=await request('/api/updates',{action:'adopt',host:h.id,components:[...selected],digest:plan.digest});message(r.status==='waiting'?'先关闭这个目录对应的程序，再重新审阅采用。':'已采用所选上游组件。');await reload();}),'ui-btn is-primary');apply.disabled=true;
    for(const c of plan.components){const details=element('details'),summary=element('summary'),box=element('input');box.type='checkbox';box.onchange=()=>{box.checked?selected.add(c.id):selected.delete(c.id);apply.disabled=!selected.size;};summary.append(box,document.createTextNode(' '+c.title+' · '+c.files.length+' 个文件'));details.append(summary,element('pre',c.files.join('\n')));review.append(details);}
    if(!plan.components.length)review.append(element('p','上游文件与当前目录一致。','muted'));else review.append(apply);
   }),'ui-btn is-blue'),review);
   if(h.role==='client'){
    const p=h.publication||{},label=field('整合包名称',p.label||''),directory=field('专用发布目录（与游戏目录分开）',p.directory||''),urls=field('玩家读取地址（HTTPS 清单或 git+https://仓库.git#分支，每行一个）',(p.feeds||[]).join('\n'),true),remote=field('Git 发布仓库（HTTPS，可留空用于现有静态站点）',p.git_remote||''),branch=field('Git 分支',p.branch||'main'),extras=field('额外共享素材（客户端相对路径，每行一项）',(p.extras||[]).join('\n'),true),preview=element('div');
    card.append(element('h3','发布你维护的整合包'),element('p','只打包框架、公开 Mod、共享配置及声明的生成素材；原版游戏、账号、数据库、生日名单和仅本地 Mod 留在本机。静态发布目录应由现有 HTTPS 站点托管；填写 Git 仓库时，Manager 使用已有 Git 凭据提交推送。','muted'));
    for(const x of [label,directory,urls,remote,branch,extras])card.append(x.wrap);
    card.append(button('保存发布设置',run(async()=>{await request('/api/owner-updates',{action:'configure',host:h.id,mode:'owner',publication:{label:label.input.value.trim(),directory:directory.input.value.trim(),feeds:lines(urls.input.value),git_remote:remote.input.value.trim(),branch:branch.input.value.trim(),extras:lines(extras.input.value)}});await reload();message('已保存，下一步预览实际文件。');}),'ui-btn is-blue'));
    card.append(button('预览发布内容',run(async()=>{const plan=await request('/api/owner-updates',{action:'preview',host:h.id});const details=element('details');details.append(element('summary','查看 '+plan.file_count+' 个文件'),element('pre',Object.keys(plan.files).join('\n')));preview.replaceChildren(element('p',plan.file_count+' 个文件，'+(plan.bytes/1048576).toFixed(1)+' MiB；排除 '+plan.excluded_mods.length+' 个本地或诊断 Mod。'),details,button('确认发布并核验公开下载',run(async()=>{await request('/api/owner-updates',{action:'publish',host:h.id,digest:plan.digest});}),'ui-btn is-primary'));}),'ui-btn is-blue'),preview,
     button('重试推送／核对已有发布',run(async()=>{await request('/api/owner-updates',{action:'verify_publish',host:h.id});}),'ui-btn is-ghost'));
   }
  }
  resultNode=element('p',undefined,'muted');lastResult='';card.append(resultNode);result(h);
 }
 draw();
 const timer=setInterval(async()=>{if(disposed)return;try{const latest=await request('/api/owner-updates');result(latest.hosts.find(h=>h.id===chosen));}catch(e){if(resultNode)resultNode.textContent=e.message;}},2000);
 return ()=>{disposed=true;runtimeDisposed=true;clearInterval(timer);clearInterval(runtimeTimer);};
}
