import {serverDateTime} from '/panel_helpers.js';
// Unit-family authoring inside the single Mod Manager; no separate session or process.
export async function mount(root,ctx){
 const shadow=root.attachShadow({mode:"open"});
 shadow.innerHTML="<style>\n:host{\n  color-scheme:dark;\n  --bg:#0d131d;\n  --panel:#172130;\n  --panel2:#111b28;\n  --line:#33445b;\n  --line2:#25354a;\n  --text:#edf3fc;\n  --muted:#9dacbf;\n  --accent:#75d8bb;\n  --accent2:#4d9fc8;\n  --warn:#f2c56d;\n  --danger:#ff9f98;\n  --ok:#8ce0bd;\n}\n*{box-sizing:border-box}\n.module-body{margin:0;background:var(--bg);color:var(--text);font:14px/1.5 \"Segoe UI\",\"Microsoft YaHei\",sans-serif}\nbutton,input,select,textarea{font:inherit}\nbutton,.button{\n  cursor:pointer;border:1px solid var(--line);border-radius:8px;padding:9px 13px;\n  background:#223147;color:var(--text);text-decoration:none;display:inline-flex;\n  align-items:center;justify-content:center;gap:6px\n}\nbutton:hover,.button:hover{border-color:var(--accent)}\nbutton:disabled,.button.disabled{opacity:.42;cursor:default;pointer-events:none}\nbutton.primary{background:var(--accent);border-color:var(--accent);color:#09261e;font-weight:700}\nbutton.danger{color:#ffc0bc;border-color:#72413f;background:#382426}\ninput,select,textarea{\n  width:100%;min-width:0;border:1px solid var(--line);border-radius:7px;\n  background:#0d1724;color:var(--text);padding:9px 10px\n}\ninput[type=checkbox]{width:auto}\ntextarea{resize:vertical;min-height:70px}\nheader{\n  height:70px;padding:0 26px;border-bottom:1px solid var(--line);background:#121b28;\n  display:flex;align-items:center;justify-content:space-between;gap:18px\n}\nheader strong{font-size:18px}\nheader small{display:block;color:var(--muted)}\n.shell{display:grid;grid-template-columns:185px minmax(0,1fr);min-height:calc(100vh - 70px)}\nnav{border-right:1px solid var(--line);padding:24px 14px;color:var(--muted)}\nnav .active{margin-top:12px;padding:12px;border-radius:8px;background:#233c42;color:#a5ead5}\nmain{width:100%;max-width:1640px;padding:26px}\n.top{display:flex;align-items:flex-start;justify-content:space-between;gap:16px;margin-bottom:15px}\n.top h1{margin:0;font-size:26px}\np{margin:5px 0;color:var(--muted)}\n.actions{display:flex;gap:8px;flex-wrap:wrap}\n#status{min-height:26px;margin:8px 0 15px;color:var(--accent);white-space:pre-wrap}\n#status.error{color:var(--danger)}\n.badge{\n  display:inline-flex;align-items:center;white-space:nowrap;border:1px solid #45645d;\n  border-radius:999px;padding:3px 8px;font-size:12px;color:var(--accent)\n}\n.badge.warning{color:var(--warn);border-color:#745f34}\n.badge.danger{color:var(--danger);border-color:#75423f}\n.badge.muted{color:var(--muted);border-color:var(--line)}\n.layout{display:grid;grid-template-columns:300px minmax(0,1fr);gap:18px}\n.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px}\n.sidebar{align-self:start;padding:14px;position:sticky;top:14px}\n.upload-zone{\n  border:1px dashed #527089;border-radius:10px;background:#101b29;padding:16px;\n  text-align:center;transition:.15s ease\n}\n.upload-zone.drag{border-color:var(--accent);background:#18312f}\n.upload-zone strong{display:block;margin-bottom:4px}\n.upload-zone input{display:none}\n.upload-zone button{margin-top:10px;width:100%}\n.progress{height:6px;border-radius:99px;background:#0b121b;overflow:hidden;margin-top:10px}\n.progress span{display:block;width:0;height:100%;background:var(--accent);transition:width .12s ease}\n.progress.hidden{display:none}\n.side-head{display:flex;align-items:center;justify-content:space-between;margin:17px 0 8px}\n.side-head h2{font-size:14px;margin:0}\n.draft-list{max-height:580px;overflow:auto}\n.draft-item{\n  display:block;width:100%;text-align:left;border:0;border-radius:8px;background:transparent;\n  padding:10px;margin:2px 0\n}\n.draft-item:hover{background:#1e2e42}\n.draft-item.selected{background:#254047;box-shadow:inset 3px 0 var(--accent)}\n.draft-item strong,.draft-item small{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n.draft-item small{color:var(--muted);margin-top:3px}\n.editor{padding:22px;min-width:0}\n.empty{padding:72px 24px;text-align:center;color:var(--muted)}\n.hidden{display:none!important}\n.editor-head{display:flex;align-items:flex-start;justify-content:space-between;gap:15px}\n.editor-head h2{margin:0;font-size:20px}\n.idline{margin-top:4px;color:var(--muted);font-family:ui-monospace,Consolas,monospace;font-size:12px}\n.section{margin-top:22px;padding-top:20px;border-top:1px solid var(--line)}\n.section>h3,.section-title h3{font-size:16px;margin:0}\n.section-title{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:11px}\n.note{font-size:12px;color:var(--muted)}\n.form-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}\nlabel.field{display:block;color:#c8d3e2}\nlabel.field>span{display:block;margin-bottom:5px}\n.wide{grid-column:span 2}\n.summary-grid{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:8px;margin-top:14px}\n.metric{background:var(--panel2);border:1px solid var(--line2);border-radius:8px;padding:10px}\n.metric strong{display:block;font-size:20px;color:#d9f6eb}\n.metric span{font-size:12px;color:var(--muted)}\n.requirements{display:grid;gap:8px}\n.requirement{\n  display:grid;grid-template-columns:auto minmax(0,1fr);gap:10px;\n  padding:11px;border:1px solid var(--line2);border-radius:8px;background:var(--panel2)\n}\n.requirement.error{border-color:#74433f;background:#2d2023}\n.requirement.required{border-color:#725e34;background:#2d281d}\n.requirement.warning{border-color:#4c6076}\n.requirement strong{display:block}\n.requirement small{display:block;color:var(--muted);margin-top:3px;overflow-wrap:anywhere}\n.upload-tools{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:12px}\n.upload-tool{padding:12px;border:1px solid var(--line2);border-radius:9px;background:var(--panel2)}\n.upload-tool label{display:flex;align-items:center;justify-content:space-between;gap:10px}\n.upload-tool input{display:none}\n.content-columns{display:grid;grid-template-columns:1fr 1fr;gap:14px}\n.record-stack{display:grid;gap:10px}\ndetails.record{border:1px solid var(--line2);border-radius:9px;background:var(--panel2);overflow:hidden}\ndetails.record[open]{border-color:#456078}\ndetails.record>summary{cursor:pointer;padding:12px 14px;list-style:none;display:flex;justify-content:space-between;gap:12px}\ndetails.record>summary::-webkit-details-marker{display:none}\n.record-body{padding:0 14px 14px}\n.record-body h4{margin:14px 0 7px;font-size:13px}\n.template-tool{\n  display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;align-items:end;\n  margin-top:13px;padding:10px;border:1px solid #384b62;border-radius:8px;background:#142234\n}\n.template-tool label{display:block;color:#c8d3e2}\n.template-tool label span{display:block;margin-bottom:5px}\n.template-tool input{margin-bottom:7px}\n.client-json{\n  margin:0;max-height:250px;overflow:auto;padding:10px;border-radius:7px;background:#09121d;\n  border:1px solid #203044;color:#bed3e9;font:12px/1.45 ui-monospace,Consolas,monospace;\n  white-space:pre-wrap;overflow-wrap:anywhere\n}\n.server-fields{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px}\n.server-field{padding:9px;border:1px solid #283a50;border-radius:7px;background:#101b29}\n.server-field label{display:block}\n.field-meta{display:flex;justify-content:space-between;gap:8px;margin-bottom:5px;color:#cad5e3}\n.field-meta small{color:var(--muted)}\n.server-field .source{display:block;color:var(--muted);font-size:11px;margin-top:5px;overflow-wrap:anywhere}\n.preview-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}\n.preview-card{border:1px solid var(--line);border-radius:11px;background:var(--panel2);overflow:hidden}\n.preview-stage{\n  position:relative;min-height:265px;background:\n  radial-gradient(circle at 72% 20%,#315674 0,transparent 34%),\n  linear-gradient(145deg,#1c3047,#0b131e 65%);\n  display:flex;align-items:center;justify-content:center;padding:18px\n}\n.preview-stage.capsule{background:\n  radial-gradient(circle at 28% 22%,#5b3b72 0,transparent 34%),\n  linear-gradient(145deg,#382544,#10131e 67%)}\n.preview-stage img{width:100%;height:220px;object-fit:contain;filter:drop-shadow(0 10px 16px #0008)}\n.preview-stage img.missing{display:none}\n.preview-fallback{color:#8291a5;text-align:center}\n.preview-copy{\n  position:absolute;left:16px;right:16px;bottom:13px;padding:9px 11px;border-radius:7px;\n  background:#07101bd9;border:1px solid #3a5068\n}\n.preview-copy strong{display:block;font-size:17px}\n.preview-copy span{color:#b7c6d8}\n.preview-controls{padding:13px;display:grid;gap:9px}\n.asset-row{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px}\n.asset-row input{display:none}\n.confirm-row{\n  display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:13px\n}\n.confirm-box{\n  display:flex;align-items:flex-start;gap:9px;padding:12px;border:1px solid var(--line);\n  border-radius:9px;background:#132131\n}\n.confirm-box:has(input:checked){border-color:var(--accent);background:#19332f}\n.confirm-box input{margin-top:4px}\n.footer{\n  display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap;\n  margin-top:22px;padding-top:18px;border-top:1px solid var(--line)\n}\n.save-state{align-self:center;color:var(--muted)}\n.save-state.dirty{color:var(--warn)}\n.busy-cover{pointer-events:none;opacity:.68}\n@media(max-width:1180px){\n  .shell{grid-template-columns:150px 1fr}\n  .layout{grid-template-columns:250px minmax(0,1fr)}\n  main{padding:18px}\n  .form-grid{grid-template-columns:1fr 1fr}\n  .summary-grid{grid-template-columns:repeat(3,1fr)}\n  .content-columns{grid-template-columns:1fr}\n}\n@media(max-width:820px){\n  header{padding:0 15px}.shell{display:block}nav{display:none}main{padding:14px}.layout{display:block}\n  .sidebar{position:static;margin-bottom:14px}.draft-list{max-height:220px}.editor{padding:15px}\n  .top,.editor-head{flex-direction:column}.form-grid,.preview-grid,.confirm-row,.upload-tools{grid-template-columns:1fr}\n  .wide{grid-column:auto}.server-fields{grid-template-columns:1fr}.summary-grid{grid-template-columns:repeat(2,1fr)}\n}\n\n.shell{display:block;min-height:0}.shell>nav{display:none}main{padding:0}.top h1{display:none}.module-body{background:transparent}\n.preview-stage,.preview-stage.capsule{background:#101a28;min-height:170px}.preview-stage img{height:140px;filter:none}.preview-copy{display:none}</style><div class=\"module-body\">\n<div class=\"actions\"><span class=\"badge muted\" id=\"connection\">正在读取目录</span></div>\n<div class=\"shell\">\n  <nav>\n    <small>内容模块</small>\n    <div class=\"active\">◇　机体导入</div>\n    <p class=\"note\">解析制作包、补齐服务端定义，并生成经过双预览确认的标准内容包。</p>\n  </nav>\n  <main>\n    <div class=\"top\">\n      <div>\n        <h1>机体内容导入</h1>\n        <p>上传 RAR、ZIP 或标准机体包。工具只读取内容，不执行包内程序、脚本或 SQL。</p>\n      </div>\n      <div class=\"actions\">\n        <button type=\"button\" id=\"refresh\">刷新目录</button>\n        <a class=\"button disabled\" id=\"server-plan\" href=\"#\" download aria-disabled=\"true\">下载服务端计划</a>\n        <a class=\"button disabled\" id=\"export\" href=\"#\" download aria-disabled=\"true\">下载玩家客户端包</a>\n      </div>\n    </div>\n    <section class=\"panel\" style=\"padding:14px;margin-bottom:16px\"><strong>机体获取方式</strong><p>确认导入会登记物品类别和商城 / 扭蛋头像，不自动送机体或上架。已启用收集扭蛋时，会同步登记机库根形态，变形态不会成为第二台奖品。请在「商城商品」新增商品、设价并应用；或同步到扭蛋目录，再在「扭蛋机」选择槽位、概率、编译并应用。</p><p class=\"note\">玩家需要同版本机体客户端包。离线发布不能直接用于在线商城或扭蛋。导入后仍需按原流程刷新游戏服务缓存。</p><button type=\"button\" id=\"sync-capsules\">同步已确认机体与扭蛋头像</button></section><div id=\"status\" role=\"status\" aria-live=\"polite\"></div>\n    <div class=\"layout\">\n      <aside class=\"panel sidebar\">\n        <div class=\"upload-zone\" id=\"drop-zone\">\n          <strong>拖入机体制作包</strong>\n          <span class=\"note\" id=\"archive-name\">支持 .rar、.zip、.sdunit.zip</span>\n          <input id=\"archive\" type=\"file\" accept=\".rar,.zip,.sdunit.zip,application/zip,application/vnd.rar\">\n          <button type=\"button\" id=\"choose-archive\">选择文件并解析</button>\n          <div class=\"progress hidden\" id=\"progress\"><span id=\"progress-bar\"></span></div>\n        </div>\n        <div class=\"side-head\">\n          <h2>导入草稿</h2>\n          <span class=\"badge muted\" id=\"draft-count\">0</span>\n        </div>\n        <div class=\"draft-list\" id=\"draft-list\"></div>\n      </aside>\n      <section class=\"panel editor\" id=\"editor\">\n        <div class=\"empty\" id=\"empty\">上传制作包，或从左侧选择一个已有草稿。</div>\n        <form id=\"editor-form\" class=\"hidden\" novalidate>\n          <div class=\"editor-head\">\n            <div>\n              <div class=\"actions\">\n                <h2 id=\"draft-title\">未命名机体包</h2>\n                <span class=\"badge\" id=\"draft-status\">草稿</span>\n              </div>\n              <div class=\"idline\" id=\"draft-id\"></div>\n            </div>\n            <div class=\"actions\">\n              <button type=\"button\" class=\"danger\" id=\"disable\">停用此草稿</button>\n            </div>\n          </div>\n\n          <div class=\"section\">\n            <div class=\"section-title\">\n              <h3>包身份与形态策略</h3>\n              <span class=\"badge warning\" id=\"revision-badge\">内容版本 —</span>\n            </div>\n            <div class=\"form-grid\">\n              <label class=\"field wide\"><span>内容包名称 *</span><input id=\"package-name\" maxlength=\"48\" required></label>\n              <label class=\"field\"><span>机库根形态 *</span><select id=\"root-unit\" required></select></label>\n              <label class=\"field\"><span>原生机体类目模板 *</span>\n                <input id=\"item-template-search\" type=\"search\" data-template-search=\"items\" placeholder=\"按类目编号、机体编号或名称搜索\">\n                <select id=\"item-template\" data-template-select=\"items\" required></select>\n              </label>\n              <label class=\"field wide\"><span>原生行为配置 *</span>\n                <select id=\"behavior-profile\" required>\n                  <option value=\"native-linked-forms-v1\">native-linked-forms-v1 · 原生关联形态</option>\n                  <option value=\"native-single-form-v1\">native-single-form-v1 · 原生单形态</option>\n                </select>\n              </label>\n              <div class=\"note\">根形态决定机库、商城和扭蛋所引用的持有记录。类目模板只用于生成新的 root 类目；后端分配新的类目编号。行为配置只声明编译策略，不代表变形、同步或实战已经通过认证。</div>\n            </div>\n            <div class=\"summary-grid\" id=\"summary\"></div>\n          </div>\n\n          <div class=\"section\">\n            <div class=\"section-title\">\n              <h3>缺失项与校验问题</h3>\n              <span class=\"badge muted\" id=\"requirement-count\">0 项</span>\n            </div>\n            <div class=\"requirements\" id=\"requirements\"></div>\n            <div class=\"upload-tools\">\n              <div class=\"upload-tool\">\n                <label>\n                  <span><strong>补充依赖文件</strong><small class=\"note\">模型、动作、纹理、特效或记录文件</small></span>\n                  <span class=\"button\">选择文件</span>\n                  <input id=\"dependency-upload\" type=\"file\" multiple accept=\".zip,.bin,.mod,.ani,.txr,.zfx,.spr,.json\">\n                </label>\n              </div>\n              <div class=\"upload-tool\">\n                <label>\n                  <span><strong>补充服务端定义</strong><small class=\"note\">版本化、类型化的 JSON 定义</small></span>\n                  <span class=\"button\">上传 JSON</span>\n                  <input id=\"definition-upload\" type=\"file\" accept=\".json,application/json\">\n                </label>\n              </div>\n            </div>\n          </div>\n\n          <div class=\"section\">\n            <div class=\"section-title\">\n              <h3>解析记录与服务端属性</h3>\n              <span class=\"note\">客户端属性只读；服务端字段按后端给出的类型和必填规则编辑。</span>\n            </div>\n            <div class=\"content-columns\">\n              <div>\n                <h3>机体形态</h3>\n                <div class=\"record-stack\" id=\"forms\"></div>\n              </div>\n              <div>\n                <h3>武器记录</h3>\n                <div class=\"record-stack\" id=\"weapons\"></div>\n              </div>\n            </div>\n          </div>\n\n          <div class=\"section\">\n            <div class=\"section-title\">\n              <h3>资源选择与原生界面预览</h3>\n              <span class=\"note\">下方使用既有生成器生成商城和扭蛋图标。请核对图标后确认导入；游戏内试看和实机测试另外记录。</span>\n            </div>\n            <div class=\"panel\" style=\"padding:14px;margin-bottom:14px\"><strong>游戏内试看（可选）</strong><p id=\"native-preview-status\">尚未准备原生预览；资源缩略图不能替代游戏实际界面。</p><div class=\"actions\"><button type=\"button\" id=\"prepare-native\">准备独立预览副本</button><button type=\"button\" id=\"launch-native\" disabled>打开原生预览客户端</button><button type=\"button\" id=\"refresh-native\">检查原生 UI 回执</button><button type=\"button\" id=\"cleanup-native\">清理过期预览副本</button></div><p class=\"note\" id=\"native-preview-instructions\"></p></div><div class=\"preview-grid\">\n              <article class=\"preview-card\">\n                <div class=\"preview-stage\">\n                  <div class=\"preview-fallback\" id=\"shop-fallback\">尚未选择商城展示图</div>\n                  <img id=\"shop-image\" class=\"missing\" alt=\"商城展示图预览\">\n                  <div class=\"preview-copy\">\n                    <strong id=\"shop-unit-name\">未选择根形态</strong>\n                    <span id=\"shop-unit-meta\">等级待解析</span>\n                    <small id=\"shop-caption-preview\"></small>\n                  </div>\n                </div>\n                <div class=\"preview-controls\">\n                  <strong>商品列表图标生成结果</strong><p class=\"note\">与普通商城商品列表使用相同的图标。商品名称、售价和上架由商城商品管理。</p>\n                  <label class=\"field\"><span>使用已解析资源</span><select id=\"shop-asset\"></select></label>\n                  <div class=\"asset-row\">\n                    <input id=\"shop-upload\" type=\"file\" accept=\".png,image/png\">\n                    <span class=\"note\" id=\"shop-file\">也可以单独上传 PNG 展示图</span>\n                    <button type=\"button\" data-upload-role=\"shop\">上传 PNG</button>\n                  </div>\n                  <label class=\"field\"><span>商品图片生成方式</span><select id=\"shop-source-mode\"><option value=\"native\">沿用包内对应位置的原生图片</option><option value=\"selected\">由所选源图生成对应尺寸</option></select></label><p class=\"note\">固定扭蛋卡面由所选源图调用既有卡框生成器；SR 等自定义候选的等级框由游戏原生控件绘制。</p><label class=\"field\"><span>展示说明</span><input id=\"shop-caption\" maxlength=\"100\" placeholder=\"例如：双形态近战型机体\"></label>\n                </div>\n              </article>\n              <article class=\"preview-card\">\n                <div class=\"preview-stage capsule\">\n                  <div class=\"preview-fallback\" id=\"capsule-fallback\">尚未选择扭蛋展示图</div>\n                  <img id=\"capsule-image\" class=\"missing\" alt=\"扭蛋机展示图预览\">\n                  <div class=\"preview-copy\">\n                    <strong id=\"capsule-unit-name\">未选择根形态</strong>\n                    <span id=\"capsule-unit-meta\">等级待解析</span>\n                    <small id=\"capsule-caption-preview\"></small>\n                  </div>\n                </div>\n                <div class=\"preview-controls\">\n                  <strong>扭蛋展示生成结果</strong><p class=\"note\" id=\"capsule-generator-note\"></p>\n                  <label class=\"field\"><span>使用已解析资源</span><select id=\"capsule-asset\"></select></label>\n                  <div class=\"asset-row\">\n                    <input id=\"capsule-upload\" type=\"file\" accept=\".png,image/png\">\n                    <span class=\"note\" id=\"capsule-file\">也可以单独上传 PNG 展示图</span>\n                    <button type=\"button\" data-upload-role=\"capsule\">上传 PNG</button>\n                  </div>\n                  <label class=\"field\"><span>自定义扭蛋候选头像生成方式</span><select id=\"capsule-source-mode\"><option value=\"native\">沿用包内对应位置的原生图片</option><option value=\"selected\">由所选源图生成对应尺寸</option></select></label><p class=\"note\">固定扭蛋卡面由所选源图调用既有卡框生成器；SR 等自定义候选的等级框由游戏原生控件绘制。</p><label class=\"field\"><span>展示说明</span><input id=\"capsule-caption\" maxlength=\"100\" placeholder=\"例如：SR 稀有槽位候选\"></label>\n                </div>\n              </article>\n            </div>\n            <div class=\"confirm-row\">\n              <label class=\"confirm-box\">\n                <input id=\"confirm-shop\" type=\"checkbox\"><span>我已核对本版本生成的商城图标。</span></label>\n              <label class=\"confirm-box\">\n                <input id=\"confirm-capsule\" type=\"checkbox\"><span>我已核对本版本生成的扭蛋图标。</span></label>\n            </div>\n            <p class=\"note\">确认会导入完整机体定义及展示资源；游戏内测试结果另行记录。本模块只登记机体目录并生成双端发布物。商城 SKU、价格和扭蛋概率由对应模块另行设置；若宿主已连接，确认后由后端事务写入目录并提示正常刷新原生缓存，本页面不会重启游戏服务。玩家客户端包可能包含可信运行模块，只用于分发，不能作为 RAR/ZIP 制作源包再次导入。</p>\n          </div>\n\n          <div class=\"footer\">\n            <span class=\"save-state\" id=\"save-state\">没有未保存的修改</span>\n            <div class=\"actions\">\n              <button type=\"submit\" id=\"save\">保存草稿</button>\n              <button type=\"button\" class=\"primary\" id=\"confirm-final\" disabled>确认图标并导入</button>\n            </div>\n          </div>\n        </form>\n      </section>\n    </div>\n  </main>\n</div>\n</div>";
 const document=new Proxy(shadow,{get(t,k){if(k==="createElement")return globalThis.document.createElement.bind(globalThis.document);if(k==="getElementById")return id=>shadow.querySelector("#"+CSS.escape(id));const value=t[k];return typeof value==="function"?value.bind(t):value;}});
 const endpoint=path=>path.startsWith("/api/providers/")?path:"/api/providers/units"+path;
 const fetch=ctx.fetch;
 const window={confirm:globalThis.confirm.bind(globalThis)};

"use strict";
const $=id=>document.getElementById(id);
const has=(object,key)=>Object.prototype.hasOwnProperty.call(object||{},key);
let state={drafts:[],revision:null};
let selectedId=null;
let draft=null;
let dirty=false;
let busy=false;
let loadSequence=0;

class ApiError extends Error{
  constructor(message,status){super(message);this.name="ApiError";this.status=status}
}

function message(text,isError=false){
  $("status").textContent=text||"";
  $("status").classList.toggle("error",Boolean(isError));
}

function setBusy(value){
  busy=Boolean(value);
  $("editor").classList.toggle("busy-cover",busy);
  $("refresh").disabled=busy;
  $("choose-archive").disabled=busy;
  syncActions();
}

async function api(path,method="GET",body=null){
  const options={method,headers:{}};
  if(method!=="GET"){
    options.headers["Content-Type"]="application/json";
    options.headers["X-Catalog-Revision"]=state.revision==null?"":String(state.revision);
    options.body=JSON.stringify(body==null?{}:body);
  }
  const response=await fetch(path,options);
  const raw=await response.text();
  let data={};
  if(raw){
    try{data=JSON.parse(raw)}
    catch(error){data={error:raw}}
  }
  if(!response.ok){
    const prefix=response.status===409?"目录版本已变化，请刷新后核对当前草稿。":"";
    throw new ApiError([prefix,data.error||data.message||"操作失败"].filter(Boolean).join(" "),response.status);
  }
  return data;
}

function statusLabel(status){
  return status==="confirmed"?"已确认":status==="disabled"?"已停用":"草稿";
}

function statusClass(status){
  return status==="confirmed"?"":status==="disabled"?"danger":"warning";
}

function resetConfirmations(){
  $("confirm-shop").checked=false;
  $("confirm-capsule").checked=false;
}

function markDirty(){
  if(!draft||draft.status==="disabled")return;
  dirty=true;
  resetConfirmations();
  $("save-state").textContent="有未保存的修改；原生预览确认已清除";
  $("save-state").classList.add("dirty");
  syncActions();
}

function markClean(){
  dirty=false;
  $("save-state").textContent="没有未保存的修改";
  $("save-state").classList.remove("dirty");
  // Dirty edits display their source art. Once saved, replace it with the
  // revision-bound generator output before icon confirmation is possible.
  if(draft)for(const role of ["shop","capsule"])renderPreview(role);
  syncActions();
}

function absorb(payload,preferredId=null){
  if(!payload||typeof payload!=="object")return;
  if(payload.id&&Array.isArray(payload.forms)&&!has(payload,"draft"))payload={draft:payload};
  for(const [key,value] of Object.entries(payload)){
    if(key!=="draft"&&key!=="session_token")state[key]=value;
  }
  if(!Array.isArray(state.drafts))state.drafts=[];
  const oldRevision=draft&&draft.content_revision;
  if(has(payload,"draft")){
    draft=payload.draft||null;
    if(draft)selectedId=draft.id;
  }
  if(preferredId!=null)selectedId=preferredId;
  if(oldRevision!=null&&draft&&oldRevision!==draft.content_revision)resetConfirmations();
  renderList();
  renderEditor();
  $("connection").textContent=state.revision==null?"目录未连接":"目录版本 "+String(state.revision).slice(0,12);
  $("connection").className="badge "+(state.revision==null?"danger":"");
}

async function refreshDraftAfterWrite(payload){
  const includedDraft=has(payload||{},"draft")||(payload&&payload.id&&Array.isArray(payload.forms));
  absorb(payload);
  if(!includedDraft&&selectedId)await loadDraft(selectedId,false);
}

function renderList(){
  const all=Array.isArray(state.drafts)?state.drafts:[];
  const query=($("project-search")?.value||"").normalize("NFKC").toLowerCase().split(/\s+/).filter(Boolean);
  const drafts=all.filter(row=>query.every(word=>String(row.search_text||row.name||"").normalize("NFKC").toLowerCase().includes(word)));
  $("draft-count").textContent=String(drafts.length);
  $("draft-list").replaceChildren();
  if(!drafts.length){
    const empty=document.createElement("p");
    empty.className="note";
    empty.textContent=query.length?"没有匹配的资料项目，请换一个名称或别名。":"还没有机体导入草稿。";
    $("draft-list").append(empty);
    return;
  }
  for(const item of drafts){
    const button=document.createElement("button");
    button.type="button";
    button.className="draft-item"+(String(item.id)===String(selectedId)?" selected":"");
    const title=document.createElement("strong");
    title.textContent=item.name||"未命名机体包";
    const meta=document.createElement("small");
    const updated=item.updated?serverDateTime(item.updated):"时间未知";
    meta.textContent=statusLabel(item.status)+" · "+updated;
    button.append(title,meta);
    button.addEventListener("click",()=>loadDraft(item.id));
    $("draft-list").append(button);
  }
}

let metadataEditors=null;
function initializeProjectActions(){
  $("editor-form").noValidate=true;
  const search=document.createElement("input");search.type="search";search.id="project-search";
  search.placeholder="搜索名称、别名、作品或型号";search.setAttribute("aria-label","搜索机体资料");
  search.addEventListener("input",renderList);$("draft-list").before(search);
  const row=document.createElement("div");row.className="actions";
  const name=document.createElement("input");name.maxLength=48;name.placeholder="新资料项目名称";
  name.setAttribute("aria-label","资料项目名称");
  const create=document.createElement("button");create.type="button";create.textContent="新建资料项目";
  create.onclick=async()=>{
    if(busy)return;if(dirty){message("请先保存当前草稿，再新建资料项目。",true);return;}
    if(!name.value.trim()){message("请填写资料项目名称。",true);return;}
    setBusy(true);try{
      const result=await api("/api/units/projects","POST",{name:name.value.trim()});
      await refreshDraftAfterWrite(result);name.value="";
      message("资料项目已建立。可以先填写简介、别名和海报；机体记录与两端登记可稍后完成。");
    }catch(error){message(error.message,true);}finally{setBusy(false);}
  };
  const download=document.createElement("a");download.id="export-project";download.textContent="导出制作项目（含资料素材）";
  download.onclick=event=>{if(!draft||dirty){event.preventDefault();message("请先保存资料，再导出制作项目。",true);}};
  row.append(name,create,download);$("choose-archive").parentElement.append(row);
}

function metadataArea(parent,label,value,maximum=4000){
  const field=document.createElement("label");field.className="field";
  const text=document.createElement("span");text.textContent=label;
  const input=document.createElement("textarea");input.maxLength=maximum;input.rows=3;
  input.value=value||"";input.style.width="100%";input.addEventListener("input",markDirty);
  field.append(text,input);parent.append(field);return input;
}

function renderProjectMetadata(){
  let panel=$("project-metadata");
  if(!panel){panel=document.createElement("section");panel.id="project-metadata";$("forms").before(panel);}
  panel.replaceChildren();
  const title=document.createElement("h3");title.textContent="机体资料与项目素材";
  const note=document.createElement("p");note.className="note";
  note.textContent="资料和海报随制作项目打包，扭蛋机可复用。资料可以先导入；成为实际奖品仍需完成机体两端登记。具体内容属于服主私有项目，不进入框架默认包。";
  panel.append(title,note);
  const metadata=draft.metadata||{};const fields=document.createElement("div");fields.className="server-fields";
  const description=metadataArea(fields,"机体简介",metadata.description);
  const gameplay=metadataArea(fields,"本服说明（与原作简介区分）",metadata.gameplay_notes,2000);
  const short=metadataArea(fields,"简短介绍",metadata.short_description,199);
  const aliases=metadataArea(fields,"搜索别名（每行一个）",(metadata.aliases||[]).join("\n"),2048);
  const examples=metadataArea(fields,"搜索示例（每行一个）",(metadata.search_examples||[]).join("\n"),2048);
  const series=blueprintInput(fields,"所属作品",metadata.series);
  const model=blueprintInput(fields,"官方型号",metadata.model_number);
  const english=blueprintInput(fields,"外文名称",metadata.english_name);
  const tags=blueprintInput(fields,"检索标签（逗号分隔）",(metadata.tags||[]).join(", "));
  const sources=metadataArea(fields,"资料出处（每行：标题 | 网址）",(metadata.sources||[]).map(r=>r.title+" | "+r.url).join("\n"),8192);
  const declared=blueprintInput(fields,"资料关联机体编号（可稍后填写）",metadata.declared_unit_id,"number");
  panel.append(fields);
  const poster=document.createElement("div");poster.className="preview-card";
  const heading=document.createElement("strong");heading.textContent="活动扭蛋机海报";
  const picture=document.createElement("img");picture.alt="活动扭蛋机海报预览";picture.style.maxWidth="361px";picture.style.width="100%";
  const current=draft.media?.activity_poster;
  if(current?.asset_sha)picture.src=endpoint("/api/units/assets/"+current.asset_sha);else picture.hidden=true;
  const help=document.createElement("p");help.className="note";
  help.textContent=current?"原图已保留并随项目导出；活动页面使用361×322画布。":"尚未导入海报。选择PNG后会保存原图与制作副本。";
  const upload=document.createElement("input");upload.type="file";upload.accept=".png,image/png";
  upload.setAttribute("aria-label","上传项目活动海报");
  upload.onchange=()=>{if(dirty){message("请先保存文字资料，再上传海报。",true);upload.value="";return;}
    uploadSupplement(upload.files[0],"asset","activity_poster").finally(()=>upload.value="");};
  poster.append(heading,picture,help,upload);panel.append(poster);
  const lines=input=>input.value.split(/\r?\n/).map(v=>v.trim()).filter(Boolean);
  metadataEditors=()=>{
    const prior=metadata.sources||[];
    const refs=lines(sources).map(line=>{
      const separator=line.indexOf("|");
      if(separator<0)throw new Error("资料出处请按“标题 | 网址”填写。");
      const title=line.slice(0,separator).trim(),url=line.slice(separator+1).trim();
      return {...(prior.find(r=>r.url===url)||{kind:"reference"}),title,url};
    });
    return {...metadata,description:description.value,gameplay_notes:gameplay.value,short_description:short.value,aliases:lines(aliases),
      search_examples:lines(examples),series:series.value,model_number:model.value,english_name:english.value,
      tags:tags.value.split(/[,，\n]/).map(v=>v.trim()).filter(Boolean),sources:refs,visibility:"private",
      declared_unit_id:declared.value.trim()?Number(declared.value):null};
  };
  if($("project-search"))$("project-search").placeholder=(metadata.search_examples||[]).length
    ?"搜索示例："+metadata.search_examples.slice(0,3).join(" / "):"搜索名称、别名、作品或型号";
}

function metric(label,value){
  const box=document.createElement("div");
  box.className="metric";
  const strong=document.createElement("strong");
  strong.textContent=value==null?"—":String(value);
  const span=document.createElement("span");
  span.textContent=label;
  box.append(strong,span);
  return box;
}

function renderSummary(){
  const summary=draft&&draft.summary||{};
  $("summary").replaceChildren(
    metric("文件",summary.files),
    metric("机体形态",summary.forms),
    metric("武器",summary.weapons),
    metric("外部武器",summary.external_weapons),
    metric("资源",summary.resources),
    metric("初始化脚本组",summary.native_program_groups||0)
  );
}

function renderNativeContent(){
  let panel=$("native-content");
  if(!panel){
    panel=document.createElement("section");panel.id="native-content";panel.className="section";
    $("summary").after(panel);
  }
  panel.replaceChildren();
  const heading=document.createElement("h3");heading.textContent="配套原生资料";panel.append(heading);
  const help=document.createElement("p");
  help.textContent="技能、浮游、图集和配套定义随制作项目一起保存。可补传包含 manifest.json 的资料 ZIP；不会修改玩家库存或现有发布。";
  panel.append(help);
  const uploadButton=(label,replaceManifest=null)=>{
    const button=document.createElement("button");button.type="button";button.textContent=label;button.dataset.nativeContentUpload="1";
    button.disabled=busy||dirty||draft.status==="disabled";
    button.onclick=()=>{
      const input=document.createElement("input");input.type="file";input.accept=".zip";
      input.onchange=()=>uploadNativeContent(input.files?.[0],replaceManifest);input.click();
    };
    return button;
  };
  panel.append(uploadButton("补充配套资料 ZIP"));
  const groups=draft.native_content_groups||[];
  if(!groups.length){
    const empty=document.createElement("p");empty.textContent="当前项目没有附加的原生依赖资料。";panel.append(empty);return;
  }
  for(const [index,group] of groups.entries()){
    const box=document.createElement("details");box.className="panel";box.style.padding="12px";box.style.marginTop="10px";
    const title=document.createElement("summary");const s=group.summary||{};
    title.textContent="资料组 "+(index+1)+" · 技能 "+(s.skills||0)+" · 浮游 "+(s.funnels||0)+" · 资源 "+(s.resources||0)+(s.unsupported_records?" · 待适配 "+s.unsupported_records:"");
    box.append(title);
    const supplied=document.createElement("p");const parts=[];
    if(s.skills)parts.push("服务端技能定义"+(s.server_skills?"已携带":"待补充"),"技能图集"+(s.skill_atlases?"已携带":"待补充"));
    if(s.funnels)parts.push("服务端浮游定义"+(s.server_funnels?"已携带":"待补充"));
    supplied.textContent=parts.join("；");box.append(supplied);
    const records=document.createElement("ul");
    for(const record of group.records||[]){
      const row=document.createElement("li");
      row.textContent=(record.kind===13?"技能：":record.kind===11?"浮游：":"待适配资料：")+(record.name||String(record.id));
      const metadata=document.createElement("details");const label=document.createElement("summary");label.textContent="编号与布局信息";
      const value=document.createElement("p");value.textContent="编号 "+record.id+"；类型 "+record.kind+"；"+record.bytes+" 字节；"+record.profile;
      metadata.append(label,value);row.append(metadata);records.append(row);
      const definition=(group.server_definitions||[]).find(item=>item.id===record.id);
      if(definition){
        const edit=document.createElement("button");edit.type="button";edit.dataset.nativeServerEdit="1";
        edit.textContent="编辑服务端定义"+(definition.complete?"":"（待补齐）");
        edit.disabled=busy||dirty||draft.status==="disabled";
        edit.onclick=()=>editNativeServerDefinition(group,definition);row.append(edit);
      }
    }
    box.append(records);
    for(const issue of group.requirements||[]){const line=document.createElement("p");line.className="badge warning";line.style.display="block";line.style.whiteSpace="normal";line.style.margin="8px 0";line.textContent=issue.message;box.append(line);}
    box.append(uploadButton("更新此组资料",group.binding.manifest_path));panel.append(box);
  }
  const note=document.createElement("p");note.textContent="这些新增配套定义目前可保存和导出为制作项目，正式发布尚未开放。";panel.append(note);
}

function renderTransformGraph(){
  let panel=$("transform-graph");
  if(!panel){panel=document.createElement("section");panel.id="transform-graph";panel.className="section";$("summary").after(panel);}
  panel.replaceChildren();
  const title=document.createElement("h3");title.textContent="原始形态关系";panel.append(title);
  const graph=draft.form_graph;
  const note=document.createElement("p");note.textContent=graph?.error||"箭头来自包内完整机体记录，保留单向关系。共享后续形态的不同入口会分别显示；实际变形条件和运行支持仍需验证。";
  panel.append(note);
  if(!graph||graph.error)return;
  const names=new Map((draft.forms||[]).map(row=>[row.id,row.name+" ("+row.id+")"]));
  const label=id=>names.get(id)||"外部形态 "+id;
  const list=document.createElement("ul");
  for(const node of graph.nodes){
    const row=document.createElement("li");row.textContent=label(node.id)+(node.transform_target?" → "+label(node.transform_target):" · 没有记录变形目标");list.append(row);
  }
  panel.append(list);
  for(const route of graph.root_routes){
    const path=document.createElement("p");path.textContent="当前根形态按记录可达："+route.path.map(label).join(" → ")+
      (route.end==="cycle"?" → "+label(route.terminal_target):route.end==="external"?" → "+label(route.terminal_target):"");panel.append(path);
    if(route.other_component_ids.length){const other=document.createElement("p");other.className="note";
      other.textContent="同组其他入口／形态："+route.other_component_ids.map(label).join("、")+"。这些记录会保留，不能据此推断它们能从当前入口变出。";panel.append(other);}
  }
}

function editNativeServerDefinition(group,entry){
  if(busy||dirty||!draft)return;
  const draftId=draft.id;
  const dialog=document.createElement("dialog");dialog.id="native-server-editor";
  dialog.style.cssText="width:min(820px,90vw);max-height:88vh;overflow:auto;border:1px solid var(--line);border-radius:12px;padding:24px;background:var(--panel);color:var(--text);font:14px/1.5 Segoe UI,Microsoft YaHei,sans-serif";
  const heading=document.createElement("h3");heading.textContent=entry.name+" · 服务端定义";
  const help=document.createElement("p");help.textContent=entry.note+" 必填项未齐也可保存草稿，导出项目会保留缺项。";
  const mode=document.createElement("select");mode.setAttribute("aria-label","定义方式");
  mode.append(new Option("手动填写必填字段","authored"),new Option("使用完整原版模板","template"));
  const choices=state.templates?.[entry.kind==="skill"?"skills":"funnels"]||[];
  const search=document.createElement("input");search.type="search";search.placeholder="按名称或编号搜索原版模板";search.setAttribute("aria-label","配套定义模板搜索");
  const select=document.createElement("select");select.setAttribute("aria-label","配套定义原版模板");
  const templateArea=document.createElement("div");templateArea.append(search,select);
  let selected=entry.definition?.kind==="template"?String(entry.definition.id):"";
  const manual=Object.fromEntries(entry.fields.map(field=>[field.name,field.value]));
  const fields=document.createElement("div");fields.className="server-fields";
  const inputs=new Map();const labels=new Map();
  for(const field of entry.fields){
    const label=document.createElement("label");label.style.display="grid";label.style.gap="4px";
    const text=document.createElement("span");text.textContent=field.label+" *"+(field.readonly?" · 从客户端读取":"");text.title=field.name;
    const input=document.createElement("input");input.dataset.nativeField=field.name;
    input.type=field.type==="string"?"text":"number";input.step=field.type==="integer"?"1":"any";
    if(field.minimum!=null)input.min=String(field.minimum);
    if(field.maximum!=null)input.max=String(field.maximum);
    if(field.max_length)input.maxLength=field.max_length;
    input.setAttribute("aria-label",field.label);input.placeholder=field.readonly?"从客户端读取":"必填，尚未提供";
    input.addEventListener("input",()=>{if(mode.value==="authored")manual[field.name]=input.value;updateStatus();});
    inputs.set(field.name,input);labels.set(field.name,text);label.append(text,input);fields.append(label);
  }
  const status=document.createElement("p");status.setAttribute("role","status");
  const errors=document.createElement("p");errors.setAttribute("role","alert");errors.style.color="var(--danger)";
  const actions=document.createElement("div");actions.className="actions";
  const cancel=document.createElement("button");cancel.type="button";cancel.textContent="取消";
  const save=document.createElement("button");save.type="button";save.className="primary";save.textContent="保存配套定义";
  actions.append(cancel,save);
  const updateStatus=()=>{
    const missing=entry.fields.filter(field=>!inputs.get(field.name).value.trim());
    const mismatches=entry.fields.filter(field=>field.readonly&&String(field.value)!==inputs.get(field.name).value);
    status.textContent=mode.value==="template"?(mismatches.length?"模板与客户端共享字段不一致："+mismatches.map(field=>field.label).join("、"):
      "模板将完整复制全部服务端字段；保存时再次核对模板版本及两端共享字段。"):
      missing.length?"还有 "+missing.length+" 个必填字段待提供；保存后仍为草稿。":"必填字段已齐；保存时会重新校验。";
  };
  const update=()=>{
    const template=choices.find(row=>String(row.id)===select.value);
    const isTemplate=mode.value==="template";templateArea.hidden=!isTemplate;
    for(const field of entry.fields){
      const input=inputs.get(field.name);
      const key=entry.kind==="skill"?"SkillID":"FunnelID";
      const value=isTemplate?(field.name===key?entry.id:template?.values[field.name]):manual[field.name];
      input.value=value==null?"":String(value);input.readOnly=isTemplate||field.readonly;
      labels.get(field.name).textContent=field.label+" *"+(field.readonly?(isTemplate?" · 两端需一致":" · 从客户端读取"):"");
    }
    updateStatus();
    save.disabled=isTemplate&&(!template||entry.fields.some(field=>field.readonly&&String(field.value)!==inputs.get(field.name).value));
  };
  const renderChoices=()=>{fillTemplateSelect(select,choices,search.value,selected,"请选择原版模板",row=>row.name+" · "+row.id);update();};
  mode.value=entry.definition?.kind==="template"?"template":"authored";
  mode.onchange=update;search.oninput=renderChoices;select.onchange=()=>{selected=select.value;update();};
  cancel.onclick=()=>dialog.close();
  dialog.addEventListener("close",()=>dialog.remove(),{once:true});
  save.onclick=async()=>{
    const template=choices.find(row=>String(row.id)===select.value);
    let definition;
    if(mode.value==="template"){
      if(!template)return;
      definition={kind:"template",id:template.id,sha256:template.sha256};
    }else{
      const values={};
      for(const field of entry.fields){
        const input=inputs.get(field.name);const value=input.value;
        if(input.validity.badInput){input.reportValidity();return;}
        if(!value.trim())continue;
        if(!input.checkValidity()){input.reportValidity();return;}
        values[field.name]=field.type==="string"?value:Number(value);
      }
      definition={kind:"draft",origin:"authored",values};
    }
    setBusy(true);save.disabled=true;cancel.disabled=true;errors.textContent="";
    try{
      const response=await api("/api/units/drafts/"+encodeURIComponent(draftId)+"/native-server-definition","POST",{
        manifest_path:group.binding.manifest_path,kind:entry.kind,id:entry.id,definition
      });
      await refreshDraftAfterWrite(response);dialog.close();
      message("配套服务端定义已保存。缺项和原生兼容检查会继续保留，原发布不变。");
    }catch(error){errors.textContent=error.message;}
    finally{setBusy(false);cancel.disabled=false;update();}
  };
  dialog.addEventListener("cancel",event=>{if(busy)event.preventDefault();});
  dialog.append(heading,help,mode,templateArea,fields,status,errors,actions);
  shadow.append(dialog);renderChoices();dialog.showModal();
}

async function uploadNativeContent(file,replaceManifest){
  if(!file||busy||!draft)return;
  if(dirty){message("请先保存当前修改，再补充配套资料。",true);return;}
  if(!file.name.toLowerCase().endsWith(".zip")){message("请选择配套资料 ZIP。",true);return;}
  setBusy(true);
  try{
    message("正在校验配套原生资料…");
    const data=await fileToBase64(file);
    await refreshDraftAfterWrite(await api("/api/units/drafts/"+encodeURIComponent(draft.id)+"/native-content","POST",{
      filename:file.name,data,...(replaceManifest?{replace_manifest:replaceManifest}:{})
    }));
    message("配套资料已保存。请查看缺项；制作项目导出会包含完整资料和来源证据。");
  }catch(error){message(error.message,true);}finally{setBusy(false);}
}

function renderRequirements(){
  const requirements=Array.isArray(draft&&draft.requirements)?draft.requirements:[];
  $("requirement-count").textContent=requirements.length+" 项";
  $("requirements").replaceChildren();
  const removable=draft&&draft.removable_missing_skills||[];
  for(const entry of removable){
    const button=document.createElement("button");button.type="button";button.dataset.dropMissingSkills="1";
    button.textContent="移除缺失技能 "+entry.skill_id+" 后导入（"+entry.references.map(r=>r.form_id).join(" / ")+"）";
    button.disabled=busy||dirty||draft.status==="disabled";
    button.onclick=()=>dropMissingSkills(entry);
    $("requirements").append(button);
  }
  if(!requirements.length){
    const row=document.createElement("div");
    row.className="requirement";
    const badge=document.createElement("span");
    badge.className="badge";
    badge.textContent="通过";
    const text=document.createElement("div");
    const strong=document.createElement("strong");
    strong.textContent="当前没有后端报告的缺失项";
    const small=document.createElement("small");
    small.textContent="仍需核对根形态、服务端字段和两种展示预览。";
    text.append(strong,small);
    row.append(badge,text);
    $("requirements").append(row);
    return;
  }
  for(const requirement of requirements){
    const level=requirement.level||"warning";
    const row=document.createElement("div");
    row.className="requirement "+level;
    const badge=document.createElement("span");
    badge.className="badge "+(level==="error"?"danger":level==="required"?"warning":"muted");
    badge.textContent=level==="error"?"拒绝":level==="required"?"必填":"提示";
    const text=document.createElement("div");
    const strong=document.createElement("strong");
    strong.textContent=requirement.message||requirement.code||"未命名校验项";
    const details=[];
    if(requirement.code)details.push("代码 "+requirement.code);
    if(requirement.field)details.push("字段 "+requirement.field);
    if(has(requirement,"accept")){
      const accepted=typeof requirement.accept==="string"?requirement.accept:JSON.stringify(requirement.accept);
      details.push("可接受 "+accepted);
    }
    const small=document.createElement("small");
    small.textContent=details.join(" · ");
    text.append(strong);
    if(details.length)text.append(small);
    row.append(badge,text);
    $("requirements").append(row);
  }
}

function recordValue(item,field){
  if(has(field,"value"))return field.value;
  if(item&&item.server&&has(item.server,field.name))return item.server[field.name];
  return null;
}

function createServerField(item,field,group,index){
  const box=document.createElement("div");
  box.className="server-field";
  const label=document.createElement("label");
  const meta=document.createElement("span");
  meta.className="field-meta";
  const name=document.createElement("span");
  name.textContent=(field.label||field.name||"未命名字段")+(field.required?" *":"");
  name.title=field.name||"";
  const type=document.createElement("small");
  type.textContent=(field.type||"string")+(field.readonly?" · 只读":"");
  meta.append(name,type);
  const value=recordValue(item,field);
  let input;
  if(field.type==="bool"){
    input=document.createElement("select");
    input.append(new Option("未填写",""),new Option("是 / true","true"),new Option("否 / false","false"));
    input.value=value===true||value==="true"?"true":value===false||value==="false"?"false":"";
  }else{
    input=document.createElement("input");
    input.type=field.type==="int"||field.type==="float"?"number":"text";
    if(field.type==="int")input.step="1";
    if(field.type==="float")input.step="any";
    input.value=value==null?"":String(value);
  }
  input.required=Boolean(field.required);
  if(field.readonly){
    if(input.tagName==="SELECT")input.disabled=true;
    else input.readOnly=true;
    input.title="此字段由解析结果或后端规则锁定";
  }
  input.dataset.serverField="1";
  input.dataset.group=group;
  input.dataset.index=String(index);
  input.dataset.name=field.name||"";
  input.dataset.valueType=field.type||"string";
  if(!field.readonly){
    input.addEventListener("input",markDirty);
    input.addEventListener("change",markDirty);
  }
  label.append(meta,input);
  box.append(label);
  if(field.source){
    const source=document.createElement("span");
    source.className="source";
    source.textContent="来源："+field.source;
    box.append(source);
  }
  return box;
}

function createTemplateTool(item,group){
  const tool=document.createElement("div");
  tool.className="template-tool";
  const label=document.createElement("label");
  const labelText=document.createElement("span");
  labelText.textContent="从原版模板补齐缺失字段";
  const search=document.createElement("input");
  search.type="search";
  search.placeholder="按编号或名称搜索模板";
  search.dataset.templateSearch=group;
  search.setAttribute("aria-label",(group==="forms"?"机体形态":"武器")+"模板搜索");
  const select=document.createElement("select");
  select.setAttribute("aria-label",(group==="forms"?"机体形态":"武器")+"原版模板");
  select.dataset.templateSelect=group;
  const templates=state.templates&&state.templates[group==="forms"?"units":group]||[];
  let current=item.template_id==null?"":String(item.template_id);
  const renderOptions=()=>fillTemplateSelect(
    select,
    templates,
    search.value,
    current,
    "显式选择模板（不会自动猜）",
    template=>(template.name||"未命名模板")+" · "+template.id
  );
  renderOptions();
  const button=document.createElement("button");
  button.type="button";
  button.textContent="补齐缺项";
  button.dataset.templateAction="1";
  button.dataset.hasSelection=select.value?"1":"0";
  button.disabled=!select.value;
  search.addEventListener("input",renderOptions);
  select.addEventListener("change",()=>{
    current=select.value;
    button.dataset.hasSelection=select.value?"1":"0";
    button.disabled=!select.value||dirty||busy;
  });
  button.addEventListener("click",()=>applyRecordTemplate(group,item.id,select.value));
  label.append(labelText,search,select);
  tool.append(label,button);
  return tool;
}

function renderRecords(targetId,items,group,emptyText){
  const target=$(targetId);
  target.replaceChildren();
  if(!Array.isArray(items)||!items.length){
    const empty=document.createElement("p");
    empty.className="note";
    empty.textContent=emptyText;
    target.append(empty);
    return;
  }
  items.forEach((item,index)=>{
    const details=document.createElement("details");
    details.className="record";
    if(index===0)details.open=true;
    const summary=document.createElement("summary");
    const title=document.createElement("strong");
    title.textContent=item.name||((group==="forms"?"形态 ":"武器 ")+String(item.id));
    const id=document.createElement("span");
    id.className="badge muted";
    id.textContent="ID "+String(item.id);
    summary.append(title,id);
    const body=document.createElement("div");
    body.className="record-body";
    const clientTitle=document.createElement("h4");
    clientTitle.textContent="客户端解析属性（只读）";
    const client=document.createElement("pre");
    client.className="client-json";
    client.textContent=JSON.stringify(item.client||{},null,2);
    const templateTool=createTemplateTool(item,group);
    const serverTitle=document.createElement("h4");
    serverTitle.textContent="服务端定义";
    const fields=document.createElement("div");
    fields.className="server-fields";
    const serverFields=Array.isArray(item.server_fields)?item.server_fields:[];
    if(serverFields.length){
      for(const field of serverFields)fields.append(createServerField(item,field,group,index));
    }else{
      const none=document.createElement("p");
      none.className="note";
      none.textContent="后端没有提供可编辑的服务端字段。";
      fields.append(none);
    }
    body.append(clientTitle,client,templateTool,serverTitle,fields);
    details.append(summary,body);
    target.append(details);
  });
}

let blueprintEditors=[];
function collectBlueprints(){return blueprintEditors.map(read=>read());}

function blueprintInput(parent,label,value,type="text"){
  const box=document.createElement("label");box.className="field";
  const text=document.createElement("span");text.textContent=label;
  const input=document.createElement("input");input.type=type;
  if(type==="number"){input.step="1";input.min="0";}
  input.value=value==null?"":String(value);
  input.addEventListener("input",markDirty);
  box.append(text,input);parent.append(box);return input;
}

function blueprintSelect(parent,label,value,rows,showIds=true){
  const box=document.createElement("label");box.className="field";
  const text=document.createElement("span");text.textContent=label;
  const select=document.createElement("select");
  let current=value==null?"":String(value);
  const search=document.createElement("input");search.type="search";
  search.placeholder="按名称或编号搜索";search.setAttribute("aria-label","搜索"+label);
  const draw=()=>fillTemplateSelect(select,rows,search.value,current,"请选择",row=>row.name+(showIds?" · "+row.id:""));
  draw();search.addEventListener("input",draw);
  select.addEventListener("change",()=>{current=select.value;});
  select.addEventListener("change",markDirty);
  box.append(text);if(rows.length>100)box.append(search);box.append(select);parent.append(box);return select;
}

function renderBlueprints(){
  let panel=$("unit-blueprints");
  if(!panel){
    panel=document.createElement("section");panel.id="unit-blueprints";
    $("weapons").after(panel);
  }
  panel.replaceChildren();blueprintEditors=[];
  const title=document.createElement("h3");title.textContent="设计图配方";
  const note=document.createElement("p");note.className="note";
  note.textContent="可保存自己的配方，或随制作项目导入有来源的设计图和图纸素材。内置兼容模板用于生成服务器配置，上传的展示模板负责客户端卡面与名称。当前支持3至5个材料，第一个是保留的关键机体，其余材料会消耗；图纸本身也必须有获取途径。需要新原生适配的配方暂时只能保存为草稿。";
  const add=document.createElement("button");add.type="button";add.textContent="添加设计图";
  add.dataset.blueprintLocked=draft.root_unit_id?"0":"1";
  add.disabled=busy||draft.status==="disabled"||!draft.root_unit_id;
  add.onclick=()=>{
    const rows=collectBlueprints();
    rows.push({schema_version:1,id:null,enabled:true,name:(draft.name||"机体")+"设计图",
      source:{kind:"authored"},output_unit_id:draft.root_unit_id,materials:[],
      minimum_player_level:null,repay_point:null});
    draft.blueprints=rows;renderBlueprints();markDirty();
  };
  panel.append(title,note,add);
  const number=input=>input.value.trim()===""?null:Number(input.value);
  const sourceNames={authored:"作者设定", "source-recovered":"恢复的源数据",unknown:"待补齐来源"};
  (draft.blueprints||[]).forEach((recipe,index)=>{
    const card=document.createElement("details");card.className="record";card.open=true;
    const summary=document.createElement("summary");summary.textContent=(recipe.name||"设计图")+" · "+(recipe.id||"保存后分配编号");
    const body=document.createElement("div");body.className="record-body";
    const fields=document.createElement("div");fields.className="server-fields";
    const name=blueprintInput(fields,"设计图名称 *",recipe.name);
    const internal=blueprintInput(fields,"图纸服务端内部名称（ASCII） *",recipe.internal_name);
    const origin=blueprintSelect(fields,"来源 *",recipe.source?.kind||"unknown",Object.entries(sourceNames).map(([id,name])=>({id,name})),false);
    const enabled=blueprintSelect(fields,"配方状态",recipe.enabled===false?"0":"1",[{id:"1",name:"启用"},{id:"0",name:"停用（保留已持有图纸定义）"}],false);
    const primary=blueprintSelect(fields,"图鉴主设计图（多份启用配方需指定一份）",recipe.encyclopedia_primary?"1":"0",[{id:"0",name:"非主配方"},{id:"1",name:"图鉴默认显示此配方"}],false);
    const template=blueprintSelect(fields,"服务器配方兼容模板 *",recipe.recipe_template_id,state.templates?.blueprints||[]);
    const sprite=blueprintInput(fields,"配方卡面 SPR（留空复用机体扭蛋图像；绑定展示模板时须与准备记录一致）",recipe.recipe_sprite||"");
    const item=blueprintSelect(fields,"服务器图纸兼容模板 *",recipe.item_template_id,state.templates?.blueprint_items||[]);
    const clientManifest=blueprintInput(fields,"客户端展示模板清单路径",recipe.client_templates?.manifest_path||"");
    const clientManifestHash=blueprintInput(fields,"客户端展示模板清单 SHA-256",recipe.client_templates?.manifest_sha256||"");
    const templateStatus=document.createElement("p");templateStatus.className="note";
    const templateField="blueprints."+(recipe.id||recipe.name||"draft")+".client_templates";
    const templateIssue=(draft.requirements||[]).find(row=>row.code==="blueprint_template_binding"&&row.field===templateField);
    const semanticNote=(draft.requirements||[]).find(row=>["blueprint_template_semantic_closure","blueprint_template_native_ui"].includes(row.code)&&row.field===templateField);
    if(templateIssue)templateStatus.textContent="客户端展示模板校验失败："+templateIssue.message;
    else if(recipe.client_templates)templateStatus.textContent=semanticNote?.message||
      "客户端展示模板已绑定；保存时会核对来源证据和准备图片。v2 还会检查每帧使用的纹理是否完整、可解码；上线前请在游戏中确认显示和交互。";
    else templateStatus.textContent="未绑定客户端展示模板：使用所选内置兼容模板的卡面与名称。";
    const level=blueprintInput(fields,"玩家最低等级 *（0为不限制）",recipe.minimum_player_level,"number");level.max="255";
    const repay=blueprintInput(fields,"回收图纸返还点数 *",recipe.repay_point,"number");
    const sourceDomain=blueprintInput(fields,"恢复来源域（恢复数据必填）",recipe.source?.domain);
    const sourceId=blueprintInput(fields,"源记录编号（恢复数据必填）",recipe.source?.record_id,"number");
    const sourceHash=blueprintInput(fields,"源记录 SHA-256（恢复数据必填）",recipe.source?.record_sha256);
    const materialArea=document.createElement("div");const materialInputs=[];
    function materialRow(material,slot){
      const row=document.createElement("div");row.className="server-fields";
      const unit=blueprintSelect(row,slot===0?"关键机体 *":"材料机体 "+(slot+1)+" *",material.unit_id,state.templates?.units||[]);
      const required=blueprintInput(row,"最低机体等级 *",material.minimum_level,"number");required.min="1";required.max="255";
      materialArea.append(row);materialInputs.push(()=>({unit_id:number(unit),minimum_level:number(required)}));
    }
    (recipe.materials||[]).forEach(materialRow);
    const read=()=>{
      const source={...(recipe.source||{}),kind:origin.value};
      for(const [key,value] of Object.entries({domain:sourceDomain.value.trim(),record_id:number(sourceId),record_sha256:sourceHash.value.trim()})){
        if(value==null||value==="")delete source[key];else source[key]=value;
      }
      const result={...recipe,name:name.value.trim(),internal_name:internal.value.trim(),source,enabled:enabled.value!=="0",encyclopedia_primary:primary.value==="1",
        recipe_template_id:number(template),item_template_id:number(item),recipe_sprite:sprite.value.trim()||null,
        output_unit_id:draft.root_unit_id,minimum_player_level:number(level),repay_point:number(repay),
        materials:materialInputs.map(read=>read())};
      const manifestPath=clientManifest.value.trim(),manifestHash=clientManifestHash.value.trim();
      if(manifestPath||manifestHash)result.client_templates={manifest_path:manifestPath,manifest_sha256:manifestHash};
      else delete result.client_templates;
      return result;
    };
    blueprintEditors.push(read);
    const addMaterial=document.createElement("button");addMaterial.type="button";addMaterial.textContent="添加材料槽（最多5个）";
    addMaterial.disabled=materialInputs.length>=5;
    addMaterial.dataset.blueprintLocked=addMaterial.disabled?"1":"0";
    addMaterial.onclick=()=>{const rows=collectBlueprints();rows[index].materials.push({unit_id:null,minimum_level:null});draft.blueprints=rows;renderBlueprints();markDirty();};
    const removeMaterial=document.createElement("button");removeMaterial.type="button";removeMaterial.textContent="移除最后一个材料槽";
    removeMaterial.disabled=!materialInputs.length;
    removeMaterial.dataset.blueprintLocked=removeMaterial.disabled?"1":"0";
    removeMaterial.onclick=()=>{const rows=collectBlueprints();rows[index].materials.pop();draft.blueprints=rows;renderBlueprints();markDirty();};
    const remove=document.createElement("button");remove.type="button";remove.textContent="删除未发布的设计图";
    remove.disabled=(draft.published?.blueprints||[]).some(r=>r.id===recipe.id);
    remove.dataset.blueprintLocked=remove.disabled?"1":"0";
    remove.onclick=()=>{const rows=collectBlueprints();rows.splice(index,1);draft.blueprints=rows;renderBlueprints();markDirty();};
    const actions=document.createElement("div");actions.className="actions";actions.append(addMaterial,removeMaterial,remove);
    body.append(fields,templateStatus,materialArea,actions);card.append(summary,body);panel.append(card);
  });
}

function addOption(select,value,label){
  const option=document.createElement("option");
  option.value=value==null?"":String(value);
  option.textContent=label;
  select.append(option);
}

function fillTemplateSelect(select,templates,query,current,blankLabel,labelFor){
  const needle=String(query||"").trim().toLowerCase();
  const matches=[];
  const exact=needle?(templates||[]).find(template=>String(template.id).toLowerCase()===needle):null;
  if(exact)matches.push(exact);
  for(const template of templates||[]){
    if(exact&&String(template.id)===String(exact.id))continue;
    const haystack=[template.id,template.name,template.unit_id]
      .filter(value=>value!=null).join(" ").toLowerCase();
    if(needle&&!haystack.includes(needle))continue;
    matches.push(template);
    if(matches.length>=100)break;
  }
  const currentTemplate=current
    ?(templates||[]).find(template=>String(template.id)===String(current))
    :null;
  if(currentTemplate&&!matches.some(template=>String(template.id)===String(current))){
    matches.unshift(currentTemplate);
  }
  select.replaceChildren();
  addOption(select,"",blankLabel);
  for(const template of matches)addOption(select,template.id,labelFor(template));
  if(current&&!Array.from(select.options).some(option=>option.value===String(current))){
    addOption(select,current,"当前模板 · "+current);
  }
  select.value=current||"";
}

function renderIdentity(){
  $("package-name").value=draft.name||"";
  $("draft-title").textContent=draft.name||"未命名机体包";
  $("draft-id").textContent="草稿 "+String(draft.id)+" · 根形态 "+(draft.root_unit_id==null?"未选择":draft.root_unit_id)+" · 新类目 "+(draft.category_id==null?"待分配":draft.category_id);
  $("draft-status").textContent=statusLabel(draft.status);
  $("draft-status").className="badge "+statusClass(draft.status);
  $("revision-badge").textContent="内容版本 "+String(draft.content_revision==null?"—":draft.content_revision).slice(0,16);
  const root=$("root-unit");
  root.replaceChildren();
  addOption(root,"","请选择根形态");
  for(const form of draft.forms||[]){
    addOption(root,form.id,(form.name||"未命名形态")+" · "+form.id);
  }
  root.value=draft.root_unit_id==null?"":String(draft.root_unit_id);
  const profile=$("behavior-profile");
  const onlyForm=(draft.forms||[]).length===1?draft.forms[0]:null;
  const transformTarget=onlyForm&&(onlyForm.transform_target??findClientValue(onlyForm.client||{},[
    "UnitNumber_Transform","transform_target","transform_target_id","transform_unit","transform_unit_id"
  ]));
  const nativeSingle=Boolean(onlyForm)&&transformTarget!=null&&Number(transformTarget)===0;
  profile.replaceChildren();
  if(nativeSingle){
    addOption(profile,"native-single-form-v1","无变形 · 后端自动采用 native-single-form-v1");
    profile.value="native-single-form-v1";
    profile.disabled=true;
    profile.required=false;
  }else{
    addOption(profile,"","请选择原生行为配置");
    addOption(profile,"native-linked-forms-v1","native-linked-forms-v1 · 原生关联形态（双向变形）");
    addOption(profile,"native-one-way-forms-v1","native-one-way-forms-v1 · 原生单向形态（终态不可变回）");
    addOption(profile,"native-single-form-v1","native-single-form-v1 · 原生单形态");
    if(draft.behavior_profile&&!Array.from(profile.options).some(option=>option.value===draft.behavior_profile)){
      addOption(profile,draft.behavior_profile,draft.behavior_profile+" · 后端提供");
    }
    profile.value=draft.behavior_profile||"";
    profile.disabled=false;
    profile.required=true;
  }
  const itemTemplate=$("item-template");
  const itemSearch=$("item-template-search");
  const itemTemplates=state.templates&&state.templates.items||[];
  let currentItemTemplate=draft.item_template_id==null?"":String(draft.item_template_id);
  itemSearch.value="";
  const renderItemOptions=()=>fillTemplateSelect(
    itemTemplate,
    itemTemplates,
    itemSearch.value,
    currentItemTemplate,
    "请选择原生机体类目模板",
    template=>{
      const unit=template.unit_id==null?"":" · 机体 "+template.unit_id;
      return(template.name||"未命名类目")+" · "+template.id+unit;
    }
  );
  itemSearch.oninput=renderItemOptions;
  itemTemplate.onchange=()=>{currentItemTemplate=itemTemplate.value};
  renderItemOptions();
}

function availablePreviewAssets(){
  const result=new Map();
  for(const asset of draft&&draft.assets||[])result.set(asset.sha,asset);
  for(const asset of state.assets||[]){
    if(asset.kind==="image"||asset.kind==="png"||asset.kind==="preview")result.set(asset.sha,asset);
  }
  return Array.from(result.values()).filter(asset=>asset&&asset.sha);
}

function assetLabel(asset){
  return (asset.filename||asset.sha||"未命名资源")+(asset.kind?" · "+asset.kind:"");
}

function renderAssetSelect(role){
  const select=$(role+"-asset");
  const selected=draft.presentation&&draft.presentation[role]&&draft.presentation[role].asset_sha||"";
  select.replaceChildren();
  addOption(select,"","请选择展示资源");
  for(const asset of availablePreviewAssets())addOption(select,asset.sha,assetLabel(asset));
  if(selected&&!Array.from(select.options).some(option=>option.value===selected)){
    addOption(select,selected,"已选择资源 · "+selected.slice(0,12));
  }
  select.value=selected;
}

function findClientValue(client,candidates){
  if(!client||typeof client!=="object")return null;
  const keys=Object.keys(client);
  for(const candidate of candidates){
    const key=keys.find(value=>value.toLowerCase()===candidate.toLowerCase());
    if(key&&client[key]!=null&&client[key]!=="")return client[key];
  }
  return null;
}

function rootPresentation(){
  const root=(draft.forms||[]).find(form=>String(form.id)===String($("root-unit").value))||null;
  if(!root)return{name:"未选择根形态",meta:"等级待解析"};
  const client=root.client||{};
  const name=findClientValue(client,[
    "display_name","unit_name","UnitName","name","strname","internal_name","UnitModelName"
  ])||root.name||("机体 "+root.id);
  const grade=findClientValue(client,[
    "grade","rank","unit_grade","unit_rank","UnitGrade","UnitRank","UnitGradeNumber"
  ]);
  const subgrade=findClientValue(client,["subgrade","grade2","unit_grade2","UnitGradeNumber2"]);
  const points=findClientValue(client,["points","point","cost","star","Star","BattleCost"]);
  const gradeText=grade==null
    ?"等级待解析"
    :"等级 "+({1:"C",2:"B",3:"A",4:"S"}[Number(grade)]||String(grade))+(subgrade==null?"":" / 子类 "+String(subgrade));
  const pointText=points==null?"":String(points)+" 点";
  return{name:String(name),meta:[gradeText,pointText,"ID "+String(root.id)].filter(Boolean).join(" · ")};
}

function previewUrl(role){
  if(!dirty&&draft.presentation_outputs?.[role])return endpoint(draft.presentation_outputs[role]);
  const selected=$(role+"-asset").value;
  if(selected)return endpoint("/api/units/assets/")+encodeURIComponent(selected);
  return draft.previews&&draft.previews[role]?endpoint(draft.previews[role]):"";
}

function renderPreview(role){
  if(role==="capsule")$("capsule-generator-note").textContent=draft.presentation_contract?.capsule?.kind==="custom"
    ?"此处是生成的候选头像资源；SR 等级边框由游戏原生自定义扭蛋控件绘制，请在自定义机台核对。"
    :"调用现有扭蛋卡面生成器：头像裁切、原生卡框、等级条与稀有剪影。此图与机台编译共用生成逻辑。";
  const identity=rootPresentation();
  $(role+"-unit-name").textContent=identity.name;
  $(role+"-unit-meta").textContent=identity.meta;
  const caption=$(role+"-caption").value.trim();
  $(role+"-caption-preview").textContent=caption?caption:"";
  const url=previewUrl(role);
  const image=$(role+"-image");
  const fallback=$(role+"-fallback");
  if(url){
    image.classList.remove("missing");
    image.src=url;
    fallback.classList.add("hidden");
  }else{
    image.removeAttribute("src");
    image.classList.add("missing");
    fallback.classList.remove("hidden");
  }
}

function renderPresentations(){
  for(const role of ["shop","capsule"]){
    const value=draft.presentation?.[role]||{};const asset=draft.assets?.find(a=>a.sha===value.asset_sha);
    $(role+"-source-mode").value=value.source_mode||(asset?.source==="upload"?"selected":"native");
  }
  for(const role of ["shop","capsule"]){
    renderAssetSelect(role);
    $(role+"-caption").value=draft.presentation&&draft.presentation[role]&&draft.presentation[role].caption||"";
    $(role+"-file").textContent="也可以单独上传 PNG 展示图";
    renderPreview(role);
  }
}

function renderEditor(){
  if(!draft){
    $("empty").classList.remove("hidden");
    $("editor-form").classList.add("hidden");
    syncActions();
    return;
  }
  $("empty").classList.add("hidden");
  $("editor-form").classList.remove("hidden");
  renderIdentity();
  renderSummary();
  renderTransformGraph();
  renderNativeContent();
  renderRequirements();
  renderRecords("forms",draft.forms||[],"forms","制作包中没有解析出机体形态。");
  renderRecords("weapons",draft.weapons||[],"weapons","制作包中没有解析出新武器记录。");
  renderProjectMetadata();
  renderBlueprints();
  renderPresentations();
  renderNativePreview();
  resetConfirmations();
  markClean();
  $("disable").disabled=draft.status==="disabled";
  renderList();
}

function syncActions(){
  const disabled=!draft||draft.status==="disabled";
  $("save").disabled=busy||disabled||!dirty;
  const both=$("confirm-shop").checked&&$("confirm-capsule").checked;
  $("confirm-final").disabled=busy||disabled||dirty||!draft.can_confirm||!both;
  const shopReady=$("shop-image").complete&&$("shop-image").naturalWidth>0;
  const capsuleReady=$("capsule-image").complete&&$("capsule-image").naturalWidth>0;
  $("confirm-final").disabled=$("confirm-final").disabled||!shopReady||!capsuleReady;
  $("prepare-native").disabled=busy||disabled||dirty||!draft.can_confirm;
  $("launch-native").disabled=busy||dirty||!draft?.native_preview?.prepared||Boolean(draft?.native_preview?.error);
  $("confirm-shop").disabled=busy||dirty||!shopReady;
  $("confirm-capsule").disabled=busy||dirty||!capsuleReady;
  const canExport=Boolean(draft&&draft.status==="confirmed");
  $("export").classList.toggle("disabled",!canExport);
  $("export").setAttribute("aria-disabled",canExport?"false":"true");
  $("export").href=canExport?endpoint("/api/units/drafts/"+encodeURIComponent(draft.id)+"/export"):"#";
  $("server-plan").classList.toggle("disabled",!canExport);
  $("server-plan").setAttribute("aria-disabled",canExport?"false":"true");
  $("server-plan").href=canExport?endpoint("/api/units/drafts/"+encodeURIComponent(draft.id)+"/server-plan"):"#";
  document.querySelectorAll("[data-template-action]").forEach(button=>{
    button.disabled=busy||disabled||dirty||button.dataset.hasSelection!=="1";
  });
  document.querySelectorAll("[data-drop-missing-skills]").forEach(button=>button.disabled=busy||dirty||disabled);
  document.querySelectorAll("[data-native-content-upload]").forEach(button=>button.disabled=busy||dirty||disabled);
  document.querySelectorAll("[data-native-server-edit]").forEach(button=>button.disabled=busy||dirty||disabled);
  document.querySelectorAll("#unit-blueprints button").forEach(button=>button.disabled=busy||disabled||button.dataset.blueprintLocked==="1");
  document.querySelectorAll("#unit-blueprints input,#unit-blueprints select").forEach(input=>input.disabled=busy||disabled);
  const project=$("export-project");if(project){project.href=draft?endpoint("/api/units/drafts/"+draft.id+"/project"):"#";project.classList.toggle("disabled",!draft||dirty||busy);}
}

async function dropMissingSkills(entry){
  if(busy||dirty||!draft)return;
  if(!window.confirm("移除缺失技能 "+entry.skill_id+"？受影响形态："+entry.references.map(r=>r.form_id).join(" / ")+"。客户端和服务端将同时清零，这个技能的效果不再生效；原始文件保留，旧预览确认会失效。"))return;
  setBusy(true);
  try{
    await refreshDraftAfterWrite(await api("/api/units/drafts/"+encodeURIComponent(draft.id)+"/drop-missing-skills","POST",{skill_ids:[entry.skill_id]}));
    message("已移除缺失技能 "+entry.skill_id+"。请重新检查商城、扭蛋预览并逐项确认；机体不再具有该技能效果。");
  }catch(error){message(error.message,true);}finally{setBusy(false);}
}

function typedValue(input){
  const type=input.dataset.valueType;
  const raw=input.value;
  if(type==="bool")return raw===""?null:raw==="true";
  if(type==="int")return raw===""?null:Number.parseInt(raw,10);
  if(type==="float")return raw===""?null:Number(raw);
  return raw===""?null:raw;
}

function collectServerRecords(items,group){
  const servers=(items||[]).map(item=>Object.assign({},item.server||{}));
  document.querySelectorAll('[data-server-field][data-group="'+group+'"]').forEach(input=>{
    const index=Number(input.dataset.index);
    if(!servers[index])servers[index]={};
    servers[index][input.dataset.name]=typedValue(input);
  });
  return(items||[]).map((item,index)=>({id:item.id,server:servers[index]||{}}));
}

function collectPatch(){
  const rootValue=$("root-unit").value;
  const itemTemplateValue=$("item-template").value;
  const behaviorValue=$("behavior-profile").value;
  return{
    name:$("package-name").value.trim(),
    root_unit_id:rootValue===""?null:Number(rootValue),
    item_template_id:itemTemplateValue===""?null:Number(itemTemplateValue),
    behavior_profile:behaviorValue===""?null:behaviorValue,
    forms:collectServerRecords(draft.forms||[],"forms"),
    weapons:collectServerRecords(draft.weapons||[],"weapons"),
    ...(metadataEditors?{metadata:metadataEditors()}:{}),
    ...(draft.blueprints?{blueprints:collectBlueprints()}:{}),
    presentation:{
      shop:{asset_sha:$("shop-asset").value||null,source_mode:$("shop-source-mode").value,caption:$("shop-caption").value.trim()},
      capsule:{asset_sha:$("capsule-asset").value||null,source_mode:$("capsule-source-mode").value,caption:$("capsule-caption").value.trim()}
    }
  };
}

function fileToBase64(file,onProgress){
  return new Promise((resolve,reject)=>{
    const reader=new FileReader();
    reader.onerror=()=>reject(reader.error||new Error("读取文件失败"));
    reader.onprogress=event=>{
      if(event.lengthComputable&&onProgress)onProgress(Math.round(event.loaded/event.total*82));
    };
    reader.onload=()=>{
      const value=String(reader.result||"");
      const comma=value.indexOf(",");
      resolve(comma>=0?value.slice(comma+1):value);
    };
    reader.readAsDataURL(file);
  });
}

function showProgress(value){
  $("progress").classList.remove("hidden");
  $("progress-bar").style.width=Math.max(0,Math.min(100,value))+"%";
}

function hideProgress(){
  setTimeout(()=>{
    $("progress").classList.add("hidden");
    $("progress-bar").style.width="0";
  },500);
}

async function inspectArchive(file){
  if(!file)return;
  const lower=file.name.toLowerCase();
  if(!(lower.endsWith(".rar")||lower.endsWith(".zip"))){
    message("请选择 RAR、ZIP 或 .sdunit.zip 制作包。",true);
    return;
  }
  $("archive-name").textContent=file.name;
  setBusy(true);
  showProgress(2);
  try{
    message("正在读取 "+file.name+"…");
    const data=await fileToBase64(file,showProgress);
    showProgress(88);
    const response=await api("/api/units/inspect","POST",{filename:file.name,data});
    showProgress(100);
    await refreshDraftAfterWrite(response);
    message("解析完成。请补齐必填字段，核对客户端记录、服务端定义和两种展示预览。");
  }catch(error){
    message(error.message,true);
  }finally{
    setBusy(false);
    hideProgress();
    $("archive").value="";
  }
}

async function loadDraft(id,announce=true){
  const sequence=++loadSequence;
  selectedId=id;
  renderList();
  setBusy(true);
  try{
    const response=await api("/api/units/drafts/"+encodeURIComponent(id));
    if(sequence!==loadSequence)return;
    absorb(response,id);
    if(announce)message("已读取草稿。");
  }catch(error){
    if(sequence===loadSequence)message(error.message,true);
  }finally{
    if(sequence===loadSequence)setBusy(false);
  }
}

async function refreshCatalog(){
  const keep=selectedId;
  setBusy(true);
  try{
    const response=await api("/api/units");
    absorb(response,keep);
    const available=(state.drafts||[]).some(item=>String(item.id)===String(keep));
    if(keep&&available&&!(response.draft&&String(response.draft.id)===String(keep))){
      await loadDraft(keep,false);
    }else if(!draft&&state.drafts.length){
      const preferred=state.drafts.find(item=>item.status!=="disabled")||state.drafts[0];
      await loadDraft(preferred.id,false);
    }
    message("目录已刷新。");
  }catch(error){
    message(error.message,true);
  }finally{
    setBusy(false);
  }
}

async function uploadSupplement(file,kind,role=null){
  if(!draft||!file)return;
  resetConfirmations();
  syncActions();
  setBusy(true);
  try{
    message("正在读取 "+file.name+"…");
    const data=await fileToBase64(file);
    let path,body;
    if(kind==="definition"){
      path="/api/units/drafts/"+encodeURIComponent(draft.id)+"/definitions";
      body={filename:file.name,data};
    }else{
      path="/api/units/drafts/"+encodeURIComponent(draft.id)+"/asset";
      body={filename:file.name,data,role:role||"dependency"};
    }
    const response=await api(path,"POST",body);
    await refreshDraftAfterWrite(response);
    message(kind==="definition"?"服务端定义已重新解析。":"补充文件已上传并重新校验。");
  }catch(error){
    message(error.message,true);
  }finally{
    setBusy(false);
  }
}

async function applyRecordTemplate(group,id,templateId){
  if(!draft||dirty||!templateId)return;
  resetConfirmations();
  syncActions();
  setBusy(true);
  try{
    const patch={};
    patch[group]=[{id,template_id:Number(templateId)}];
    const response=await api("/api/units/drafts/"+encodeURIComponent(draft.id),"PUT",patch);
    await refreshDraftAfterWrite(response);
    message("已从显式选择的原版模板补齐缺失字段；包内已解析值保持不变，请逐项核对来源和数值。");
  }catch(error){
    message(error.message,true);
  }finally{
    setBusy(false);
  }
}

async function saveDraft(event){
  event.preventDefault();
  if(!draft||!dirty)return;
  setBusy(true);
  try{
    const response=await api("/api/units/drafts/"+encodeURIComponent(draft.id),"PUT",collectPatch());
    await refreshDraftAfterWrite(response);
    message(draft.project_kind==="metadata-only"
      ?"资料已保存，可在扭蛋机的项目素材库中复用。机体记录与两端登记尚未完成。"
      :"草稿已保存。资料与素材可供扭蛋编辑器复用；发布前请核对当前预览。");
  }catch(error){
    message(error.message,true);
  }finally{
    setBusy(false);
  }
}

async function disableDraft(){
  if(!draft||draft.status==="disabled")return;
  if(!window.confirm("停用此草稿？它会保留在目录中，但不能继续确认或导出。"))return;
  setBusy(true);
  try{
    const response=await api("/api/units/drafts/"+encodeURIComponent(draft.id),"DELETE",{});
    await refreshDraftAfterWrite(response);
    message("草稿已软停用，内容记录仍保留。");
  }catch(error){
    message(error.message,true);
  }finally{
    setBusy(false);
  }
}

async function confirmDraft(){
  if(!draft||dirty||!draft.can_confirm||!$("confirm-shop").checked||!$("confirm-capsule").checked)return;
  setBusy(true);
  try{
    const response=await api("/api/units/drafts/"+encodeURIComponent(draft.id)+"/confirm","POST",{
      preview_revision:draft.content_revision,
      shop:true,
      capsule:true
    });
    const confirmed=response&&response.draft?response.draft:response;
    const serverApplied=Boolean(confirmed&&confirmed.server_result&&confirmed.server_result.applied);
    await refreshDraftAfterWrite(response);
    message(serverApplied
      ?"生成图标已确认，服务端定义已应用；原生缓存需按正常流程刷新。现在可以下载玩家客户端包和服务端计划。"
      :"原生界面已核对并登记到本模块目录，但未写入游戏服务端。现在可以下载玩家客户端包和服务端计划。");
  }catch(error){
    resetConfirmations();
    syncActions();
    message(error.message,true);
  }finally{
    setBusy(false);
  }
}

$("choose-archive").addEventListener("click",()=>$("archive").click());
$("archive").addEventListener("change",()=>inspectArchive($("archive").files[0]));
$("refresh").addEventListener("click",refreshCatalog);
$("editor-form").addEventListener("submit",saveDraft);
$("disable").addEventListener("click",disableDraft);
$("confirm-final").addEventListener("click",confirmDraft);
$("confirm-shop").addEventListener("change",syncActions);
$("confirm-capsule").addEventListener("change",syncActions);
for(const id of ["shop-image","capsule-image"]){$(id).addEventListener("load",syncActions);$(id).addEventListener("error",syncActions);}
$("export").addEventListener("click",event=>{
  if($("export").getAttribute("aria-disabled")==="true")event.preventDefault();
});
$("server-plan").addEventListener("click",event=>{
  if($("server-plan").getAttribute("aria-disabled")==="true")event.preventDefault();
});

for(const id of ["package-name","root-unit","item-template","behavior-profile","shop-asset","capsule-asset","shop-source-mode","capsule-source-mode","shop-caption","capsule-caption"]){
  $(id).addEventListener("input",()=>{
    markDirty();
    if(id==="root-unit"||id.endsWith("-asset")||id.endsWith("-caption")){
      renderPreview(id.startsWith("capsule")?"capsule":id.startsWith("shop")?"shop":"shop");
      if(id==="root-unit")renderPreview("capsule");
    }
  });
  $(id).addEventListener("change",()=>{
    markDirty();
    if(id==="root-unit"||id.endsWith("-asset")||id.endsWith("-caption")){
      renderPreview(id.startsWith("capsule")?"capsule":id.startsWith("shop")?"shop":"shop");
      if(id==="root-unit")renderPreview("capsule");
    }
  });
}

for(const role of ["shop","capsule"]){
  $(role+"-image").addEventListener("error",()=>{
    $(role+"-image").classList.add("missing");
    $(role+"-fallback").classList.remove("hidden");
    $(role+"-fallback").textContent="所选资源无法生成 PNG 预览";
  });
  document.querySelector('[data-upload-role="'+role+'"]').addEventListener("click",()=>$(role+"-upload").click());
  $(role+"-upload").addEventListener("change",async()=>{
    const file=$(role+"-upload").files[0];
    if(!file)return;
    $(role+"-file").textContent=file.name;
    await uploadSupplement(file,"asset",role);
    $(role+"-upload").value="";
  });
}

$("dependency-upload").addEventListener("change",async()=>{
  const files=Array.from($("dependency-upload").files||[]);
  for(const file of files)await uploadSupplement(file,"asset","dependency");
  $("dependency-upload").value="";
});

$("definition-upload").addEventListener("change",async()=>{
  const file=$("definition-upload").files[0];
  if(file)await uploadSupplement(file,"definition");
  $("definition-upload").value="";
});

const dropZone=$("drop-zone");
for(const eventName of ["dragenter","dragover"]){
  dropZone.addEventListener(eventName,event=>{
    event.preventDefault();
    dropZone.classList.add("drag");
  });
}
for(const eventName of ["dragleave","drop"]){
  dropZone.addEventListener(eventName,event=>{
    event.preventDefault();
    dropZone.classList.remove("drag");
  });
}
dropZone.addEventListener("drop",event=>{
  const file=event.dataTransfer&&event.dataTransfer.files&&event.dataTransfer.files[0];
  inspectArchive(file);
});

await (async()=>{
  setBusy(true);
  try{
    const response=await api("/api/units");
    absorb(response);
    if(response.draft){
      message("已读取机体导入草稿。");
    }else if(state.drafts.length){
      const preferred=state.drafts.find(item=>item.status!=="disabled")||state.drafts[0];
      await loadDraft(preferred.id,false);
      message("已读取机体导入目录。");
    }else{
      message("请选择一个机体制作包开始解析。");
    }
  }catch(error){
    message(error.message,true);
    $("connection").textContent="目录连接失败";
    $("connection").className="badge danger";
  }finally{
    setBusy(false);
  }
})();

$("sync-capsules").onclick=async()=>{
  if(dirty){message("请先保存当前修改，再同步已确认的发布。",true);return;}
  if(!window.confirm("同步同一服务器已确认的机体与头像到扭蛋目录？不会修改机台、价格或概率。"))return;
  try{
    const target="/api/providers/capsules";
    let response=await ctx.fetch(target+"/api/bootstrap");let data=await response.json();if(!response.ok)throw Error(data.error);
    response=await ctx.fetch(target+"/api/units/import",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({expected_revision:data.catalog_revision})});
    data=await response.json();if(!response.ok)throw Error(data.error);message(data.message||"已同步；请在扭蛋机面板配置机台并编译应用。");
  }catch(error){message(error.message,true);}
};

