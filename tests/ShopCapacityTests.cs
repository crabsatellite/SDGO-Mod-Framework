using System;
using System.Linq;
using Sdgo.Shop;

static class ShopCapacityTests
{
    static void Check(bool value,string text) {if(!value)throw new Exception(text);}
    static void Refuses(Action action,string part) {try{action();}catch(InvalidOperationException e){Check(e.Message.Contains(part),e.Message);return;}throw new Exception("Expected refusal: "+part);}
    public static int Main()
    {
        try
        {
            var s=new DemoRepository().Read();var item=s.Items.First(i=>i.TimeLimit==0);
            foreach(int id in Enumerable.Range(1019001,999))s.ProductIds.Add(id);
            foreach(int n in Enumerable.Range(90001,9999))s.Skus.Add("1"+n.ToString("D5"));
            s.ProductIds.Add(1010002);s.Skus.Add("100001");
            var catalog=Catalog.Empty();var p=Model.Create(catalog,s,item,"容量测试");
            Check(p.product_id==1010003&&p.item_id=="100002","fallback must skip every native and ledger reservation");
            p.retired=true;p.enabled=false;catalog.products.Add(p);
            var next=Model.Create(catalog,s,item,"另一个商品");
            Check(next.product_id==1010004&&next.item_id=="100003","retired identities must stay reserved");
            catalog.products=Enumerable.Range(0,5000).Select(i=>new Product{key="large-"+i,product_id=1011000+i,item_id=(130000+i).ToString(),template_id=1010001,category_id=item.Id,name="容量测试",label="永久",price=500,currency="GB",validity=1,enabled=true}).ToList();
            Model.Validate(catalog,s);Model.ValidateChinese(catalog);
            string file=System.IO.Path.GetTempFileName();
            try {Model.Save(file,catalog);Check(Model.Read<Catalog>(file).products.Count==5000,"large workspace round trip");}
            finally {System.IO.File.Delete(file);}
            Check(Model.Ini(catalog).Contains("[product:large-4999]"),"large compile includes final offer");
            string oldRoot=System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"test-data","capacity-old-"+Guid.NewGuid().ToString("N"));
            string world=System.IO.Path.Combine(oldRoot,"world"),module=System.IO.Path.Combine(world,"sdgo","mods","shop_catalog");
            System.IO.Directory.CreateDirectory(module);
            System.IO.File.WriteAllText(System.IO.Path.Combine(module,"mod.ini"),"[mod]\nversion=0.3.0\n");
            using(var service=new ShopService(System.IO.Path.Combine(oldRoot,"state"),false))
            {
                service.Settings.WorldDirectory=world;service.Catalog=catalog;
                Refuses(()=>service.Apply(),"shop_catalog 至 0.4.0");
                Check(!System.IO.Directory.Exists(System.IO.Path.Combine(oldRoot,"state","apply-history")),"legacy runtime refusal precedes native helper and SQL writes");
            }
            foreach(int id in Enumerable.Range(1010001,9999))s.ProductIds.Add(id);
            Refuses(()=>Model.Create(Catalog.Empty(),s,item),"其他商城页");
            var other=new Item{Id=1210001,TimeLimit=0,Name="测试副官"};s.Items.Add(other);
            s.Templates.Add(new Template{Id=1030001,Tab=3,Kind=12,Sku="300001",Visible=true});
            Check(Model.Create(Catalog.Empty(),s,other).product_id==1039001,"full unit tab cannot block operator tab");
            Console.WriteLine("SHOP CAPACITY PASS: 5000 offers, tail fallback, retired reservations and independent tab exhaustion");return 0;
        }catch(Exception e){Console.Error.WriteLine(e);return 1;}
    }
}
