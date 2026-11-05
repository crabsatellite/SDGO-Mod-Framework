import {e,button,download} from '/panel_helpers.js';
export async function mount(root,ctx){
 const intro=e('section',undefined,'manager-card');
 intro.append(
  e('h2','活动收藏扭蛋'),
  e('p','奖品是可永久持有的初始形态，通过游戏内领取箱交付。默认每次100,000 GP、每周15次，可集中使用；价格、周期与限额可在下方配置。规则是“先补图鉴，图鉴满后补机库”：第一阶段补齐尚未解锁的图鉴机体；全部符合条件的图鉴机体解锁后，第二阶段补回已解锁但当前机库和领取箱都没有的机体。'),
  e('p','C/B/A/S 默认概率为 35%/30%/20%/15%。当前阶段中没有候选机体的等级会退出抽取，其余等级按原权重重新分配。若图鉴已经解锁完且所有机体都在机库或领取箱中，本次抽取走原生失败退款路径。'),
  e('p','使用361×322活动海报。先生成配套资源，再检查并分别安装到目标 WorldServer 和客户端，最后导出玩家包。安装前关闭对应程序；本面板不会停止游戏服务。')
 );
 root.append(intro);
 let state=await ctx.request('/api/state',undefined);
 const rulesCard=e('section',undefined,'manager-card');rulesCard.append(e('h2','价格、限额与海报规则'));
 const controls={};
 for(const [key,label,min,max] of [['price_gp','每次价格（GB）',1,1000000000],['draw_limit','每周期成功次数（0为不限次）',0,100000],['utc_offset_minutes','周期时区（UTC分钟偏移）',-720,840]]){
  const row=e('label',label),n=e('input');n.type='number';n.min=min;n.max=max;n.value=state.rules[key];controls[key]=n;row.append(n);rulesCard.append(row);
 }
 const cycle=e('select');for(const [v,label] of [['week','每周一重置'],['day','每日零点重置'],['none','不限次']])cycle.add(new Option(label,v));cycle.value=state.rules.limit_period;cycle.onchange=()=>{if(cycle.value==='none')controls.draw_limit.value=0;};rulesCard.append(cycle);
 rulesCard.append(e('p','按账号跨服务器计数；只计成功发出的奖品，失败退款不占次数。保存后重新生成并安装配套服务端、客户端资源，海报与价格同步生效。变更限额不会清空本周期已经使用的次数。'));
 rulesCard.insertBefore(e('p','单独保存：改完点本区「保存价格与限额」。只保存规则，要在下方重新生成并安装配套资源后，游戏里才会生效。','muted section-scope-note'),rulesCard.firstChild.nextSibling);
 const rulesSnapshot=()=>JSON.stringify([cycle.value,...Object.values(controls).map(n=>n.value)]);let savedRules=rulesSnapshot();
 ctx.guard?.(()=>rulesSnapshot()!==savedRules?'「收藏扭蛋」的价格与限额有未保存的修改。':'');
 // Results also show in the Manager's message corner, wherever the page is scrolled.
 const report=text=>{result.textContent=text;ctx.message(text);};
 rulesCard.append(button('保存价格与限额',async()=>{const value={limit_period:cycle.value};for(const [key,n] of Object.entries(controls))value[key]=Number(n.value);state=await ctx.request('/api/rules',{value,revision:state.revision});savedRules=rulesSnapshot();report('规则已保存，请重新生成配套资源并安装；海报按当前规则生成。');render();},ctx,'primary'));root.append(rulesCard);
 const flow=e('section',undefined,'manager-card'),steps=e('div',undefined,'how-it-works');
 for(const [n,text] of [['1','生成 / 更新配套资源'],['2','检查安装条件（不写入）'],['3','安装目标 World 和本机客户端（先关闭对应程序）'],['4','导出玩家包']]){const step=e('div',undefined,'how-step');step.append(e('b',n),e('span',text));steps.append(step);}
 const summary=e('p'),result=e('p'),actions=e('div',undefined,'actions');
 flow.append(e('h2','生成、检查与安装'),e('p','按顺序操作。「更新为图鉴规则」只用于升级旧版数据库，不需要重启 World。','muted section-scope-note'),steps,summary,actions,result);root.append(flow);
 const render=()=>{const position=state.placement?`活动机台第${state.placement.page}页、第${state.placement.page_slot}格。`:'';summary.textContent=state.generated?`已生成 · ${state.eligible_roots} 个可用初始形态 · ${state.price_gp.toLocaleString()} GP。${position}生成后还需要分别安装到 World 和客户端。`:'尚无与当前配置匹配的构建。';};render();
 for(const [id,label] of [['upgrade-catalog','更新为图鉴规则（无需重启）'],['generate','生成 / 更新配套资源'],['check-install','检查安装条件'],['install-server','安装 / 更新目标 World'],['install-client','安装 / 更新本机客户端'],['export','导出玩家包']]){
  actions.append(button(label,async()=>{
   if(id==='upgrade-catalog'&&!confirm('以当前永久机库和未领取单位奖品初始化图鉴，删除旧完整历史证明与额外历史表；以后先补图鉴，全解锁后补机库。原生账号、机体和抽取记录保留。继续更新已安装的收藏扭蛋规则？'))return;
   if(id.startsWith('install-')&&!confirm('更新工作区中选定的目标？请先关闭对应程序。'))return;
   const value=await ctx.request('/api/'+id,{revision:state.revision});
   if(value instanceof Response){await download(value,'SDGO-CollectionCapsule-Player.zip');report('玩家包已生成，请查看下载目录并分发给其他玩家。');}
   else {report(({'upgrade-catalog':'图鉴规则已更新，旧账号可直接使用；本次仅更新 SQL，无需重启 World。','generate':'配套资源已生成。接下来检查安装条件，并分别安装服务端和客户端。','check-install':'安装条件检查通过。此检查不会写入服务端或客户端。','install-server':'服务端已安装。现在可以启动选定的 WorldServer。','install-client':'客户端资源已安装。请使用原来的方式启动游戏。'})[id]);state=await ctx.request('/api/state',undefined);render();}
  },ctx,id==='generate'?'primary':''));
 }
 const family=e('input');family.type='file';family.accept='.json';family.hidden=true;
 family.onchange=async()=>{try{
  const payload=JSON.parse(await family.files[0].text());
  if(!confirm('登记该内容包的初始形态？机体定义必须已经在目标数据库中。'))return;
  await ctx.request('/api/register-family',{revision:state.revision,family:payload});
  report('初始形态已登记，可重新生成配套资源。');
 }catch(error){ctx.message(error.message,true);}finally{family.value='';}};
 root.append(family,button('登记导入机体的初始形态',()=>family.click(),ctx));
 root.append(e('p','安装时会以当前永久机体和领取箱内尚未领取的永久机体建立最小图鉴解锁基线。Mod 只保存账号与已解锁机体 ID，不导入旧版完整获得历史，也不记录时间、来源或抽取事件。普通扭蛋目录另行更新后，请在本页重新生成并安装客户端资源，再导出最新玩家包。','intro'));
}
