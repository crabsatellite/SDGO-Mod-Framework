export function e(tag,text,cls){const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;}
export function button(text,fn,ctx,cls=''){const n=e('button',text,cls);n.onclick=async()=>{n.disabled=true;try{await fn();}catch(error){ctx.message(error.message,true);}finally{n.disabled=false;}};return n;}
export function field(label,value,type='text'){const box=e('label',label),input=document.createElement('input');input.type=type;if(type==='checkbox')input.checked=!!value;else input.value=value??'';box.append(input);return {box,input};}
// A panel's edits that wait for explicit saves: one bar, kept in view at the bottom of the page, lists each
// save scope ({title, snapshot()}) as saved or unsaved and holds the save buttons ({label, run, covers: scope
// indexes, primary}). Leaving the page with unsaved scopes asks first (ctx.guard).
export function saveBar(root,ctx,page,scopes,actions){
 const bar=e('div',undefined,'savebar'),list=e('div',undefined,'savebar-scopes'),group=e('div',undefined,'savebar-group');
 const saved=scopes.map(scope=>scope.snapshot()),rows=scopes.map(()=>list.appendChild(e('span',undefined,'savebar-scope')));
 const dirty=()=>scopes.map((scope,i)=>scope.snapshot()!==saved[i]);
 function refresh(){const d=dirty();scopes.forEach((scope,i)=>{rows[i].textContent=scope.title+(d[i]?'：有未保存的修改':'：已保存');rows[i].classList.toggle('is-dirty',d[i]);});bar.classList.toggle('is-dirty',d.includes(true));}
 for(const action of actions){
  const run=button(action.label,async()=>{await action.run();for(const i of action.covers)saved[i]=scopes[i].snapshot();refresh();},ctx,'ui-btn '+(action.primary?'is-primary':'is-blue'));
  run.type='button';group.append(run);
 }
 let pending=0;const later=()=>{clearTimeout(pending);pending=setTimeout(refresh,0);};
 for(const name of ['input','change','click'])root.addEventListener(name,later);
 ctx.guard?.(()=>{const d=dirty(),names=scopes.filter((_,i)=>d[i]).map(scope=>scope.title);return names.length?'「'+page+'」中'+names.join('、')+'有未保存的修改。':'';});
 bar.append(list,group);refresh();return bar;
}
export async function download(response,name){const blob=await response.blob(),url=URL.createObjectURL(blob),link=e('a');link.href=url;link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(url),30000);}

let displayUtcOffsetMinutes=0;
export function setDisplayUtcOffset(value){if(Number.isInteger(value)&&value>=-720&&value<=840)displayUtcOffsetMinutes=value;}
export function serverDateTime(value){return new Date(new Date(value).getTime()+displayUtcOffsetMinutes*60000).toLocaleString('zh-CN',{timeZone:'UTC',hour12:false});}
