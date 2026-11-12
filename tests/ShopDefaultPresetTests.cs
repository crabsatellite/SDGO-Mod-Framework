using System;
using System.Linq;
using System.IO;
using Sdgo.Shop;

// Synthetic domain tests; native purchase acceptance is a separate GameTest.
static class ShopDefaultPresetTests
{
    static void Check(bool condition,string text) {if(!condition)throw new Exception(text);}
    static Snapshot Fixture()
    {
        var s=new Snapshot();
        int[] categories={1120001,1110001,1210001,1520001,2250001};
        int[] tabs={1,1,3,2,7};
        for(int i=0;i<categories.Length;i++)
        {
            int id=(100+tabs[i])*10000+1;
            s.Items.Add(new Item{Id=categories[i],Name="虚构测试物品",TimeLimit=i==1||i==2?1:0});
            s.Templates.Add(new Template{Id=id,Tab=tabs[i],Kind=categories[i]/100000,Sku=(i+1)+"00001",Visible=true});
            s.ActiveProducts.Add(new ActiveProduct{ProductId=id,OptionId=i+1,Category=categories[i],Validity=i==1||i==2?3:1,Price=321+i,Currency=i%2==0?"MB":"GB",SingleItem=true});
            s.ProductIds.Add(id);
        }
        return s;
    }
    public static int Main()
    {
        try
        {
            var original=Catalog.Empty();var snapshot=Fixture();
            var generated=Model.DefaultPresets(original,snapshot);
            Check(original.products.Count==0&&original.default_presets_version==0,"generation must not mutate its input");
            Check(generated.products.Count==5&&generated.default_presets_version==1&&!generated.default_presets_applied,"five initial offers await actual apply");
            for(int i=0;i<5;i++)
            {
                var p=generated.products[i];var source=snapshot.ActiveProducts[i];
                Check(p.enabled&&!p.retired&&p.category_id==source.Category&&p.price==source.Price&&p.currency==source.Currency&&p.validity==source.Validity&&p.template_id==source.ProductId,"preset must inherit the actual source template and exact target terms");
            }
            Check(generated.products.Select(p=>p.product_id).Distinct().Count()==5,"IDs must be independently allocated");
            generated.products[0].price=876;generated.products[1].enabled=false;generated.products.RemoveAt(2);
            Check(Model.DefaultPresets(generated,snapshot)==null,"editing, disabling or removing defaults must not recreate/reset them");
            var authored=Catalog.Empty();authored.products.Add(generated.products[0]);
            Check(Model.DefaultPresets(authored,snapshot)==null,"existing authored catalogs must not be applied automatically");
            snapshot.ActiveProducts.Last().Price=0;
            bool refused=false;try{Model.DefaultPresets(original,snapshot);}catch(InvalidOperationException){refused=true;}
            Check(refused&&original.products.Count==0,"missing valid source must refuse the complete initialization");
            using(var service=new ShopService(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"test-data","presets-"+Guid.NewGuid().ToString("N")),true))
            {
                service.Connect();service.Applied.Add(new Applied {Id=1019001,Visible=true});
                bool ownershipRefused=false;
                try{service.InitializeDefaultPresets();}catch(InvalidOperationException e){ownershipRefused=e.Message.Contains("服务器已有");}
                Check(ownershipRefused&&service.Catalog.products.Count==0,"an empty local workspace must not replace an existing server owner's rows");
            }
            Console.WriteLine("DEFAULT SHOP PRESET DOMAIN TESTS PASS");return 0;
        }
        catch(Exception e){Console.Error.WriteLine(e.Message);return 1;}
    }
}
