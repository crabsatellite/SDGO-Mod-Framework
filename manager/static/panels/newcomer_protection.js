import {e,saveBar} from '/panel_helpers.js';
export async function mount(root,ctx){
 const api=(path,body)=>ctx.request('/api/newcomer-protection'+path,body,body===undefined?'GET':'POST');
 let state=await api(''),selected=null;
 root.append(e('p','指定玩家在任务和对战中获得生命与普通武器伤害倍率。必杀、生命条数保持原规则；配置在对应 Battle 服务端重启后生效。'));
 root.append(e('p',state.privacy,'muted'));
 if(!state.hosts.length){root.append(e('p','请先在 Battle 服务端安装新手保护模组。'));return;}
 const hosts=e('select');for(const host of state.hosts)hosts.add(new Option(host.name,host.id));
 root.append(e('label','对战服务进程'),hosts);
 const enabled=e('input');enabled.type='checkbox';
 const health=e('input'),attack=e('input');for(const input of [health,attack]){input.type='number';input.min='1';input.max='10';input.step='0.1';}
 const field=(label,input)=>{const row=e('label',label);row.append(input);root.append(row);};
 field('启用新手保护',enabled);field('生命倍率',health);field('普通武器伤害倍率',attack);
 const list=e('ul'),query=e('input'),matches=e('select'),search=e('button','查找玩家','ui-btn'),add=e('button','加入保护名单','ui-btn');
 query.type='search';query.placeholder='输入玩家名称';
 const tools=e('div',undefined,'toolbar');tools.append(query,search,matches,add);root.append(e('h3','本服保护名单'),list,tools);
 let accounts=[],bar=null;
 const show=()=>{list.replaceChildren();for(const id of accounts){const row=e('li',state.players[String(id)]||'玩家名称暂未读取');const remove=e('button','移除','ui-btn');remove.onclick=()=>{accounts=accounts.filter(n=>n!==id);show();};row.append(remove);list.append(row);}};
 const choose=()=>{selected=state.hosts.find(h=>h.id===hosts.value);const v=selected.details.values;enabled.checked=v.enabled;health.value=v.health_multiplier;attack.value=v.attack_multiplier;accounts=v.protected_accounts.split(',').filter(n=>n.trim()).map(Number);show();};
 hosts.onchange=()=>{if(bar?.unsaved().length&&!globalThis.confirm('新手保护有未保存的修改，仍要切换服务进程吗？')){hosts.value=selected.id;return;}choose();bar?.markSaved();};choose();
 search.onclick=async()=>{try{const result=await api('/players?q='+encodeURIComponent(query.value));matches.replaceChildren();for(const row of result.rows){matches.add(new Option(row.title,row.value));state.players[String(row.value)]=row.title;}}catch(error){ctx.message(error.message,true);}};
 add.onclick=()=>{const id=Number(matches.value);if(id&&!accounts.includes(id)){accounts.push(id);show();}};
 bar=saveBar(root,ctx,'新手保护',[{title:'本服名单与倍率',snapshot:()=>JSON.stringify([enabled.checked,health.value,attack.value,accounts])}],[{label:'保存本服设置',covers:[0],primary:true,run:async()=>{const result=await api('/policy',{host:selected.id,revisions:selected.details.revisions,values:{enabled:enabled.checked,health_multiplier:Number(health.value),attack_multiplier:Number(attack.value),protected_accounts:accounts}});selected.details=result;ctx.message('已保存；请重启这个 Battle 服务端后生效。');}}]);
}
