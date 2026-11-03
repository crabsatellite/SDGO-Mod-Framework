using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Web.Script.Serialization;
using Sdgo.Manager;
using Sdgo.Shop;

public static class ShopSearchIndexTests
{
    public sealed class State
    {
        public Catalog catalog {get;set;}
        public string revision {get;set;}
        public bool demo {get;set;}
        public SearchIndexResult search_index {get;set;}
    }
    public sealed class Response
    {
        public bool ok {get;set;}
        public string error {get;set;}
        public State value {get;set;}
    }
    sealed class Fixture : IDisposable
    {
        public readonly string Root;
        public readonly string Workspace;
        public readonly string World;
        public readonly string Client;
        public readonly string Dictionary;
        public readonly string Helper;
        public Fixture(string root,string mode)
        {
            Root=Path.Combine(root,Guid.NewGuid().ToString("N"));
            Workspace=Path.Combine(Root,"workspace");
            World=Path.Combine(Root,"WorldServer");
            Client=Path.Combine(Root,"client");
            string serverMod=Path.Combine(World,"sdgo","mods","shop_catalog");
            string localization=Path.Combine(Client,"sdgo","mods","shop_localization");
            string labels=Path.Combine(Client,"sdgo","mods","shop_labels");
            Directory.CreateDirectory(Workspace);Directory.CreateDirectory(serverMod);
            Directory.CreateDirectory(localization);Directory.CreateDirectory(labels);
            File.WriteAllText(Path.Combine(World,"WorldServer.ini"),"[DB StoreBilling]\r\nPWD=fixture-secret\r\n",Encoding.ASCII);
            File.WriteAllBytes(Path.Combine(Client,"GOnline.exe"),new byte[]{1});
            File.WriteAllBytes(Path.Combine(localization,"shop_localization.dll"),new byte[]{1});
            Dictionary=Path.Combine(localization,"zh-CN.ini");File.WriteAllText(Dictionary,"MODE="+mode+"\r\n",Encoding.UTF8);
            File.WriteAllText(Path.Combine(labels,"display.ini"),"[shop]\r\nrevision=fixture-display\r\n",Encoding.UTF8);
            Helper=Path.Combine(serverMod,"shop_catalog_apply.exe");
            File.Copy(Assembly.GetExecutingAssembly().Location,Helper);
            string serviceAssembly=typeof(ShopService).Assembly.Location;
            File.Copy(serviceAssembly,Path.Combine(serverMod,Path.GetFileName(serviceAssembly)));
            var catalog=Catalog.Empty();
            catalog.products.Add(new Product {
                key="fixture",product_id=1019001,item_id="190001",template_id=1010001,category_id=1120022,
                name="测试机体",label="永久",price=500,currency="GB",validity=1,enabled=true
            });
            Model.Save(Path.Combine(Workspace,"shop.json"),catalog);
        }
        public ShopService Open()
        {
            var service=new ShopService(Workspace,false);
            service.Settings.WorldDirectory=World;service.Settings.ClientDirectory=Client;
            service.Applied=new List<Applied> {new Applied {
                Id=1019001,Sku="190001",Category=1120022,Price=500,Validity=1,Currency="GB",
                Visible=true,Label="永久",Name="测试机体"
            }};
            return service;
        }
        public void Dispose() {try {Directory.Delete(Root,true);} catch {}}
    }
    static readonly JavaScriptSerializer Json=Model.Json();
    static int checks;
    static void Check(bool value,string name)
    {
        if(!value)throw new Exception(name);
        checks++;Console.WriteLine("PASS "+name);
    }
    static Exception Refuses(Action action,string name)
    {
        try {action();}
        catch(InvalidOperationException error) {Check(true,name);return error;}
        throw new Exception("Accepted invalid state: "+name);
    }
    static string Value(string[] args,string name)
    {
        int index=Array.IndexOf(args,name);
        if(index<0||index+1>=args.Length) throw new Exception("Missing helper argument "+name);
        return args[index+1];
    }
    static int HelperMain(string[] args)
    {
        string host=Value(args,"--host-ini"),search=Value(args,"--search-index"),output=Value(args,"--output");
        if(!File.Exists(host)||!File.Exists(search)||!Directory.Exists(output)) return 20;
        var allowed=new HashSet<string> {"--host-ini","--search-index","--output","--display-profile"};
        for(int i=0;i<args.Length;i+=2)
        {
            if(i+1>=args.Length||!allowed.Contains(args[i])) return 21;
        }
        string display=Array.IndexOf(args,"--display-profile")>=0?Value(args,"--display-profile"):"";
        if(display!=""&&!File.Exists(display)) return 22;
        File.WriteAllLines(Path.Combine(output,"fixture-arguments.txt"),args,Encoding.UTF8);
        string mode=File.ReadAllText(search,Encoding.UTF8);
        if(mode.Contains("MODE=exit"))
        {
            Console.Error.WriteLine("PWD=fixture-secret");
            return 7;
        }
        string result=Path.Combine(output,"search-index-result.json");
        if(mode.Contains("MODE=malformed")) File.WriteAllText(result,"{",Encoding.UTF8);
        else if(mode.Contains("MODE=not-installed")) File.WriteAllText(result,"{\"installed\":false,\"name_count\":73}",Encoding.UTF8);
        else File.WriteAllText(result,"{\"installed\":true,\"name_count\":73,\"revision\":\"fixture-r1\",\"password\":\"must-not-surface\"}",Encoding.UTF8);
        return 0;
    }
    static Response Request(string workspace,string action,string revision="")
    {
        var request=new Dictionary<string,object> {
            {"workspace",workspace},{"world","Z:\\must-not-open"},{"client","Z:\\must-not-open"},
            {"demo","1"},{"action",action},{"body",new Dictionary<string,object>()},{"revision",revision}
        };
        return Json.Deserialize<Response>(ShopApi.Execute(Json.Serialize(request)));
    }
    static void SuccessDoesNotPublishCatalog(string root)
    {
        using(var fixture=new Fixture(root,"success"))
        using(var service=fixture.Open())
        {
            string catalogBefore=Json.Serialize(service.Catalog);
            string appliedBefore=Json.Serialize(service.Applied);
            string fileBefore=Model.FileHash(service.CatalogFile);
            var result=service.SyncSearchIndex();
            Check(result.installed&&result.name_count==73&&result.revision=="fixture-r1","helper confirmation is reduced to the search-index whitelist");
            Check(Json.Serialize(service.Catalog)==catalogBefore&&Model.FileHash(service.CatalogFile)==fileBefore,"search-index action leaves the catalog draft untouched");
            Check(Json.Serialize(service.Applied)==appliedBefore,"search-index action leaves applied products untouched");
            string history=Directory.GetDirectories(Path.Combine(fixture.Workspace,"search-index-history")).Single();
            var arguments=File.ReadAllLines(Path.Combine(history,"fixture-arguments.txt"),Encoding.UTF8);
            Check(arguments.Contains("--search-index")&&arguments.Contains("--display-profile")&&!arguments.Contains("--catalog"),"helper receives only search-index inputs and optional display profile");
            Check(!File.Exists(Path.Combine(history,"catalog.ini"))&&!File.Exists(Path.Combine(history,"shop.json")),"search-index history contains no catalog publication candidate");
        }
        using(var fixture=new Fixture(root,"success"))
        {
            File.Delete(Path.Combine(fixture.Client,"sdgo","mods","shop_labels","display.ini"));
            using(var service=fixture.Open())
            {
                service.SyncSearchIndex();
                string history=Directory.GetDirectories(Path.Combine(fixture.Workspace,"search-index-history")).Single();
                var arguments=File.ReadAllLines(Path.Combine(history,"fixture-arguments.txt"),Encoding.UTF8);
                Check(!arguments.Contains("--display-profile"),"missing display profile is omitted rather than blocking search aliases");
            }
        }
    }
    static void ErrorsNeverClaimSuccess(string root)
    {
        using(var fixture=new Fixture(root,"exit"))
        using(var service=fixture.Open())
        {
            var error=Refuses(()=>service.SyncSearchIndex(),"helper failure rejects the update");
            Check(!error.Message.Contains("fixture-secret")&&!error.Message.Contains("PWD="),"helper output and credentials are not returned in errors");
        }
        using(var fixture=new Fixture(root,"malformed"))
        using(var service=fixture.Open())
        {
            var error=Refuses(()=>service.SyncSearchIndex(),"malformed helper receipt rejects the update");
            Check(error.Message.Contains("未确认")&&!error.Message.Contains("成功"),"malformed receipt never claims effectiveness");
        }
        using(var fixture=new Fixture(root,"not-installed"))
        using(var service=fixture.Open())
        {
            var error=Refuses(()=>service.SyncSearchIndex(),"negative helper receipt rejects the update");
            Check(error.Message.Contains("未确认"),"negative receipt remains an unconfirmed result");
        }
        using(var fixture=new Fixture(root,"success"))
        using(var service=fixture.Open())
        {
            File.Delete(fixture.Dictionary);
            var error=Refuses(()=>service.SyncSearchIndex(),"missing search dictionary rejects the update");
            Check(error.Message.Contains("商城汉化词典"),"missing dictionary error identifies the required client feature");
        }
        using(var fixture=new Fixture(root,"success"))
        using(var service=fixture.Open())
        {
            using(var stream=new FileStream(fixture.Dictionary,FileMode.Create,FileAccess.Write,FileShare.None)) stream.SetLength(8L*1024*1024+1);
            var error=Refuses(()=>service.SyncSearchIndex(),"dictionary over eight megabytes is rejected");
            Check(error.Message.Contains("8 MB"),"dictionary limit is reported without invoking the helper");
        }
    }
    static void DemoAndRevisionGate(string root)
    {
        string workspace=Path.Combine(root,"demo");
        var read=Request(workspace,"read");
        Check(read.ok&&read.value.demo,"demo workspace opens without database or helper access");
        Model.Save(Path.Combine(workspace,"shop.json"),Catalog.Empty());
        var stale=Request(workspace,"search-index",read.value.revision);
        Check(!stale.ok&&stale.error.Contains("刷新"),"stale revision rejects search-index before any action");
        var current=Request(workspace,"read");
        string draft=Json.Serialize(current.value.catalog);
        var simulated=Request(workspace,"search-index",current.value.revision);
        Check(simulated.ok&&simulated.value.search_index!=null&&!simulated.value.search_index.installed,"demo search-index is explicitly non-installed simulation");
        Check(Json.Serialize(simulated.value.catalog)==draft,"demo search-index leaves the catalog draft untouched");
    }
    public static int Main(string[] args)
    {
        if(args.Contains("--search-index")) return HelperMain(args);
        string root=Path.Combine(Path.GetTempPath(),"sdgo-shop-search-index-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            SuccessDoesNotPublishCatalog(root);
            ErrorsNeverClaimSuccess(root);
            DemoAndRevisionGate(root);
            Console.WriteLine("SHOP SEARCH INDEX TESTS PASS: "+checks);
            return 0;
        }
        catch(Exception error)
        {
            Console.Error.WriteLine(error);return 1;
        }
        finally
        {
            try {Directory.Delete(root,true);} catch {}
        }
    }
}
