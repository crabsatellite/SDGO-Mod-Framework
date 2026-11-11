import {download} from '/panel_helpers.js';
import {createAbilitiesEditor,renderDeveloperProtocol,renderPackPreview} from '/panels/operator_protocol_ui.js';

export async function mount(root,ctx){
 const shadow=root.attachShadow({mode:'open'});
 shadow.innerHTML=`<style>
:host{color-scheme:dark;--bg:var(--ui-hull-900,#10151e);--panel:var(--ui-hull-800,#1b2330);--line:var(--ui-line-strong,#334054);--muted:var(--ui-muted,#9fadc2);--text:var(--ui-text,#eaf0fa);--accent:var(--ui-gold,#7ed9bd)}
*{box-sizing:border-box}.module-body{color:var(--text);font:14px/1.5 "Segoe UI","Microsoft YaHei",sans-serif}button,input,select,textarea{font:inherit}
button,.btn{cursor:pointer;border:1px solid var(--line);border-radius:8px;padding:9px 13px;background:#263348;color:var(--text);text-decoration:none}
button:hover,.btn:hover{border-color:var(--accent)}button.primary{background:var(--accent);color:#0e2921;font-weight:600}button:disabled{opacity:.4;cursor:default}
input,select,textarea{border:1px solid var(--line);border-radius:7px;background:#111a27;color:var(--text);padding:9px;width:100%;min-width:0}
input[type=checkbox],input[type=radio]{width:auto}.connection-line{margin-bottom:16px}.top{display:flex;justify-content:space-between;align-items:start;gap:15px;margin-bottom:18px}
.top h1{display:none}.top p,p{margin:5px 0;color:var(--muted)}.actions,.inline{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.spread{justify-content:space-between}
.layout{display:grid;grid-template-columns:285px minmax(0,1fr);gap:20px}.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px}.list{padding:14px;align-self:start;position:sticky;top:var(--mm-sticky-top,150px)}
.listbox{height:min(660px,calc(100vh - var(--mm-sticky-top,150px) - 110px));overflow:auto;margin-top:10px}.op{display:flex;width:100%;align-items:center;gap:10px;border:0;background:transparent;text-align:left;margin:3px 0;padding:10px}
.op.selected{background:#2a4248;box-shadow:inset 3px 0 var(--accent)}.op img{width:42px;height:48px;object-fit:contain;background:#111a27;border-radius:5px}.op span{min-width:0;overflow:hidden;text-overflow:ellipsis}.op small{display:block;color:var(--muted)}
.editor{padding:24px}.identity{display:grid;grid-template-columns:130px minmax(0,1fr);gap:24px}.portrait{height:155px;width:130px;object-fit:contain;background:#101923;border-radius:10px;border:1px solid var(--line)}
label.field{display:block;margin-bottom:15px;color:#c7d2e2}label.field>span,.compact-field{display:block;margin-bottom:5px}.form-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.span-2{grid-column:span 2}
.note{color:var(--muted);font-size:12px;margin-top:7px}.section{margin-top:24px;padding-top:20px;border-top:1px solid var(--line)}h2{font-size:16px;margin:0 0 12px}h3{font-size:14px;margin:0 0 8px}
.section-heading{display:flex;justify-content:space-between;gap:16px;align-items:start;margin-bottom:12px}.modes{display:grid;grid-template-columns:1fr 1fr;gap:12px}.mode{border:1px solid var(--line);padding:13px;border-radius:9px;cursor:pointer}
.mode:has(input:checked){border-color:var(--accent);background:#213b38}.mode strong{display:block;margin:5px 0}.mode small{color:var(--muted)}
.rules{display:grid;gap:9px}.rule{display:grid;grid-template-columns:minmax(150px,1fr) 95px 80px 80px 42px;gap:9px;align-items:center}.rule-head{font-size:12px;color:var(--muted)}.totals{display:flex;justify-content:space-between;color:var(--muted);padding:12px 0}
.footer{display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap;margin-top:24px;padding-top:20px;border-top:1px solid var(--line);position:sticky;bottom:0;background:var(--panel);z-index:2}
#status{min-height:24px;margin:10px 0;color:var(--accent)}.error{color:#ffb0a9!important}.badge{font-size:12px;color:var(--accent);padding:3px 8px;border:1px solid #41635a;border-radius:20px}
.gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));gap:9px;max-height:420px;overflow:auto;margin-top:12px}.gallery button{padding:8px}.gallery img{height:80px;max-width:95px;object-fit:contain}.gallery small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.hidden{display:none!important}.notice{border:1px solid var(--line);border-radius:8px;padding:10px 12px;margin:10px 0;background:#111a27}.warning{border-color:#8b7241;color:#ffe4a3}.mono-wrap,.protocol-json{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.55 "Cascadia Mono",monospace}
.ability-grid,.protocol-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}.ability-card,.protocol-card,.protocol-doc{border:1px solid var(--line);border-radius:9px;padding:12px;background:#111923}
.ability-bonus-grid,.ability-extra-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}.ability-extra-grid{grid-template-columns:1fr 1fr;margin:12px 0}.ability-custom.disabled{opacity:.55}
.timed-list{display:grid;gap:8px}.timed-row{display:grid;grid-template-columns:115px 115px repeat(3,minmax(90px,1fr)) 42px;gap:8px;align-items:end}.ability-extensions{margin-top:12px}.ability-extensions textarea{margin-top:10px}
.protocol-facts{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin:12px 0}.protocol-fact{display:flex;justify-content:space-between;gap:14px;border-bottom:1px solid var(--line);padding:7px 0}.protocol-fact span{color:var(--muted)}.protocol-fact strong{overflow-wrap:anywhere;text-align:right}
.preview-heading{display:flex;gap:15px;align-items:center}.preview-heading .portrait{width:95px;height:115px}.pack-meta{margin-top:9px}.pack-meta code{overflow-wrap:anywhere}
dialog{background:var(--panel);color:var(--text);border:1px solid var(--line);border-radius:14px;width:min(960px,95vw);max-height:92vh;padding:22px}dialog::backdrop{background:#0009}.dialog-body{max-height:65vh;overflow:auto}.dialog-actions{display:flex;justify-content:flex-end;gap:8px;margin-top:18px}
@media(max-width:1050px){.layout{grid-template-columns:230px 1fr}.identity{grid-template-columns:100px 1fr}.portrait{width:100px}.rule{grid-template-columns:1fr 75px 65px 65px 36px}.ability-grid{grid-template-columns:1fr}.timed-row{grid-template-columns:1fr 1fr 1fr}}
@media(max-width:800px){.layout{display:block}.list{position:static;margin-bottom:15px}.listbox{height:210px}.editor{padding:16px}.modes,.form-grid,.ability-bonus-grid,.ability-extra-grid,.protocol-grid,.protocol-facts{grid-template-columns:1fr}.span-2{grid-column:auto}.rule{grid-template-columns:1fr 70px 60px 60px 32px}.top{flex-direction:column}.identity{gap:14px}.timed-row{grid-template-columns:1fr 1fr}}
</style>
<div class="module-body">
 <div class="connection-line"><span class="badge" id="connection">正在加载</span></div>
 <div class="top"><div><h1>副官管理</h1><p>设置独立外观、能力、结算掉落、发放方式和商城草稿预填。</p></div>
  <div class="actions">
   <button id="import-pack" class="primary" type="button">导入副官分享包</button>
   <button id="registrations" type="button">导出商品登记</button>
   <button id="package" type="button">生成客户端更新包</button>
  </div>
 </div>
 <div id="status" role="status"></div>
 <div class="layout">
  <aside class="panel list"><div class="inline"><input id="search" aria-label="搜索副官" placeholder="搜索名称或编号"><button id="new" title="基于当前副官创建新条目" type="button">＋</button></div><div class="listbox" id="list"></div></aside>
  <section class="panel editor">
   <div id="empty">选择一名副官开始编辑</div>
   <form id="form" class="hidden">
    <div class="identity">
     <div><img id="preview" class="portrait" alt="副官头像"><button type="button" id="change-avatar" style="width:130px;margin-top:8px">更换头像</button><button type="button" id="share" class="hidden" style="width:130px;margin-top:8px">导出 .opack</button></div>
     <div>
      <div class="inline spread" style="margin-bottom:12px"><small id="op-id"></small><span class="badge" id="managed"></span></div>
      <div class="form-grid">
       <label class="field span-2"><span>显示名称</span><input id="name" maxlength="48" required></label>
       <label class="field"><span>技能 / 动作模板编号</span><input id="template-id" type="number" min="60001" max="60999" required></label>
       <label class="inline"><input id="enabled" type="checkbox">启用此副官配置</label>
      </div>
      <div id="pack-meta" class="pack-meta hidden"></div>
      <p class="note">显示名称不覆盖玩家个人昵称。资源包外观编译到此副官自己的客户端命名空间，不覆盖模板原始资源。</p>
     </div>
    </div>
    <div class="section"><h2>发放方式</h2><div class="modes">
     <label class="mode"><input type="radio" name="delivery" value="all_players"><strong>所有玩家免费获得</strong><small>这是显式配置；导入分享包本身不会把商品上架。</small></label>
     <label class="mode"><input type="radio" name="delivery" value="product"><strong>注册为商品</strong><small>保存商品登记，由商城模块另行创建未上架草稿。</small></label>
    </div></div>
    <div class="section">
     <div class="section-heading"><div><h2>商城草稿预填</h2><p class="note">这里只保存预填值，不创建、上架或应用商城商品。</p></div><button id="open-shop" type="button">前往商城草稿</button></div>
     <div class="form-grid">
      <label class="field"><span>中文商品名称</span><input id="store-name" maxlength="50"></label>
      <label class="field"><span>中文销售标签</span><input id="store-label" maxlength="50"></label>
      <label class="field"><span>售价</span><input id="store-price" type="number" min="1"></label>
      <label class="field"><span>币种</span><select id="store-currency"><option value="">未预填</option><option value="GB">GB</option><option value="MB">MB</option></select></label>
      <label class="field"><span>数量 / 租赁天数</span><input id="store-validity" type="number" min="1"></label>
     </div>
    </div>
    <div id="abilities" class="section"></div>
    <div class="section">
     <div class="inline spread"><h2>结算掉落</h2><button type="button" id="add-rule">＋ 添加掉落物品</button></div>
     <p class="note">以原生副官掉落触发为一次。每次最多命中一条规则；物品池内等概率抽取，剩余概率不掉落。</p>
     <div class="rule rule-head"><span>物品</span><span>概率 %</span><span>最少数量</span><span>最多数量</span><span></span></div><div class="rules" id="rules"></div>
     <div class="totals"><span id="chance-total"></span><span id="drop-expected"></span><span id="no-drop"></span></div><datalist id="items"></datalist>
    </div>
    <div class="footer"><button type="button" id="disable">停用自定义配置</button><div class="actions"><button type="button" id="apply">应用到服务端</button><button class="primary" type="submit">保存配置草稿</button></div></div>
   </form>
  </section>
 </div>
 <section class="panel editor section" id="runtime-panel">
  <h2>服务端能力组件</h2>
  <p class="note">自定义经验和结算能力需要配套的服务端组件。安装前关闭所选战斗服务；安装与应用配置后重新启动，再读取运行状态。</p>
  <div class="actions"><button type="button" id="runtime-check">检查安装</button><button type="button" id="runtime-install" disabled>安装配套组件</button><button type="button" id="runtime-refresh">读取生效状态</button></div>
  <div id="runtime-state" class="notice" role="status">尚未读取服务端状态。</div>
 </section>
 <section id="developer-protocol" class="panel editor section hidden"></section>
 <dialog id="avatar-dialog"><div class="inline spread"><h2>选择副官外观</h2><button type="button" id="close-avatar">关闭</button></div><input type="search" id="avatar-search" placeholder="搜索已有头像或导入包"><label class="btn">上传 PNG 头像<input id="upload" type="file" accept="image/png" hidden></label><p class="note">原生头像保留模板配套图片；导入包保留完整 profile；PNG 按模板各位置尺寸生成。</p><div class="gallery" id="gallery"></div></dialog>
 <dialog id="import-dialog">
  <div class="inline spread"><div><h2>导入副官分享包</h2><p class="note">先预览资源、模板、能力和 fallback，再确认创建新的独立副官。</p></div><button id="close-import" type="button">关闭</button></div>
  <div class="actions"><label class="btn">选择 .opack / ZIP<input id="pack-file" type="file" accept=".opack,.zip,application/zip" hidden></label><button id="pack-path" type="button">从本机路径预览</button></div>
  <div id="import-preview" class="dialog-body"><p class="note">尚未选择分享包。</p></div>
  <div id="import-fields" class="form-grid hidden">
   <label class="field span-2"><span>新副官显示名称</span><input id="import-name" maxlength="48"></label>
   <label class="field"><span>技能 / 动作模板编号</span><input id="import-template" type="number" min="60001" max="60999"></label>
   <label class="field"><span>资源 profile 模板编号</span><input id="import-source" type="number" min="60001" max="60999"></label>
  </div>
  <div class="dialog-actions"><button id="confirm-import" class="primary" type="button" disabled>创建独立副官草稿</button></div>
 </dialog>
</div>`;

 const document=new Proxy(shadow,{get(target,key){if(key==='createElement')return globalThis.document.createElement.bind(globalThis.document);const value=target[key];return typeof value==='function'?value.bind(target):value;}});
 const $=id=>document.getElementById(id);
 let state=null,selected=null,draft=null,savedSnapshot='',protocol=null,abilityEditor=null,importResult=null;
 const providerUrl=path=>path?.startsWith('/api/providers/')?path:path?.startsWith('/api/')?'/api/providers/operators'+path:path;
 const message=(text,error=false)=>{ctx.message(text,error);$('status').textContent=text;$('status').classList.toggle('error',error);};
 async function api(path,method='GET',body){
  const headers={};
  if(method!=='GET'&&state?.revision)headers['X-Catalog-Revision']=state.revision;
  return ctx.request(path,body===undefined?undefined:body,method,headers);
 }
 async function binary(path,name,headers={}){
  const response=await ctx.fetch(path,{headers});
  if(!response.ok){let text='文件生成失败';try{text=(await response.json()).error||text;}catch{}throw new Error(text);}
  await download(response,name);
 }
 const avatarUrl=op=>{
  if(op.avatar.kind==='upload')return '/api/providers/operators/api/operators/upload/'+op.avatar.sha256;
  if(op.avatar.kind==='pack')return '/api/providers/operators/api/operators/packs/'+op.avatar.sha256+'/portrait';
  return '/api/providers/operators/api/operators/portrait/'+op.avatar.operator_id;
 };
 const editSnapshot=()=>JSON.stringify({
  selected,
  controls:[...$('form').querySelectorAll('input,select,textarea')].map(control=>[control.type,control.value,control.checked]),
  avatar:draft?.avatar,drops:draft?.drops,
 });
 const dirty=()=>!!draft&&savedSnapshot!==editSnapshot();
 const requireSaved=action=>{if(!dirty())return true;message('请先保存当前副官配置草稿，再'+action+'。',true);return false;};
 function runtimeState(value){
  const lines=[];
  if(!value.configured)lines.push('尚未配置服务端根目录和配套框架包目录。');
  else{
   lines.push(value.files_installed?'配套文件已安装。':'配套文件尚未对齐。');
   lines.push(value.database_applied?'当前配置已应用到数据库。':'当前配置尚未应用到数据库。');
   lines.push(value.running_effective?'已核实运行服务读取了当前配置。':value.restart_required?'重新启动所选战斗服务后，再读取生效状态。':'尚无当前配置已被运行服务读取的证据。');
  }
  for(const reason of value.reasons||[])lines.push(reason);
  $('runtime-state').textContent=lines.join(' ');
 }
 async function refreshRuntime(){runtimeState(await api('/api/operators/runtime-status'));}
 ctx.guard?.(()=>dirty()?'「副官 '+selected+'」有尚未保存的配置草稿。':'');

 function accept(data,id=selected){
  state=data;selected=id;renderList();
  if(id&&state.operators.some(op=>op.id===id))select(id,true);
  $('connection').textContent=data.capabilities.server_apply?'已连接宿主服务端':'离线配置模式';
  $('apply').disabled=!data.capabilities.server_apply;
  $('package').disabled=!data.capabilities.client_package;
 }
 function renderList(){
  if(!state)return;
  const query=$('search').value.trim().toLowerCase();$('list').replaceChildren();
  for(const op of state.operators){
   if(!String(op.id).includes(query)&&!op.name.toLowerCase().includes(query))continue;
   const button=document.createElement('button');button.type='button';button.className='op'+(selected===op.id?' selected':'');
   const image=document.createElement('img');image.src=avatarUrl(op);image.alt='';
   const text=document.createElement('span');text.textContent=op.name;
   const small=document.createElement('small');small.textContent=op.id+' · '+(!op.managed?'沿用原版':!op.enabled?'已停用':op.delivery==='all_players'?'全员免费':'商品登记');
   text.append(small);button.append(image,text);button.onclick=()=>select(op.id);$('list').append(button);
  }
 }
 function select(id,force=false){
  if(!force&&id===selected)return;
  if(!force&&id!==selected&&dirty()&&!globalThis.confirm('当前副官有未保存的修改。放弃修改并切换吗？'))return;
  const source=state.operators.find(op=>op.id===id);if(!source)return;
  selected=id;draft=structuredClone(source);$('empty').classList.add('hidden');$('form').classList.remove('hidden');
  $('preview').src=avatarUrl(draft);$('op-id').textContent='副官 '+id+' · 商品类别 '+draft.category_id;$('managed').textContent=draft.managed?'自定义配置':'沿用原版';
  $('name').value=draft.name;$('template-id').value=draft.template_id;$('enabled').checked=draft.enabled;
  document.querySelectorAll('input[name=delivery]').forEach(input=>input.checked=input.value===draft.delivery);
  const storefront=draft.storefront||{};$('store-name').value=storefront.name||'';$('store-label').value=storefront.label||'';$('store-price').value=storefront.price??'';$('store-currency').value=storefront.currency||'';$('store-validity').value=storefront.validity??'';
  abilityEditor.set(draft.abilities);renderRules();renderPackMeta();renderList();savedSnapshot=editSnapshot();
 }
 function renderPackMeta(){
  const box=$('pack-meta'),pack=draft.avatar.kind==='pack'?(state.imported_packs||{})[draft.avatar.sha256]:null;
  box.replaceChildren();box.classList.toggle('hidden',!pack);$('share').classList.toggle('hidden',!pack);
  if(!pack)return;
  const label=document.createElement('span');label.className='badge';label.textContent='分享包 · '+(pack.voice_files??0)+' 个语音文件 / '+(pack.voice_events??0)+' 个语音事件';
  const code=document.createElement('code');code.textContent=draft.avatar.sha256;box.append(label,code);
 }
 function total(){
  const sum=draft.drops.reduce((value,rule)=>value+Number(rule.chance_bps||0),0);
  const expected=draft.drops.reduce((value,rule)=>value+Number(rule.chance_bps||0)*(Number(rule.min_amount||0)+Number(rule.max_amount||0))/20000,0);
  $('chance-total').textContent='掉落合计 '+(sum/100).toFixed(2)+'%';$('drop-expected').textContent='每次触发期望 '+expected.toFixed(4)+' 件';$('no-drop').textContent='不掉落 '+Math.max(0,100-sum/100).toFixed(2)+'%';$('chance-total').classList.toggle('error',sum>10000);
 }
 function ruleInput(value,type,label){const input=document.createElement('input');input.type=type;input.value=value;input.setAttribute('aria-label',label);return input;}
 function renderRules(){
  $('rules').replaceChildren();
  draft.drops.forEach((rule,index)=>{
   const row=document.createElement('div');row.className='rule';
   const definition=state.items.find(item=>item.id===rule.category_id),pool=rule.pool&&(state.drop_pools||{})[rule.pool];
   const item=ruleInput(pool?pool.label+'（'+pool.category_ids.length+'种）':definition?definition.name+' · '+definition.id:rule.category_id,'text','掉落物品');item.setAttribute('list','items');
   item.onchange=()=>{const pooled=Object.entries(state.drop_pools||{}).find(([_key,value])=>item.value===value.label+'（'+value.category_ids.length+'种）');if(pooled){rule.pool=pooled[0];rule.category_id=pooled[1].category_ids[0];}else{delete rule.pool;const match=item.value.match(/ · (\d+)$/);rule.category_id=Number(match?match[1]:item.value.split(' ')[0]);}};
   const chance=ruleInput(rule.chance_bps/100,'number','掉落概率百分比');chance.min=0;chance.max=100;chance.step='.01';chance.oninput=()=>{rule.chance_bps=Math.round(Number(chance.value)*100);total();};
   const low=ruleInput(rule.min_amount,'number','最少数量');low.min=1;low.max=65535;low.onchange=()=>rule.min_amount=Number(low.value);
   const high=ruleInput(rule.max_amount,'number','最多数量');high.min=1;high.max=65535;high.onchange=()=>rule.max_amount=Number(high.value);
   const remove=document.createElement('button');remove.type='button';remove.textContent='×';remove.onclick=()=>{draft.drops.splice(index,1);renderRules();};
   row.append(item,chance,low,high,remove);$('rules').append(row);
  });total();
 }
 function renderGallery(){
  const query=$('avatar-search').value.toLowerCase();$('gallery').replaceChildren();
  for(const item of state.gallery){
   if(!item.name.toLowerCase().includes(query)&&!String(item.id).includes(query))continue;
   const button=document.createElement('button');button.type='button';const image=document.createElement('img');image.src=providerUrl(item.url);image.alt=item.name;const label=document.createElement('small');label.textContent=item.name;
   button.append(image,label);button.onclick=()=>{draft.avatar={kind:'native',operator_id:item.id};$('preview').src=avatarUrl(draft);renderPackMeta();$('avatar-dialog').close();};$('gallery').append(button);
  }
  for(const [sha,pack] of Object.entries(state.imported_packs||{})){
   const text=(pack.name||'导入资源包')+' '+sha;if(!text.toLowerCase().includes(query))continue;
   const button=document.createElement('button');button.type='button';const image=document.createElement('img');image.src='/api/providers/operators/api/operators/packs/'+sha+'/portrait';image.alt=pack.name||'导入资源包';const label=document.createElement('small');label.textContent=pack.name||sha.slice(0,12);
   button.append(image,label);button.onclick=()=>{draft.avatar={kind:'pack',sha256:sha,operator_id:pack.source_template};$('preview').src=avatarUrl(draft);renderPackMeta();$('avatar-dialog').close();};$('gallery').append(button);
  }
 }
 function optionalInteger(id,label){const raw=$(id).value.trim();if(!raw)return undefined;const value=Number(raw);if(!Number.isInteger(value)||value<1)throw new Error(label+'必须是正整数');return value;}
 function readForm(){
  const storefront={};for(const [id,key] of [['store-name','name'],['store-label','label']]){const value=$(id).value.trim();if(value)storefront[key]=value;}
  const price=optionalInteger('store-price','售价'),validity=optionalInteger('store-validity','数量或租赁天数');if(price!==undefined)storefront.price=price;if(validity!==undefined)storefront.validity=validity;if($('store-currency').value)storefront.currency=$('store-currency').value;
  return {
   name:$('name').value.trim(),enabled:$('enabled').checked,
   delivery:document.querySelector('input[name=delivery]:checked')?.value||'product',
   avatar:structuredClone(draft.avatar),drops:structuredClone(draft.drops),
   template_id:Number($('template-id').value),abilities:abilityEditor.read(),storefront,
  };
 }
 async function save(){
  const value=readForm();const data=await api('/api/operators/'+selected,'PUT',value);accept(data,selected);message('副官配置草稿已保存。客户端、服务端和商城仍需分别执行对应动作。');
 }
 function buildItems(){
  $('items').replaceChildren();
  for(const pool of Object.values(state.drop_pools||{})){const option=document.createElement('option');option.value=pool.label+'（'+pool.category_ids.length+'种）';$('items').append(option);}
  for(const item of state.items){if(item.id===1070001)continue;const option=document.createElement('option');option.value=item.name+' · '+item.id;$('items').append(option);}
 }
 const fileBase64=file=>new Promise((resolve,reject)=>{const reader=new FileReader();reader.onerror=()=>reject(new Error('读取分享包失败'));reader.onload=()=>resolve(String(reader.result).split(',',2)[1]||'');reader.readAsDataURL(file);});
 async function previewPack(body,label){
  importResult=await api('/api/operators/import-preview','POST',body);renderPackPreview($('import-preview'),importResult);
  const preview=importResult.preview||{};$('import-name').value=preview.name||label||'';$('import-template').value=preview.source_template||'';$('import-source').value=preview.source_template||'';
  $('import-fields').classList.remove('hidden');$('confirm-import').disabled=false;
 }

 $('search').oninput=renderList;$('avatar-search').oninput=renderGallery;
 $('change-avatar').onclick=()=>{renderGallery();$('avatar-dialog').showModal();};$('close-avatar').onclick=()=>$('avatar-dialog').close();
 $('add-rule').onclick=()=>{const item=state.items.find(value=>value.id!==1070001);if(!item){message('当前目录没有可用掉落物品。',true);return;}draft.drops.push({category_id:item.id,chance_bps:1000,min_amount:1,max_amount:1});renderRules();};
 $('form').onsubmit=event=>{event.preventDefault();save().catch(error=>message(error.message,true));};
 $('disable').onclick=async()=>{try{if(!requireSaved('停用配置'))return;accept(await api('/api/operators/'+selected,'DELETE',{}),selected);message('自定义配置已停用，已发放内容保留。');}catch(error){message(error.message,true);}};
 $('new').onclick=async()=>{try{if(dirty()&&!globalThis.confirm('当前修改未保存。放弃修改并新建吗？'))return;const data=await api('/api/operators/new','POST',{template_id:selected||60001});accept(data,data.selected_id);message('已创建独立副官草稿，默认登记为商品且不会自动上架。');}catch(error){message(error.message,true);}};
 $('upload').onchange=async()=>{const file=$('upload').files[0];if(!file)return;try{const data=await api('/api/operators/avatar','POST',{png:await fileBase64(file)});state.revision=data.revision;draft.avatar=data.uploaded_avatar;$('preview').src=avatarUrl(draft);renderPackMeta();$('avatar-dialog').close();message('头像已上传，请保存副官配置草稿。');}catch(error){message(error.message,true);}};
 $('apply').onclick=async()=>{try{if(!requireSaved('应用到服务端'))return;await api('/api/operators/apply','POST',{});message('服务端规则已应用。重新启动相关服务后读取生效状态。');await refreshRuntime();}catch(error){message(error.message,true);}};
 $('runtime-check').onclick=async()=>{try{if(!requireSaved('检查安装'))return;const plan=await api('/api/operators/runtime-plan');$('runtime-install').disabled=!plan.ready;$('runtime-state').textContent=plan.ready?'检查通过：将更新 '+plan.selected_hosts.length+' 个已启用战斗服务的配套组件，保留已有设置。':(plan.blockers||[]).join('；');}catch(error){message(error.message,true);}};
 $('runtime-install').onclick=async()=>{try{if(!requireSaved('安装配套组件'))return;const result=await api('/api/operators/install-runtime','POST',{});$('runtime-install').disabled=true;runtimeState(result.runtime_status||result.status||result);message('配套组件已安装。应用服务端规则后重新启动相关服务。');}catch(error){message(error.message,true);}};
 $('runtime-refresh').onclick=()=>refreshRuntime().catch(error=>message(error.message,true));
 $('open-shop').onclick=()=>{if(!requireSaved('前往商城'))return;if(draft.delivery!=='product'){message('请先把发放方式设为「注册为商品」并保存。',true);return;}ctx.openProvider('shop');};
 $('share').onclick=()=>{if(requireSaved('导出分享包'))binary('/api/operators/'+selected+'/share','operator-'+selected+'.opack').catch(error=>message(error.message,true));};
 $('registrations').onclick=()=>{if(requireSaved('导出商品登记'))binary('/api/operators/registrations','operator-product-registrations.json').catch(error=>message(error.message,true));};
 $('package').onclick=()=>{if(requireSaved('生成客户端更新包'))binary('/api/operators/package','SDGO-operator-client-update.zip').catch(error=>message(error.message,true));};
 $('import-pack').onclick=()=>{if(requireSaved('导入分享包'))$('import-dialog').showModal();};$('close-import').onclick=()=>$('import-dialog').close();
 $('pack-file').onchange=async()=>{const file=$('pack-file').files[0];if(!file)return;try{await previewPack({zip:await fileBase64(file),filename:file.name},file.name);}catch(error){message(error.message,true);}};
 $('pack-path').onclick=async()=>{try{const picked=await ctx.pick({kind:'file'});if(picked.path)await previewPack({path:picked.path},picked.path);}catch(error){message(error.message,true);}};
 $('confirm-import').onclick=async()=>{try{
  const body={sha256:importResult.pack_sha256},name=$('import-name').value.trim(),template=$('import-template').value.trim(),source=$('import-source').value.trim();
  if(name)body.name=name;if(template)body.template_id=Number(template);if(source)body.source_template=Number(source);
  const data=await api('/api/operators/import','POST',body);$('import-dialog').close();accept(data,data.selected_id);message('分享包已创建为独立副官草稿。未创建或上架商城商品。');
 }catch(error){message(error.message,true);}};

 try{
  try{protocol=await api('/api/operators/protocol','GET');}catch(error){if(ctx.developer)message('协议说明暂不可用：'+error.message,true);}
  const data=await api('/api/operators','GET');
  const registry=data.registered_ability_extensions||[];
  abilityEditor=createAbilitiesEditor($('abilities'),null,{registered_extensions:Array.isArray(registry)?registry:Object.keys(registry)});
  if(ctx.developer){$('developer-protocol').classList.remove('hidden');renderDeveloperProtocol($('developer-protocol'),protocol);}
  accept(data,(data.operators.find(op=>op.managed)||data.operators[0])?.id);buildItems();
 }finally{root.inert=false;}
}
