export function e(tag,text,cls){const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;}
export function button(text,fn,ctx,cls=''){const n=e('button',text,cls);n.onclick=async()=>{n.disabled=true;try{await fn();}catch(error){ctx.message(error.message,true);}finally{n.disabled=false;}};return n;}
export function field(label,value,type='text'){const box=e('label',label),input=document.createElement('input');input.type=type;if(type==='checkbox')input.checked=!!value;else input.value=value??'';box.append(input);return {box,input};}
export function reviewChange(plan){
 return new Promise(resolve=>{const dialog=e('dialog'),list=e('ul'),actions=e('div',undefined,'actions');
  dialog.append(e('h2',plan.title),e('p',plan.maintenance));for(const effect of plan.effects)list.append(e('li',effect));dialog.append(list);
  if(plan.nodes?.length)dialog.append(e('p','涉及服务：'+plan.nodes.join('、'),'muted'));
  let accepted=false;const close=()=>dialog.close();
  actions.append(button('返回修改',close,{message:()=>{}},'ui-btn is-ghost'),button('确认本次变更',()=>{accepted=true;close();},{message:()=>{}},'ui-btn is-primary'));
  dialog.append(actions);dialog.onclose=()=>{dialog.remove();resolve(accepted);};dialog.oncancel=()=>{accepted=false;};document.body.append(dialog);dialog.showModal();
 });
}
// A panel's edits that wait for explicit saves: one bar, kept in view at the bottom of the page, lists each
// save scope ({title, snapshot(), jump?()}) as saved or unsaved and holds the save buttons ({label, run, covers:
// scope indexes, primary}; run returning false means cancelled). A scope with jump() is a link to its section,
// for panels whose save buttons stay inside each section. Leaving the page with unsaved scopes asks first
// (ctx.guard). bar.markSaved() takes the current values as saved; bar.unsaved() lists unsaved scope titles.
export function saveBar(root,ctx,page,scopes,actions,hint){
 const bar=e('div',undefined,'savebar'),list=e('div',undefined,'savebar-scopes'),group=e('div',undefined,'savebar-group');
 if(hint)list.append(e('span',hint,'savebar-hint'));
 const saved=scopes.map(scope=>scope.snapshot()),rows=scopes.map(scope=>{const row=list.appendChild(e(scope.jump?'button':'span',undefined,'savebar-scope'));if(scope.jump){row.type='button';row.classList.add('is-link','ui-plain');row.onclick=scope.jump;}return row;});
 const dirty=()=>scopes.map((scope,i)=>scope.snapshot()!==saved[i]);
 function refresh(){const d=dirty();scopes.forEach((scope,i)=>{rows[i].textContent=scope.title+(d[i]?'：有未保存的修改':'：已保存');rows[i].classList.toggle('is-dirty',d[i]);});bar.classList.toggle('is-dirty',d.includes(true));}
 for(const action of actions){
  const run=button(action.label,async()=>{if(await action.run()===false)return;for(const i of action.covers)saved[i]=scopes[i].snapshot();refresh();},ctx,'ui-btn '+(action.primary?'is-primary':'is-blue'));
  run.type='button';group.append(run);
 }
 let pending=0;const later=()=>{clearTimeout(pending);pending=setTimeout(refresh,0);};
 for(const name of ['input','change','click'])root.addEventListener(name,later);
 const unsaved=()=>{const d=dirty();return scopes.filter((_,i)=>d[i]).map(scope=>scope.title);};
 ctx.guard?.(()=>{const names=unsaved();return names.length?'「'+page+'」中'+names.join('、')+'有未保存的修改。':'';});
 bar.markSaved=()=>{scopes.forEach((scope,i)=>saved[i]=scope.snapshot());refresh();};
 bar.unsaved=unsaved;
 bar.append(list);if(actions.length)bar.append(group);refresh();return bar;
}
// Scrolls the page so a section (also one inside a panel's shadow root) starts just below the sticky header.
export function scrollToSection(node){const top=parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--mm-sticky-top'))||140;scrollTo({top:node.getBoundingClientRect().top+scrollY-top-16,behavior:'smooth'});}
export async function download(response,name){const blob=await response.blob(),url=URL.createObjectURL(blob),link=e('a');link.href=url;link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);}

let displayUtcOffsetMinutes=0;
export function setDisplayUtcOffset(value){if(Number.isInteger(value)&&value>=-720&&value<=840)displayUtcOffsetMinutes=value;}
export function serverDateTime(value){return new Date(new Date(value).getTime()+displayUtcOffsetMinutes*60000).toLocaleString('zh-CN',{timeZone:'UTC',hour12:false});}
