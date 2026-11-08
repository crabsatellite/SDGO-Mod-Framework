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
 const gitSource=field('Manager 与默认模组程序源（JSON；官方 Release 或 Git SSH/HTTPS）',JSON.stringify(upstream.framework_source,null,2),true);program.append(gitSource.wrap,element('p','玩家的游戏框架与 Mod 版本由服主配置锁定。这个来源用于 Manager 更新，以及服主审阅和选择基础程序包。','muted'),button('导入程序源配置',run(async()=>{await request('/api/updates',{action:'source',source:JSON.parse(gitSource.input.value)});message('已保存程序源；Git 来源复用本机已有身份。');}),'ui-btn is-blue'));
 if(upstream.program_managed_by_client){program.replaceChildren(element('h2','Manager 程序上游'),element('p','这个 Manager 随服主选择的模组版本更新，避免独立升级与整合包修复互相覆盖。DIY 维护者可以明确接管后审阅上游。','muted'));}
 const runtimeCard=element('section',undefined,'manager-card');
 runtimeCard.append(element('h2','服主配置与内容更新'),element('p','在兼容的原版客户端上安装服主选定的 Mod、共享参数和自定义内容。配置源支持 HTTPS、Git SSH/HTTPS 和离线目录；完整客户端仓库不是必需条件。','muted'));
 runtimeCard.append(element('p','安装配套的新版本 Manager 与框架后，绑定的玩家双击 GOnline.exe 即会显示更新进度，校验程序及服主内容后自动打开游戏。','muted'));
 const runtimeRows=element('div');runtimeCard.append(runtimeRows);root.prepend(runtimeCard);
 let runtimeDisposed=false,runtimeFingerprint='';
 async function refreshRuntime(){
  const state=await request('/api/client-updates');if(runtimeDisposed)return;
  const fingerprint=JSON.stringify(state);if(fingerprint===runtimeFingerprint)return;runtimeFingerprint=fingerprint;
  runtimeRows.replaceChildren();
  if(state.hosts.some(h=>h.source)){
   const automatic=element('input');automatic.type='checkbox';automatic.checked=state.automatic;
   automatic.onchange=run(async()=>{await request('/api/services/manager.client_updates',{action:automatic.checked?'start':'stop'});});
   const label=element('label');label.append(automatic,document.createTextNode('Manager 运行期间自动同步模组及服主内容；游戏运行时等待关闭'));runtimeRows.append(label);
  }
  for(const host of state.hosts){
   const row=element('div',undefined,'manager-card'),r=host.result||{};
   const downloading=['queued','working'].includes(r.status);
   row.append(element('h3',host.name),element('p',host.source?.label||host.error||'粘贴服主提供的配置地址，或导入离线配置文件','muted'));
   const sourceInput=field('服主配置地址（HTTPS 或 Git 地址）',(host.source?.schema_version===3?host.source.feeds||[]:host.pending?.feeds||[]).join('\n'),true),offline=field('或离线 server-profile.json 的本地路径',host.pending?.offline||'');
   sourceInput.input.disabled=downloading;offline.input.disabled=downloading;
   const bind=button('一键安装／跟随服主配置',run(async()=>{bind.disabled=true;try{const result=await request('/api/client-updates',{action:'profile_install',host:host.id,feeds:lines(sourceInput.input.value),offline:offline.input.value.trim()||null});const cancelled=result.hosts.find(h=>h.id===host.id)?.result?.status==='cancelled';message(cancelled?'导入已取消，可继续。':'已绑定服主配置，正在从程序源和内容源统一安装。');await refreshRuntime();}finally{bind.disabled=false;}}),'ui-btn is-primary');bind.disabled=downloading;
   if(host.mode!=='owner')row.append(sourceInput.wrap,offline.wrap,bind);
    if(r.progress){
     const p=r.progress,phases={manifest:'检查锁定配置',reuse:'校验本地可复用内容',mirror:'核对备用源的同一内容',content:'下载并校验服主内容',content_archive:'下载服主内容包',framework:'下载并校验模组包',staging:'暂存并完整校验',install:'安装已校验文件'};
     const total=p.total_bytes||0,received=p.verified_bytes===undefined?(p.current_bytes||0):p.verified_bytes;
     const bar=element('progress');bar.style.width='100%';bar.style.accentColor='#2d8bd2';bar.max=total||p.total_files||1;
     if(total)bar.value=Math.min(total,received);else if(p.phase==='install')bar.value=p.verified_files||0;
     const size=n=>(n/1048576).toFixed(1)+' MiB';
     row.append(element('p',(phases[p.phase]||'处理中')+(total?' · '+(p.verified_bytes!==undefined?'已校验 ':'已接收 ')+size(received)+' / '+size(total):'')+(p.verified_files!==undefined?' · '+(['reuse','install'].includes(p.phase)?'已处理 ':'已校验 ')+p.verified_files+' / '+p.total_files+' 个文件':''),'muted'),bar);
     if(p.wire_bytes)row.append(element('p','当前连接已接收 '+size(p.wire_bytes)+'（Git 压缩传输，完成后校验）','muted'));
     if(p.file)row.append(element('p',p.file.split('/').slice(-2).join('/'),'muted'));
    }
    if(downloading)row.append(button('取消本次更新',run(async()=>{await request('/api/client-updates',{action:'cancel',host:host.id});message('正在取消；保留已校验下载，缺失文件不会安装。');await refreshRuntime();}),'ui-btn is-ghost'));
    if(host.source&&['cancelled','error'].includes(r.status))row.append(button('继续下载并安装',run(async()=>{await request('/api/client-updates',{action:'sync',host:host.id});await refreshRuntime();}),'ui-btn is-primary'));
    if(!host.source&&r.message)row.append(element('p',r.message,r.status==='error'?'error':'muted'));
   if(host.source){
    if(host.source.framework)row.append(element('p','框架源：'+host.source.framework.repository+' · '+host.source.framework.channel+'；内容源：'+host.source.label,'muted'));
    if(r.framework_version)row.append(element('p','已检查框架 '+r.framework_version+' · 内容提交 '+r.commit.slice(0,12),'muted'));
    if(host.recovery_required)row.append(button('恢复未完成更新',run(async()=>{await request('/api/client-updates',{action:'recover',host:host.id});await refreshRuntime();}),'ui-btn is-blue'));
    const busy=['queued','working'].includes(r.status),check=button('检查配置与内容更新',run(async()=>{await request('/api/client-updates',{action:'check',host:host.id});await refreshRuntime();}),'ui-btn is-blue');check.disabled=busy;row.append(check);
    if(r.status==='available'){
     const details=element('details');details.append(element('summary','查看更新与退役文件'),element('pre',[...(r.files||[]),...(r.retired||[]).map(n=>'退役：'+n)].join('\n')));
     row.append(element('p',r.changed_files+' 个文件需要更新，'+r.retired_files+' 个旧文件将保留到恢复目录；保留 '+(r.preserved_settings||0)+' 项已有配置。','muted'),details,button('确认安装更新',run(async()=>{await request('/api/client-updates',{action:'install',host:host.id,digest:r.digest});await refreshRuntime();}),'ui-btn is-primary'));
    }
    if(r.status==='restart_required')row.append(button('重启 Manager 完成客户端更新',run(async()=>{await request('/api/client-updates',{action:'restart',host:host.id});message('正在交接，更新完成后 Manager 会重新打开。');}),'ui-btn is-primary'));
    const label={queued:'已排队',working:'正在校验和同步',cancelled:'已取消，可继续下载',current:'已安装的 Mod、服主配置和内容已核对',installed:'服主配置已安装并核验',recovered:'原文件已恢复',waiting:'等待游戏关闭',error:'更新未完成，未接受不完整内容',restart_required:'等待重启 Manager'}[r.status]||'';
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
  const h=data.hosts.find(h=>h.id===chosen);
  if(h.complete_runtime&&h.mode!=='owner'){
   card.append(element('p','此目录由上方的配置与内容更新统一同步。共享程序、资源和 Mod 跟随同一个发布者。','muted'),
    button('接管为服主 DIY',run(async()=>{await request('/api/owner-updates',{action:'configure',host:h.id,mode:'owner'});await reload();message('已接管维护；自动运行文件更新会保留此目录。');}),'ui-btn is-blue'));
   return;
  }
  const modes=element('select');modes.className='ui-input';modes.style.marginTop='12px';
  for(const [id,title]of [['starter','基础整合包（默认）：接受上游运行更新'],['owner','服主 DIY：自行接管维护与发布'],...(h.role==='client'?[['player','玩家：仅跟随指定负责人']]:[])]){const o=element('option',title);o.value=id;modes.append(o);}
  modes.value=h.mode;card.append(modes,element('p','DIY 和玩家目录不会被程序上游自动覆盖。明确切回基础模式会解除原负责人绑定；已有文件不会因此立即改变。','muted'));
  card.append(element('p','默认跟随基础整合包。服主需要自行维护时，明确选择「服主 DIY」并保存；已绑定负责人的玩家继续跟随该负责人。','muted'),button('保存维护方式',run(async()=>{if(modes.value==='player'){message('请在上方导入服主配置。');return;}await request('/api/owner-updates',{action:'configure',host:h.id,mode:modes.value});await reload();message('维护边界已保存。');}),'ui-btn is-blue'));
  if(h.mode==='owner'){
   const review=element('div');card.append(element('h3','审阅上游组件'),element('p','先在上方检查上游版本，再预览差异。只采用你勾选的组件，已声明的独立配置仍保留；选中的程序和脚本将使用上游版本。','muted'),button('预览上游差异',run(async()=>{
    const plan=await request('/api/updates',{action:'runtime_plan',host:h.id});review.replaceChildren();const selected=new Set();
    const apply=button('采用所选组件',run(async()=>{const r=await request('/api/updates',{action:'adopt',host:h.id,components:[...selected],digest:plan.digest});message(r.status==='waiting'?'先关闭这个目录对应的程序，再重新审阅采用。':'已采用所选上游组件。');await reload();}),'ui-btn is-primary');apply.disabled=true;
    for(const c of plan.components){const details=element('details'),summary=element('summary'),box=element('input');box.type='checkbox';box.onchange=()=>{box.checked?selected.add(c.id):selected.delete(c.id);apply.disabled=!selected.size;};summary.append(box,document.createTextNode(' '+c.title+' · '+c.files.length+' 个文件'));details.append(summary,element('pre',c.files.join('\n')));review.append(details);}
    if(!plan.components.length)review.append(element('p','上游文件与当前目录一致。','muted'));else review.append(apply);
   }),'ui-btn is-blue'),review);
   const profileDirectory=field('配置档导出／发布目录（与客户端分开）',h.profile_publication?.directory||''),profileFeeds=field('玩家读取配置档的地址（HTTPS、Git 或离线 local+file 地址）',(h.profile_publication?.feeds||[]).join('\n'),true),profileRemote=field('Git 发布地址（可选；留空则仅导出）',h.profile_publication?.git_remote||''),profileBranch=field('Git 内容分支',h.profile_publication?.branch||'content'),profileExtras=field('额外共享内容的客户端相对路径（每行一项）',(h.profile_publication?.extras||[]).join('\n'),true);
   card.append(element('h3','发布服主配置与内容'),element('p','先在程序上游检查版本。导出包记录选定 Mod 的精确版本、共享配置与生成素材；不含框架 DLL、原版游戏、SQL、密码、名单或玩家数据。目录可交给现有 HTTPS／Git 发布流程，离线使用目录中的清单。','muted'),profileDirectory.wrap,profileFeeds.wrap,profileRemote.wrap,profileBranch.wrap,profileExtras.wrap,button('保存并导出服主配置档',run(async()=>{const result=await request('/api/client-updates',{action:'profile_publish',host:h.id,directory:profileDirectory.input.value.trim(),feeds:lines(profileFeeds.input.value),extras:lines(profileExtras.input.value),git_remote:profileRemote.input.value.trim(),branch:profileBranch.input.value.trim()});message('已导出配置修订 '+result.revision+'，'+result.files+' 个内容文件；框架 '+result.framework+'。');}),'ui-btn is-primary'));
   if(h.complete_runtime)card.append(element('p','完整客户端发布由这套整合包的维护流程提供。DIY 修改在发布之前只影响本机；此处可以审阅采用上游 Mod 组件。','muted'));


  }
  resultNode=element('p',undefined,'muted');lastResult='';card.append(resultNode);result(h);
 }
 draw();
 const timer=setInterval(async()=>{if(disposed)return;try{const latest=await request('/api/owner-updates');result(latest.hosts.find(h=>h.id===chosen));}catch(e){if(resultNode)resultNode.textContent=e.message;}},2000);
 return ()=>{disposed=true;runtimeDisposed=true;clearInterval(timer);clearInterval(runtimeTimer);};
}
