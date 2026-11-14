import {e,button,field,download} from '/panel_helpers.js';
export async function mount(root,ctx){
 root.inert=true;
 let state, selected=null, query='', page=0;const pageSize=50;
 root.append(e('p','首次绑定空商城目录会自动上架永久机体、租赁机体、副官、设计图和消耗品预设，沿用本服现货的价格与期限。初始化后可在下方编辑或下架；更新不会重置你的修改。','muted'));
 const hasChinese=value=>/[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]/.test(value||'');
 const automaticLabel=(product,validity)=>{const item=state.items.find(row=>row.Id===product.category_id);if(!item)return product.label;if(item.TimeLimit===1)return (item.Kind===11?'租赁 ':'有效期 ')+validity+' 天';return item.Kind===11?'永久':'数量 '+validity;};
 const controls=e('div',undefined,'actions'),search=document.createElement('input');search.placeholder='搜索商品名称或编号';search.oninput=()=>{query=search.value;page=0;render();};controls.append(search);
 const select=document.createElement('select');select.ariaLabel='新增商品的原生物品';
 const createName=document.createElement('input');createName.placeholder='中文商品名称（必填，可含 MSA-001 等代号）';createName.maxLength=50;createName.required=true;
 const createNameBox=e('label','新商品中文名称');createNameBox.append(createName,e('span','上方原生物品下拉保留原始名称，仅用于确认实际物品。','field-help'));
 controls.append(select,createNameBox,button('新增商品',async()=>{if(!select.value)throw Error('请选择原生物品');const name=createName.value.trim();if(!hasChinese(name))throw Error('中文商品名称必须包含至少一个汉字，可以同时保留英文或数字代号。');await action('create',{category_id:Number(select.value),name});createName.value='';},ctx,'primary'),button('应用到游戏',async()=>{if(!confirm('将当前商城草稿应用到已选择的服务器和客户端？未补齐中文的旧商品会被拒绝。'))return;await action('apply',{});ctx.message('服务器已返回应用结果。游戏内显示与购买由你手动检查。');},ctx),button('更新中文检索',async()=>{await action('search-index',{});const result=state.search_index;if(state.demo){ctx.message('演示模式仅模拟中文检索更新，未连接服务器数据库。');return;}if(!result||!result.installed)throw Error('服务器未确认中文检索更新。');ctx.message('中文检索已更新：'+result.name_count+' 个名称'+(result.revision?'，版本 '+result.revision:'')+'。');},ctx),button('导出配置',async()=>download(await ctx.fetch('/api/shop/export'),'shop.json'),ctx),button('导出玩家更新包',async()=>{const response=await ctx.fetch('/api/shop/client-package',{headers:{'X-Shop-Revision':state.revision}});if(!response.ok){let message='玩家更新包生成失败';try{message=(await response.json()).error||message;}catch{}throw Error(message);}await download(response,'SDGO-shop-client-update.zip');},ctx));
 const importFile=document.createElement('input');importFile.type='file';importFile.accept='.json';importFile.hidden=true;importFile.onchange=async()=>{try{await action('import',{catalog:JSON.parse(await importFile.files[0].text())});}catch(error){ctx.message(error.message,true);}};
 controls.append(button('导入配置',()=>importFile.click(),ctx),button('刷新物品目录',()=>load(),ctx),importFile);root.append(controls);
 const operatorBox=e('section',undefined,'manager-card');root.append(operatorBox);
 const layout=e('div',undefined,'settings-grid'),list=e('section',undefined,'manager-card'),editor=e('section',undefined,'manager-card');layout.append(list,editor);root.append(layout);
 async function load(){state=await ctx.request('/api/shop/catalog');const options=state.items.map(item=>new Option(item.Name+' · '+item.Id,item.Id));select.replaceChildren(...options);render();}
 async function action(name,body){state=await ctx.request('/api/shop/'+name,body,'POST',{'X-Shop-Revision':state.revision});render();}
 function renderOperatorImports(){
  const rows=Object.values(state.operator_imports||{}).sort((a,b)=>a.operator_id-b.operator_id);
  operatorBox.replaceChildren();operatorBox.hidden=!rows.length;if(!rows.length)return;
  operatorBox.append(e('h2','副官商品草稿'),e('p','副官面板只保存商品预填。这里创建的商品保持未上架；仍需检查名称、价格和期限后保存，最后再单独应用到游戏。','muted'));
  for(const info of rows){
   const card=e('div',undefined,'manager-card'),title=e('strong',info.display_name+' · 副官 '+info.operator_id),meta=e('p','商品类别 '+info.category_id,'muted');card.append(title,meta);
   if(info.draft_defaults){const values=info.draft_defaults;card.append(e('p','预填：'+[
    values.name,values.label,values.price!==undefined?values.price+' '+(values.currency||''):undefined,
    values.validity!==undefined?'数量 / 期限 '+values.validity:undefined,
   ].filter(Boolean).join(' · '),'muted'));}
   if(info.draft_error)card.append(e('p',info.draft_error,'error'));
   else if(info.shop_product_keys?.length){
    card.append(button('打开商城商品',()=>{selected=info.shop_product_keys[0];render();},ctx));
   }else{
    card.append(button('创建未上架商品草稿',async()=>{await action('create',{category_id:info.category_id});const current=state.operator_imports?.[String(info.category_id)];selected=current?.shop_product_keys?.[0]||selected;render();ctx.message('副官商品草稿已创建并保持未上架。检查后保存；不会自动应用到游戏。');},ctx,'primary'));
   }
   operatorBox.append(card);
  }
 }
 function render(){
  renderOperatorImports();
  list.replaceChildren(e('h2',state.demo?'商城商品（演示）':'商城商品'));
  const rows=state.products.filter(row=>JSON.stringify(row.product).toLowerCase().includes(query.toLowerCase()));
  const pages=Math.max(1,Math.ceil(rows.length/pageSize));page=Math.min(page,pages-1);
  list.append(e('p',`共 ${rows.length} 件 · 第 ${page+1} / ${pages} 页，每页 ${pageSize} 件`,'muted'));
  for(const row of rows.slice(page*pageSize,(page+1)*pageSize)){const p=row.product;list.append(button(p.name+' · '+p.price+' '+p.currency+' · '+row.status,()=>{selected=p.key;render();},ctx));}
  if(pages>1){const navigation=e('div',undefined,'actions'),previous=button('上一页',()=>{page--;render();},ctx),next=button('下一页',()=>{page++;render();},ctx);previous.disabled=page===0;next.disabled=page===pages-1;navigation.append(previous,next);list.append(navigation);}
  if(!rows.length)list.append(e('p','还没有管理商品。选择一个原生物品，然后新增。','muted'));
  editor.replaceChildren();const product=state.catalog.products.find(p=>p.key===selected);if(!product){editor.append(e('p','选择商品查看和编辑。新增前还需要在原生物品下方填写中文商品名称。','muted'));return;}
  editor.append(e('h2',product.name),e('p','商品 '+product.product_id+' · 物品 '+product.category_id,'muted'));
  const imported=state.unit_imports?.[String(product.category_id)];if(imported){const image=document.createElement('img');image.src=imported.shop_image_url;image.alt=imported.display_name;image.style.cssText='width:122px;height:122px;object-fit:contain';editor.append(image,e('p','与游戏普通商品列表相同的已确认图标','muted'));}
  const operatorImport=state.operator_imports?.[String(product.category_id)];if(operatorImport)editor.append(e('p','副官 '+operatorImport.operator_id+' 的商城商品 · 新建时默认未上架','muted'));
  if(!hasChinese(product.name)||!hasChinese(product.label))editor.append(e('p','这个旧商品尚未补齐中文名称或中文销售标签。可以继续查看；保存修改或发布前必须补齐。','error'));
  const fields={};for(const [key,label,type] of [['name','中文商品名称（必填，需含汉字）'],['label','中文销售标签（必填，需含汉字）'],['price','售价','number'],['validity','数量 / 租赁天数','number'],['enabled','上架','checkbox']]){fields[key]=field(label,product[key],type);editor.append(fields[key].box);}
  fields.validity.input.oninput=()=>{const validity=Number(fields.validity.input.value);if(Number.isFinite(validity)&&validity>0)fields.label.input.value=automaticLabel(product,validity);};
  const currency=e('label','币种'),value=document.createElement('select');for(const name of ['GB','MB'])value.append(new Option(name,name));value.value=product.currency;currency.append(value);editor.append(currency);
  editor.append(button('保存商品',async()=>{const values={currency:value.value};for(const [key,entry] of Object.entries(fields))values[key]=entry.input.type==='checkbox'?entry.input.checked:entry.input.type==='number'?Number(entry.input.value):entry.input.value.trim();if(!hasChinese(values.name)||!hasChinese(values.label))throw Error('中文商品名称和中文销售标签都必须包含至少一个汉字。');await action('update',{key:product.key,values});ctx.message('商品草稿已保存，应用到游戏后才会更改服务器商品。');},ctx,'primary'),button(product.retired?'恢复商品':'删除商品',()=>action(product.retired?'restore':'delete',{key:product.key}),ctx));
 }
 try{await load();}finally{root.inert=false;}
}