function renderNativePreview(){
 const current=draft?.native_preview||{};
 $("native-preview-status").textContent=current.error||(!current.prepared?"尚未准备原生预览。":current.ready?"已收到原生显示调用回执；请在游戏窗口核对实际画面后勾选确认。":"等待原生界面：商城 "+(current.shop?"已调用":"待查看")+" / 扭蛋 "+(current.capsule?"已调用":"待查看"));
 $("native-preview-instructions").textContent=[current.client,current.instructions,current.machine_id?(current.machine_kind==="custom"?"自定义预览机台 ":"固定预览机台 ")+current.machine_id+"，选择候选机体查看。":""].filter(Boolean).join(" · ");
}
async function nativeAction(action){
 if(!draft||busy)return;
 if(dirty){message("请先保存修改；原生预览与内容版本绑定。",true);return;}
 setBusy(true);
 const identity=draft.id,revision=draft.content_revision;
 try{
  const result=await api("/api/units/drafts/"+encodeURIComponent(identity)+"/"+action,"POST",{});
  if(draft?.id!==identity||draft.content_revision!==revision)return;
  draft.native_preview=result;
  renderNativePreview();syncActions();
 }catch(error){message(error.message,true);}finally{setBusy(false);}
}
$("prepare-native").onclick=()=>nativeAction("native-preview");
$("launch-native").onclick=()=>nativeAction("launch-preview");
async function refreshNativePreview(){
 if(!draft)return;const identity=draft.id,revision=draft.content_revision;
 const result=await api("/api/units/drafts/"+identity+"/native-preview");
 if(draft?.id!==identity||draft.content_revision!==revision)return;
 draft.native_preview=result;renderNativePreview();syncActions();
}
$("refresh-native").onclick=()=>refreshNativePreview().catch(error=>message(error.message,true));
ctx.setInterval(async()=>{if(!draft?.native_preview?.prepared||busy)return;try{await refreshNativePreview();}catch{}},2000);

$("cleanup-native").onclick=async()=>{if(busy)return;try{const result=await api("/api/units/preview-cleanup","POST",{});message("已清理 "+result.removed_copies+" 个过期副本；保留当前预览和运行中的客户端。");}catch(error){message(error.message,true);}};

initializeProjectActions();

}
