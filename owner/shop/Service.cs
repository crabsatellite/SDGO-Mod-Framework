using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Runtime.InteropServices;

namespace Sdgo.Shop
{
    public sealed class SearchIndexResult
    {
        public bool installed { get; set; }
        public int name_count { get; set; }
        public string revision { get; set; }
    }
    public sealed class ShopService : IDisposable
    {
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern uint GetPrivateProfileString(string section,string key,string fallback,StringBuilder value,uint size,string file);
        static void RequireVersion(string root,string id,Version minimum)
        {
            var text=new StringBuilder(64);
            GetPrivateProfileString("mod","version","",text,64,Path.GetFullPath(Path.Combine(root,"sdgo","mods",id,"mod.ini")));
            Version actual;
            if(!Version.TryParse(text.ToString().Split('-')[0],out actual)||actual<minimum)
                throw new InvalidOperationException("扩展商城需要更新 "+id+" 至 "+minimum+" 或以上版本；请先安装配套程序，尚未写入商品数据库。");
        }
        const long SearchIndexMaximumBytes=8L*1024*1024;
        const long SearchResultMaximumBytes=64L*1024;
        public readonly string DirectoryPath;
        public Settings Settings;
        public Catalog Catalog;
        public Snapshot Snapshot;
        public List<Applied> Applied=new List<Applied>();
        public bool Demo;
        string savedHash;
        Dictionary<string,string> savedProducts;
        readonly FileStream lease;
        readonly DemoRepository demoRepository=new DemoRepository();
        public string CatalogFile { get {return Path.Combine(DirectoryPath,"shop.json");} }
        public string ModDirectory {get {return Path.Combine(Settings.WorldDirectory??"","sdgo","mods","shop_catalog");}}
        public ShopService(string directory,bool demo)
        {
            DirectoryPath=Path.GetFullPath(directory);Directory.CreateDirectory(DirectoryPath);Demo=demo;
            try {lease=new FileStream(Path.Combine(DirectoryPath,"shop-ui.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None);} catch(IOException) {throw new InvalidOperationException("这个配置目录已有商城管理窗口，请使用已打开的窗口。");}
            string profile=Path.Combine(DirectoryPath,"owner-settings.json");Settings=File.Exists(profile)?Model.Read<Settings>(profile):new Settings();
            if(!File.Exists(profile))
            {
                string world=Path.GetFullPath(Path.Combine(DirectoryPath,"..","Server","WorldServer"));
                string client=Path.GetFullPath(Path.Combine(DirectoryPath,"..","..","SDGO-client"));
                if(File.Exists(Path.Combine(world,"WorldServer.ini"))) Settings.WorldDirectory=world;
                if(File.Exists(Path.Combine(client,"GOnline.exe"))) Settings.ClientDirectory=client;
            }
            Catalog=File.Exists(CatalogFile)?Model.Read<Catalog>(CatalogFile):Catalog.Empty();Model.Validate(Catalog);savedHash=Model.FileHash(CatalogFile);CaptureProducts();
        }
        public void Dispose() {lease.Dispose();}
        public void Connect(bool persistSettings=true)
        {
            Snapshot=null;Applied=new List<Applied>();
            IRepository repo=Demo?(IRepository)demoRepository:new SqlRepository(Settings.WorldDirectory);
            var snapshot=repo.Read();var applied=repo.ReadApplied(Catalog.catalog_id);Snapshot=snapshot;Applied=applied;
            if(!Demo&&persistSettings) Model.Save(Path.Combine(DirectoryPath,"owner-settings.json"),Settings);
        }
        public void RefreshApplied()
        {
            if(!Demo) Applied=new SqlRepository(Settings.WorldDirectory).ReadApplied(Catalog.catalog_id);
        }
        void Unchanged() {if(Model.FileHash(CatalogFile)!=savedHash) throw new InvalidOperationException("配置文件已被其他工具修改。请关闭并重新打开窗口后再应用，避免覆盖新内容。");}
        static string ProductState(Product product) {return Model.Json().Serialize(product);}
        void CaptureProducts() {savedProducts=Catalog.products.ToDictionary(p=>p.key,p=>ProductState(p));}
        public void ValidateDraftChinese()
        {
            foreach(var product in Catalog.products.Where(p=>!Model.HasChinese(p.name)||!Model.HasChinese(p.label)))
            {
                string saved;
                if(!savedProducts.TryGetValue(product.key,out saved)) Model.ValidateChinese(product);
                else if(!product.retired&&saved!=ProductState(product)) Model.ValidateChinese(product);
            }
        }
        public void SaveDraft()
        {
            Unchanged();Model.Validate(Catalog,Snapshot);ValidateDraftChinese();Model.Save(CatalogFile,Catalog);
            savedHash=Model.FileHash(CatalogFile);CaptureProducts();
        }
        public bool InitializeDefaultPresets()
        {
            Unchanged();
            if(Catalog.default_presets_version==0&&Catalog.products.Count==0&&Applied.Count!=0)
                throw new InvalidOperationException("服务器已有此商城标识管理的商品。请选择原商城配置目录；未初始化或覆盖默认商品。");
            var candidate=Model.DefaultPresets(Catalog,Snapshot);
            if(candidate==null)return false;
            // Persist assigned identities before the native SQL transaction.
            // On failure these remain reviewable drafts, never silently retried.
            Catalog=candidate;SaveDraft();Apply();
            return true;
        }
        static void Probe(string dir)
        {
            Directory.CreateDirectory(dir);string test=Path.Combine(dir,".shop-write-"+Guid.NewGuid().ToString("N"));
            using(var s=new FileStream(test,FileMode.CreateNew,FileAccess.Write,FileShare.None,1,FileOptions.DeleteOnClose)) s.WriteByte(0);
        }
        public static string QuoteArg(string value)
        {
            var b=new StringBuilder("\"");int slashes=0;
            foreach(char c in value) {if(c=='\\') {slashes++;continue;} if(c=='\"') b.Append('\\',slashes*2+1);else b.Append('\\',slashes);slashes=0;b.Append(c);}
            b.Append('\\',slashes*2);return b.Append('"').ToString();
        }
        static string RequiredFile(string root,string relative,string label,long maximum=0,string maximumText="")
        {
            if(String.IsNullOrWhiteSpace(root)) throw new InvalidOperationException("请先在商城设置中选择"+label+"所在目录。");
            string basePath=Path.GetFullPath(root);
            string file=Path.GetFullPath(Path.Combine(basePath,relative));
            string prefix=basePath.EndsWith(Path.DirectorySeparatorChar.ToString())||
                basePath.EndsWith(Path.AltDirectorySeparatorChar.ToString())?basePath:basePath+Path.DirectorySeparatorChar;
            if(!file.StartsWith(prefix,StringComparison.OrdinalIgnoreCase)||!File.Exists(file))
                throw new InvalidOperationException("所选目录缺少"+label+"。");
            var info=new FileInfo(file);
            if((info.Attributes&FileAttributes.ReparsePoint)!=0)
                throw new InvalidOperationException(label+"不能使用链接文件。");
            if(info.Length<=0) throw new InvalidOperationException(label+"为空。");
            if(maximum>0&&info.Length>maximum)
                throw new InvalidOperationException(label+"超过"+(maximumText==""?"大小限制":maximumText)+"，未执行中文检索更新。");
            return file;
        }
        static SearchIndexResult ReadSearchIndexResult(string file)
        {
            string label="中文检索更新回执";
            string resultFile=RequiredFile(Path.GetDirectoryName(file),Path.GetFileName(file),label,SearchResultMaximumBytes);
            Dictionary<string,object> raw;
            try {raw=Model.Json().Deserialize<Dictionary<string,object>>(File.ReadAllText(resultFile,Encoding.UTF8));}
            catch {throw new InvalidOperationException(label+"无效，服务器效果未确认。");}
            object installedValue,countValue,revisionValue;
            if(raw==null||!raw.TryGetValue("installed",out installedValue)||!(installedValue is bool)||
                !raw.TryGetValue("name_count",out countValue))
                throw new InvalidOperationException(label+"字段不完整，服务器效果未确认。");
            long count;
            try
            {
                if(countValue is bool||countValue is string) throw new InvalidCastException();
                decimal number=Convert.ToDecimal(countValue);
                if(number!=Decimal.Truncate(number)) throw new InvalidCastException();
                count=Convert.ToInt64(number);
            }
            catch {throw new InvalidOperationException(label+"中的名称数量无效，服务器效果未确认。");}
            if(count<0||count>1000000) throw new InvalidOperationException(label+"中的名称数量越界，服务器效果未确认。");
            string revision="";
            if(raw.TryGetValue("revision",out revisionValue)&&revisionValue!=null)
            {
                revision=revisionValue as string;
                if(revision==null||revision.Length>128||revision.IndexOfAny(new[]{'\r','\n','\0'})>=0)
                    throw new InvalidOperationException(label+"中的版本无效，服务器效果未确认。");
            }
            var result=new SearchIndexResult {installed=(bool)installedValue,name_count=(int)count,revision=revision};
            if(!result.installed) throw new InvalidOperationException("服务器未确认中文检索已安装。");
            return result;
        }
        public SearchIndexResult SyncSearchIndex()
        {
            Unchanged();
            if(Demo) return new SearchIndexResult {installed=false,name_count=0,revision=""};
            string host=RequiredFile(Settings.WorldDirectory,"WorldServer.ini","WorldServer.ini");
            string helper=RequiredFile(Settings.WorldDirectory,Path.Combine("sdgo","mods","shop_catalog","shop_catalog_apply.exe"),"商城服务器组件");
            RequiredFile(Settings.ClientDirectory,"GOnline.exe","游戏客户端");
            RequiredFile(Settings.ClientDirectory,Path.Combine("sdgo","mods","shop_localization","shop_localization.dll"),"商城汉化 Mod");
            string searchIndex=RequiredFile(Settings.ClientDirectory,Path.Combine("sdgo","mods","shop_localization","zh-CN.ini"),"商城汉化词典",SearchIndexMaximumBytes," 8 MB");
            string displayProfilePath=Path.Combine(Path.GetFullPath(Settings.ClientDirectory),"sdgo","mods","shop_labels","display.ini");
            string displayProfile=File.Exists(displayProfilePath)?
                RequiredFile(Settings.ClientDirectory,Path.Combine("sdgo","mods","shop_labels","display.ini"),"商城显示配置",SearchIndexMaximumBytes," 8 MB"):null;
            Probe(DirectoryPath);
            string run=Path.Combine(DirectoryPath,"search-index-history",DateTime.UtcNow.ToString("yyyyMMddTHHmmssfffffff")+"-"+Guid.NewGuid().ToString("N").Substring(0,8));
            Directory.CreateDirectory(run);
            string arguments="--host-ini "+QuoteArg(host)+" --search-index "+QuoteArg(searchIndex)+" --output "+QuoteArg(run);
            if(displayProfile!=null) arguments+=" --display-profile "+QuoteArg(displayProfile);
            var start=new ProcessStartInfo(helper) {
                UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,
                StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8,Arguments=arguments
            };
            int exit;
            using(var process=Process.Start(start))
            {
                if(process==null) throw new InvalidOperationException("中文检索更新工具未启动。");
                var stdout=process.StandardOutput.ReadToEndAsync();var stderr=process.StandardError.ReadToEndAsync();
                if(!process.WaitForExit(60000))
                    throw new InvalidOperationException("中文检索更新工具仍在处理，结果尚未确认。请稍后检查服务器状态，勿重复执行。记录目录："+run);
                stdout.Wait();stderr.Wait();exit=process.ExitCode;
            }
            if(exit!=0) throw new InvalidOperationException("服务器未返回可验证的中文检索更新结果。请检查记录目录："+run);
            return ReadSearchIndexResult(Path.Combine(run,"search-index-result.json"));
        }
        public void Apply()
        {
            Unchanged();Model.Validate(Catalog,Snapshot);Model.ValidateChinese(Catalog);
            string displayDir=String.IsNullOrEmpty(Settings.ClientDirectory)?null:Path.Combine(Settings.ClientDirectory,"sdgo","mods","shop_labels");
            string localizationDir=String.IsNullOrEmpty(Settings.ClientDirectory)?null:Path.Combine(Settings.ClientDirectory,"sdgo","mods","shop_localization");
            if(localizationDir!=null&&!File.Exists(Path.Combine(localizationDir,"shop_localization.dll")))
                throw new InvalidOperationException("所选客户端没有安装商城汉化 Mod（shop_localization.dll），请先安装玩家更新包，或清空客户端设置。");
            if(displayDir!=null&&!File.Exists(Path.Combine(displayDir,"shop_labels.dll")))
                throw new InvalidOperationException("所选客户端没有安装显示补丁，请重新选择客户端，或清空客户端设置。");
            bool expanded=Catalog.products.Count(p=>!p.retired)>256||Catalog.products.Any(p=>!p.retired&&(p.product_id%10000<9001||Int32.Parse(p.item_id.Substring(1))<90001));
            if(!Demo&&expanded)
            {
                RequireVersion(Settings.WorldDirectory,"shop_catalog",new Version(0,4,0));
                if(displayDir!=null){RequireVersion(Settings.ClientDirectory,"shop_localization",new Version(0,4,0));RequireVersion(Settings.ClientDirectory,"shop_labels",new Version(0,2,0));}
            }
            if(Demo)
            {
                Applied=Catalog.products.Where(p=>!p.retired).Select(p=>new Applied {Id=p.product_id,Sku=p.item_id,Category=p.category_id,Price=p.price,Validity=p.validity,Currency=p.currency,Visible=p.enabled,Label=p.label,Name=p.name}).ToList();demoRepository.Applied=Applied;
                Snapshot.ActiveProducts=Applied.Where(a=>a.Visible).Select(a=>new ActiveProduct {ProductId=a.Id,OptionId=a.Id,Category=a.Category,Validity=a.Validity}).ToList();
                if(Catalog.default_presets_version==1)Catalog.default_presets_applied=true;
                SaveDraft();return;
            }
            string helper=Path.Combine(ModDirectory,"shop_catalog_apply.exe");
            if(!File.Exists(helper)) throw new InvalidOperationException("所选 WorldServer 尚未安装商城 Mod，请先安装服务器组件。");
            Probe(DirectoryPath);Probe(ModDirectory);if(displayDir!=null) Probe(displayDir);
            string run=Path.Combine(DirectoryPath,"apply-history",DateTime.UtcNow.ToString("yyyyMMddTHHmmssfffffff")+"-"+Guid.NewGuid().ToString("N").Substring(0,8));Directory.CreateDirectory(run);
            string candidate=Path.Combine(run,"catalog.ini");File.WriteAllText(candidate,Model.Ini(Catalog),Encoding.Unicode);Model.Save(Path.Combine(run,"shop.json"),Catalog);
            var start=new ProcessStartInfo(helper) {UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8,
                Arguments="--host-ini "+QuoteArg(Path.Combine(Settings.WorldDirectory,"WorldServer.ini"))+" --catalog "+QuoteArg(candidate)+" --output "+QuoteArg(run)};
            string output,error;int exit;
            using(var process=Process.Start(start))
            {
                var stdout=process.StandardOutput.ReadToEndAsync();var stderr=process.StandardError.ReadToEndAsync();
                int timeout=(int)Math.Min(900000L,120000L+Catalog.products.Count*100L);
                if(!process.WaitForExit(timeout)) throw new InvalidOperationException("应用工具仍在处理，结果尚未确认。请稍后刷新服务器状态，勿重复应用。记录目录："+run);
                output=stdout.Result;error=stderr.Result;exit=process.ExitCode;
            }
            File.WriteAllText(Path.Combine(run,"result.txt"),output+error,Encoding.UTF8);
            if(exit!=0) throw new InvalidOperationException("服务器未报告完整应用成功，请检查记录目录后刷新状态："+run+"\r\n"+error);
            try
            {
                Model.Atomic(Path.Combine(ModDirectory,"catalog.ini"),File.ReadAllBytes(candidate));
                Model.Atomic(Path.Combine(ModDirectory,"runtime-display.ini"),File.ReadAllBytes(Path.Combine(run,"runtime-display.ini")));
                if(Catalog.default_presets_version==1)Catalog.default_presets_applied=true;
                SaveDraft();
                if(displayDir!=null) Model.Atomic(Path.Combine(displayDir,"display.ini"),File.ReadAllBytes(Path.Combine(run,"runtime-display.ini")));
            }
            catch(Exception e) {throw new InvalidOperationException("服务器商品已应用，但本地配置/标签保存未完成。请保留并恢复记录目录："+run+"\r\n"+e.Message);}
            Connect(false);
        }
        public void Import(string file)
        {
            var incoming=Model.Read<Catalog>(file);Model.Validate(incoming,Snapshot);
            if(incoming.catalog_id!=Catalog.catalog_id&&(Catalog.products.Count>0||Applied.Any(a=>a.Visible))) throw new InvalidOperationException("导入配置的标识不同。当前目录已管理商品，请为另一套配置选择单独的管理目录。");
            if(incoming.catalog_id==Catalog.catalog_id)
            {
                incoming.default_presets_version=Catalog.default_presets_version;
                incoming.default_presets_applied=Catalog.default_presets_applied;
                foreach(var old in Catalog.products)
                {
                    var replacement=incoming.products.FirstOrDefault(p=>p.key==old.key);
                    if(replacement!=null&&(replacement.product_id!=old.product_id||replacement.item_id!=old.item_id||replacement.template_id!=old.template_id))
                        throw new InvalidOperationException("导入内容更改了已有商品身份，请新增商品而非复用编号。");
                    if(replacement==null) {var retired=Model.Clone(old);retired.retired=true;retired.enabled=false;incoming.products.Add(retired);}
                }
                Model.Validate(incoming,Snapshot);
            }
            var previous=Catalog;Catalog=incoming;
            try {ValidateDraftChinese();}
            catch {Catalog=previous;throw;}
        }
        public string Status(Product p)
        {
            var a=Applied.FirstOrDefault(x=>x.Id==p.product_id);
            if(p.retired) return a!=null&&a.Visible?"待应用删除":"已删除 / 保留编号";
            if(a==null) return "未应用";
            return a.Matches(p)?(p.enabled?"已上架":"已下架"):"待应用修改";
        }
    }
}
