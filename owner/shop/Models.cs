using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace Sdgo.Shop
{
    public sealed class Product
    {
        public string key { get; set; }
        public int product_id { get; set; }
        public string item_id { get; set; }
        public int template_id { get; set; }
        public int category_id { get; set; }
        public string name { get; set; }
        public string label { get; set; }
        public int price { get; set; }
        public string currency { get; set; }
        public int validity { get; set; }
        public bool enabled { get; set; }
        public bool retired { get; set; }
    }
    public sealed class Catalog
    {
        public int schema_version { get; set; }
        public string catalog_id { get; set; }
        public List<Product> products { get; set; }
        public int default_presets_version { get; set; }
        public bool default_presets_applied { get; set; }
        public static Catalog Empty() { return new Catalog { schema_version=1, catalog_id="host-shop", products=new List<Product>() }; }
    }
    public sealed class Settings
    {
        public string WorldDirectory { get; set; }
        public string ClientDirectory { get; set; }
    }
    public sealed class Item
    {
        public int Id, TimeLimit, Grade, RefIndex;
        public string Name;
        public int Kind { get { return Id/100000; } }
        public string Mode { get { return TimeLimit==1 ? "租赁 / 有效天数" : Kind==11 ? "永久机体" : "数量"; } }
        public string KindName { get { return Kind==11 ? "机体" : "物品类别 "+Kind; } }
        public string Label(int validity) { return TimeLimit==1 ? (Kind==11 ? "租赁 " : "有效期 ")+validity+" 天" : Kind==11 ? "永久" : "数量 "+validity; }
    }
    public sealed class Template
    {
        public int Id, Tab, Kind;
        public string Sku;
        public bool Visible;
    }
    public sealed class ActiveProduct
    {
        public int ProductId,OptionId,Category,Validity;
        public int Price;
        public string Currency;
        public bool SingleItem;
    }
    public sealed class Snapshot
    {
        public List<Item> Items=new List<Item>();
        public List<Template> Templates=new List<Template>();
        public List<ActiveProduct> ActiveProducts=new List<ActiveProduct>();
        public HashSet<int> ProductIds=new HashSet<int>();
        public HashSet<string> Skus=new HashSet<string>();
    }
    public static class Model
    {
        public static JavaScriptSerializer Json() { return new JavaScriptSerializer { MaxJsonLength=32*1024*1024, RecursionLimit=100 }; }
        public static T Clone<T>(T value) { return Json().Deserialize<T>(Json().Serialize(value)); }
        public static T Read<T>(string file) { return Json().Deserialize<T>(File.ReadAllText(file,Encoding.UTF8)); }
        public static string Hash(byte[] bytes) { using(var h=SHA256.Create()) return BitConverter.ToString(h.ComputeHash(bytes)).Replace("-","").ToLowerInvariant(); }
        public static string FileHash(string file) { return File.Exists(file) ? Hash(File.ReadAllBytes(file)) : ""; }
        public static void Atomic(string file,byte[] data)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file)));
            string pending=file+"."+Guid.NewGuid().ToString("N")+".pending";
            File.WriteAllBytes(pending,data);
            if(File.Exists(file)) File.Replace(pending,file,file+"."+DateTime.UtcNow.ToString("yyyyMMddTHHmmssfffffff")+".bak");
            else File.Move(pending,file);
        }
        public static void Save<T>(string file,T data) { Atomic(file,new UTF8Encoding(false).GetBytes(Json().Serialize(data)+"\n")); }
        public static void Text(string value)
        {
            if(String.IsNullOrEmpty(value)||value.Length>50||value.IndexOfAny(new[]{'\r','\n','\0'})>=0||value.StartsWith("\"")||value.EndsWith("\""))
                throw new InvalidOperationException("商品名称和选项文字需要 1–50 个字符，不可换行或以双引号开头/结尾。");
        }
        public static bool HasChinese(string value)
        {
            return !String.IsNullOrEmpty(value)&&Regex.IsMatch(value,@"[\u3400-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF]");
        }
        public static void Chinese(string value,string field)
        {
            if(String.IsNullOrEmpty(value))
                throw new InvalidOperationException(field+"必须包含至少一个汉字，可以同时保留 MSA-001 等英文或数字代号。");
            Text(value);
            if(!HasChinese(value))
                throw new InvalidOperationException(field+"必须包含至少一个汉字，可以同时保留 MSA-001 等英文或数字代号。");
        }
        public static void ValidateChinese(Product product)
        {
            Chinese(product.name,"商品中文名称");
            Chinese(product.label,"商品销售标签");
        }
        public static void ValidateChinese(Catalog catalog)
        {
            foreach(var product in catalog.products.Where(p=>!p.retired)) ValidateChinese(product);
        }
        public static void Validate(Catalog catalog,Snapshot snapshot=null)
        {
            if(catalog==null||catalog.schema_version!=1||!Key(catalog.catalog_id)||catalog.products==null||catalog.products.Count>4096)
                throw new InvalidOperationException("不支持的商品配置或配置标识。");
            var ids=new HashSet<int>(); var skus=new HashSet<string>(); var keys=new HashSet<string>();
            foreach(var p in catalog.products)
            {
                if(p==null||!Key(p.key)||!keys.Add(p.key)||p.product_id<=0||p.template_id<=0||p.category_id<=0||p.product_id==p.template_id||!ids.Add(p.product_id)||p.item_id==null||!Regex.IsMatch(p.item_id,@"^[0-9]{6}$")||!skus.Add(p.item_id))
                    throw new InvalidOperationException("商品编号重复或无效；请使用“新增商品”自动分配编号。");
                if(p.price<1||p.validity<1||p.validity>1000000||(p.currency!="GB"&&p.currency!="MB")||(p.retired&&p.enabled))
                    throw new InvalidOperationException("价格须为正整数，币种须为 GB / MB，删除的商品不能上架。");
                Text(p.name);Text(p.label);
                if(snapshot!=null&&!p.retired)
                {
                    var item=snapshot.Items.FirstOrDefault(i=>i.Id==p.category_id);
                    var template=snapshot.Templates.FirstOrDefault(t=>t.Id==p.template_id&&t.Kind==p.category_id/100000);
                    if(item==null||template==null||p.product_id<=(template.Tab+100)*10000||p.product_id>=(template.Tab+101)*10000||p.item_id[0]!=template.Sku[0])
                        throw new InvalidOperationException("商品物品或原生模板不适用于当前区服。");
                    if(item.Kind==11&&item.TimeLimit==0&&p.validity!=1) throw new InvalidOperationException("永久机体的数量必须为 1。");
                    if(item.TimeLimit==1&&p.validity>36500) throw new InvalidOperationException("租期最多 36500 天。");
                }
            }
            if(catalog.products.Count(p=>!p.retired)>256) throw new InvalidOperationException("一个配置最多管理 256 件未删除商品。");
        }
        public static bool Key(string key) { return key!=null&&Regex.IsMatch(key,@"^[A-Za-z0-9_.-]{1,64}$"); }
        public static Product Create(Catalog catalog,Snapshot snapshot,Item item)
        {
            return Create(catalog,snapshot,item,item.Name);
        }
        public static Product Create(Catalog catalog,Snapshot snapshot,Item item,string name)
        {
            var t=snapshot.Templates.Where(x=>x.Kind==item.Kind).OrderByDescending(x=>x.Visible).ThenBy(x=>x.Id).FirstOrDefault();
            if(t==null) throw new InvalidOperationException("这类物品尚无可用的原生单物品模板。");
            var ids=new HashSet<int>(snapshot.ProductIds.Concat(catalog.products.Select(p=>p.product_id)));
            var skus=new HashSet<string>(snapshot.Skus.Concat(catalog.products.Select(p=>p.item_id)));
            int id=Enumerable.Range((100+t.Tab)*10000+9001,999).FirstOrDefault(n=>!ids.Contains(n));
            string sku=Enumerable.Range(90001,9999).Select(n=>t.Sku[0]+n.ToString("D5")).FirstOrDefault(s=>!skus.Contains(s));
            if(id==0||sku==null) throw new InvalidOperationException("本页可用的 Mod 商品编号已用完。");
            return new Product {key="p-"+Guid.NewGuid().ToString("N"),product_id=id,item_id=sku,template_id=t.Id,category_id=item.Id,
                name=name,label=item.Label(item.TimeLimit==1 ? 7 : 1),price=500,currency="GB",validity=item.TimeLimit==1 ? 7 : 1,enabled=true};
        }
        public static Catalog DefaultPresets(Catalog catalog,Snapshot snapshot)
        {
            // Initial binding only. An existing or edited catalog is never
            // applied implicitly, and removing a preset does not recreate it.
            if(catalog.default_presets_version!=0||catalog.products.Count!=0)
                return null;
            var candidate=Clone(catalog);
            var keys=new[]{"unit-permanent","unit-rental","operator","blueprint","consumable"};
            var names=new[]{"预设·永久机体","预设·租赁机体","预设·副官","预设·设计图","预设·消耗品"};
            var kinds=new[]{11,11,12,15,22};
            for(int index=0;index<keys.Length;index++)
            {
                int kind=kinds[index];int time=index==0?0:index==1?1:-1;
                var source=snapshot.ActiveProducts.OrderBy(p=>p.ProductId).ThenBy(p=>p.OptionId).FirstOrDefault(p=>
                    p.SingleItem&&p.Price>0&&(p.Currency=="GB"||p.Currency=="MB")&&p.Validity>0&&p.Validity<=1000000&&
                    snapshot.Templates.Any(t=>t.Id==p.ProductId&&t.Kind==kind)&&
                    snapshot.Items.Any(i=>i.Id==p.Category&&i.Kind==kind&&(time<0||i.TimeLimit==time)&&
                        (i.TimeLimit!=1||p.Validity<=36500)&&(kind!=11||i.TimeLimit!=0||p.Validity==1)));
                if(source==null)throw new InvalidOperationException("默认商城预设缺少有效的现货来源："+names[index]+"；未创建或上架任何预设。");
                var item=snapshot.Items.First(i=>i.Id==source.Category);
                // Clone the selected offer's own native template, retaining
                // original eligibility/gifting fields as well as sale terms.
                var nativeSource=new Snapshot {Items=snapshot.Items,ProductIds=snapshot.ProductIds,Skus=snapshot.Skus,
                    Templates=snapshot.Templates.Where(t=>t.Id==source.ProductId&&t.Kind==kind).Take(1).ToList()};
                var product=Create(candidate,nativeSource,item,names[index]);
                product.key="preset-v1-"+keys[index];product.price=source.Price;product.currency=source.Currency;
                product.validity=source.Validity;product.label=item.Label(source.Validity);product.enabled=true;
                candidate.products.Add(product);
            }
            candidate.default_presets_version=1;
            Validate(candidate,snapshot);ValidateChinese(candidate);
            return candidate;
        }
        public static string Ini(Catalog catalog)
        {
            Validate(catalog);ValidateChinese(catalog);var b=new StringBuilder("; Host-owned shop definitions. No credentials.\r\n[shop]\r\nSchema=1\r\nCatalogId="+catalog.catalog_id+"\r\nRevision="+Hash(Encoding.UTF8.GetBytes(Json().Serialize(catalog)))+"\r\n");
            foreach(var p in catalog.products.Where(x=>!x.retired))
                b.AppendFormat("\r\n[product:{0}]\r\nProductId={1}\r\nItemId={2}\r\nTemplateId={3}\r\nCategory={4}\r\nPrice={5}\r\nCurrency={6}\r\nValidity={7}\r\nName={8}\r\nLabel={9}\r\nEnabled={10}\r\n",p.key,p.product_id,p.item_id,p.template_id,p.category_id,p.price,p.currency,p.validity,p.name,p.label,p.enabled?1:0);
            return b.ToString();
        }
        public static byte[] Unicode(string value) { return Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes(value)).ToArray(); }
    }
}
