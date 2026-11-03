using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Web.Script.Serialization;
using Sdgo.Manager;
using Sdgo.Shop;

public static class ShopChineseRequiredTests
{
    public sealed class State
    {
        public Catalog catalog {get;set;}
        public string revision {get;set;}
    }
    public sealed class Response
    {
        public bool ok {get;set;}
        public string error {get;set;}
        public State value {get;set;}
    }
    static readonly JavaScriptSerializer Json=Model.Json();
    static int checks;
    static void Check(bool value,string name)
    {
        if(!value)throw new Exception(name);
        checks++;Console.WriteLine("PASS "+name);
    }
    static Response Request(string workspace,string action,object body=null,string revision="")
    {
        var request=new Dictionary<string,object> {
            {"workspace",workspace},{"world",""},{"client",""},{"demo","1"},{"action",action},
            {"body",body??new Dictionary<string,object>()},{"revision",revision}
        };
        return Json.Deserialize<Response>(ShopApi.Execute(Json.Serialize(request)));
    }
    static Catalog LegacyCatalog()
    {
        var catalog=Catalog.Empty();
        catalog.products.Add(new Product {
            key="legacy",product_id=1019001,item_id="190001",template_id=1010001,category_id=1120022,
            name="Gundam MSA-001",label="Permanent",price=500,currency="GB",validity=1,enabled=true
        });
        return catalog;
    }
    public static int Main()
    {
        string root=Path.Combine(Path.GetTempPath(),"sdgo-shop-chinese-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            Check(Model.HasChinese("高达 MSA-001")&&!Model.HasChinese("Gundam MSA-001"),"Chinese detector permits mixed model codes");
            string api=Path.Combine(root,"api");
            var read=Request(api,"read");Check(read.ok,"Manager shop workspace opens");
            var missing=Request(api,"create",new Dictionary<string,object>{{"category_id",1110549}},read.value.revision);
            Check(!missing.ok&&missing.error.Contains("中文"),"Manager API rejects a missing Chinese product name");
            var english=Request(api,"create",new Dictionary<string,object>{{"category_id",1110549},{"name","Gundam MSA-001"}},read.value.revision);
            Check(!english.ok&&english.error.Contains("汉字"),"Manager API rejects an English-only product name");
            var created=Request(api,"create",new Dictionary<string,object>{{"category_id",1110549},{"name","高达 MSA-001"}},read.value.revision);
            Check(created.ok&&created.value.catalog.products.Single().label=="租赁 7 天","Manager creates a Chinese product and keeps its numeric sale label automatic");
            var product=created.value.catalog.products.Single();
            var validity=Request(api,"update",new Dictionary<string,object> {
                {"key",product.key},{"values",new Dictionary<string,object>{{"validity",30},{"label","租赁 7 天"}}}
            },created.value.revision);
            Check(validity.ok&&validity.value.catalog.products.Single().label=="租赁 30 天","Manager API regenerates the numeric sale label when validity changes");
            product=validity.value.catalog.products.Single();
            var badUpdate=Request(api,"update",new Dictionary<string,object> {
                {"key",product.key},{"values",new Dictionary<string,object>{{"name","Gundam MSA-001"}}}
            },validity.value.revision);
            Check(!badUpdate.ok&&badUpdate.error.Contains("中文"),"Manager API rejects removal of the required Chinese name");

            var incoming=Model.Clone(validity.value.catalog);
            var added=Model.Clone(product);added.key="imported";added.product_id++;added.item_id="190002";
            added.name="Imported Gundam";added.label="Permanent";incoming.products.Add(added);
            var badImport=Request(api,"import",new Dictionary<string,object>{{"catalog",incoming}},validity.value.revision);
            Check(!badImport.ok&&badImport.error.Contains("中文"),"Manager import rejects a new definition without Chinese text");
            added.retired=true;added.enabled=false;
            var retiredImport=Request(api,"import",new Dictionary<string,object>{{"catalog",incoming}},validity.value.revision);
            Check(!retiredImport.ok&&retiredImport.error.Contains("中文"),"new retired import entries cannot bypass the Chinese requirement");

            string legacy=Path.Combine(root,"legacy");Directory.CreateDirectory(legacy);
            Model.Save(Path.Combine(legacy,"shop.json"),LegacyCatalog());
            using(var service=new ShopService(legacy,true))
            {
                service.Connect();
                Check(service.Catalog.products.Single().name=="Gundam MSA-001","legacy workspace remains readable without migration or deletion");
                service.SaveDraft();Check(File.Exists(service.CatalogFile),"unchanged legacy draft remains preservable");
                service.Catalog.products[0].price++;
                bool draftRejected=false;try {service.SaveDraft();}catch(InvalidOperationException e) {draftRejected=e.Message.Contains("中文");}
                Check(draftRejected,"modified legacy definition requires Chinese before save");
            }
            using(var service=new ShopService(legacy,true))
            {
                service.Connect();
                bool publishRejected=false;try {service.Apply();}catch(InvalidOperationException e) {publishRejected=e.Message.Contains("中文");}
                Check(publishRejected,"publication rejects every unfinished active legacy definition");
                service.Catalog.products[0].name="高达 MSA-001";service.Catalog.products[0].label="永久";
                service.SaveDraft();service.Apply();
                Check(service.Status(service.Catalog.products[0])=="已上架","completed Chinese definition can be saved and published");
            }
            Console.WriteLine("SHOP CHINESE REQUIRED TESTS PASS: "+checks);
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
