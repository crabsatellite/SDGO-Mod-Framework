import {e,field,button,saveBar} from '/panel_helpers.js';

export async function mount(root,ctx){
 async function api(path='',body){const r=await ctx.fetch('/api/ai-dialogue'+path,{method:body===undefined?'GET':'POST',headers:{'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body)});const data=await r.json();if(!r.ok)throw Error(data.error||'AI 台词操作失败');return data;}
 let state=await api();
 root.append(e('p','AI 会结合本局机体、击破双方、队友、已死亡次数与复活状态说一句现场台词。默认关闭；启用后由当前 Manager 在后台调用快速模型。','muted'));
 if(!state.clients.length){root.append(e('p','请先添加已经安装 AI 互动台词的客户端。'));return;}
 const target=field('客户端','','text'),select=e('select');target.input.replaceWith(select);for(const h of state.clients){const option=e('option',h.name);option.value=h.id;select.append(option);}select.value=state.host_id;root.append(target.box);
 select.onchange=async()=>{if(bar.unsaved().length&&!confirm('有未保存的修改，要切换客户端吗？')){select.value=state.host_id;return;}try{state=await api('?host_id='+encodeURIComponent(select.value));fill();bar.markSaved();}catch(error){ctx.message(error.message,true);}};
 const grid=e('div',undefined,'settings-grid'),controls={};root.append(grid);
 const specs=[['enabled','启用 AI 互动台词','checkbox'],['trigger','触发方式',['probability','always']],['probability_percent','每次事件触发概率（%）','number'],['visibility','谁能看到',['local','room']],['cooldown_seconds','发言最短间隔（秒，0 为每次事件）','number'],['max_lines','每局 API 调用上限（含失败）','number'],['team_talk','启用队友集火与掩护建议','checkbox'],['endpoint','DeepSeek API 地址','url'],['model','快速模型名称','text'],['language','台词语言','text']];
 const labels={probability:'按概率触发',always:'每次符合条件的事件',local:'仅自己可见',room:'本局战斗频道（所有人可见）'};
 for(const [key,title,type]of specs){const box=e('label',title),input=Array.isArray(type)?e('select'):e('input');if(Array.isArray(type)){for(const choice of type){const option=e('option',labels[choice]);option.value=choice;input.append(option);}}else input.type=type;if(type==='number'){input.min='0';input.step='1';}box.append(input);grid.append(box);controls[key]=input;}
 root.append(e('p','“每次触发”仍遵守间隔和每局调用上限；将间隔设为 0 可逐次触发。只处理你击破 AI、AI 击破你和分配给你的队友建议。发到战斗频道时需要接收端也装有本组件。文字显示为“机体名：台词”，多人时在台词里自然点名。','muted'));
 const keyCard=e('section',undefined,'manager-card'),key=field('DeepSeek API 密钥（仅本机保存）','','password'),keyState=e('p',undefined,'muted');key.input.autocomplete='new-password';keyCard.append(e('h2','本机 API'),key.box,keyState);
 keyCard.append(button('保存本机密钥',async()=>{const secret=key.input.value;key.input.value='';const value=await api('/key',{host_id:state.host_id,api_key:secret});state.has_api_key=value.has_api_key;keyState.textContent=value.has_api_key?'已保存本机密钥；不会显示、导出或随更新分发。':'尚未保存 API 密钥。';ctx.message('本机密钥已保存。');},ctx));
 keyCard.append(button('清除本机密钥',async()=>{await api('/key',{host_id:state.host_id,api_key:''});state.has_api_key=false;keyState.textContent='尚未保存 API 密钥。';},ctx));root.append(keyCard);
 const service=e('p');root.append(service);
 async function serviceAction(action){const r=await globalThis.fetch('/api/services/battle.ai_dialogue',{method:'POST',headers:{'Content-Type':'application/json','X-ModManager-Token':ctx.token},body:JSON.stringify({action})});const value=await r.json();if(!r.ok)throw Error(value.error||'后台服务操作失败');state.service=value;serviceText();}
 function serviceText(){const s=state.service;service.textContent='后台台词服务：'+(s.running?'运行中':'已停止')+(s.error?'；'+s.error:'');}
 root.append(button('启动台词服务',()=>serviceAction('start'),ctx),button('停止台词服务',()=>serviceAction('stop'),ctx));
 const promptLabel=e('label','角色与战况提示词'),prompt=e('textarea');prompt.rows=16;promptLabel.append(prompt);root.append(promptLabel);
 const personasLabel=e('label','机体与机师设定（JSON）'),personas=e('textarea');personas.rows=14;personasLabel.append(personas);root.append(personasLabel,e('p','可指定机体编号或名称片段。未匹配时，模型按机体与原作推测口吻；量产机可使用普通士兵人设。生存人数、战果和是否已出局以游戏状态为准。快速模型关闭思考，超时回复直接丢弃。','muted'));
 function values(){return Object.fromEntries(specs.map(([key,,type])=>[key,type==='checkbox'?controls[key].checked:type==='number'?Number(controls[key].value):controls[key].value]));}
 function fill(){for(const[key,,type]of specs)if(type==='checkbox')controls[key].checked=state.values[key];else controls[key].value=state.values[key];prompt.value=state.prompt;personas.value=JSON.stringify(state.personas,null,2);keyState.textContent=state.has_api_key?'已保存本机密钥；不会显示、导出或随更新分发。':'尚未保存 API 密钥。';serviceText();}
 fill();
 const bar=saveBar(root,ctx,'AI 机师互动台词',[{title:'台词规则与机师设定',snapshot:()=>JSON.stringify([values(),prompt.value,personas.value])}],[{label:'保存台词设置',primary:true,covers:[0],run:async()=>{state=await api('',{host_id:state.host_id,values:values(),revisions:state.revisions,content_revisions:state.content_revisions,prompt:prompt.value,personas:JSON.parse(personas.value)});fill();ctx.message('台词设置已保存；游戏将自动读取。首次安装组件需重新启动游戏。');}}]);root.append(bar);
 ctx.setInterval(async()=>{try{const r=await globalThis.fetch('/api/services/battle.ai_dialogue');if(r.ok){state.service=await r.json();serviceText();}}catch{}},4000);
}
