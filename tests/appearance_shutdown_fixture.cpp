// A DLL fixture is required: EXE statics run before ExitProcess, whereas the
// reported hang occurred in DLL statics after Windows stopped other threads.
#include "../mods/unit_appearance_client/decal/textures.cpp"
#include "../mods/unit_appearance_client/decal/render.cpp"
#include <array>

namespace {
struct Counts { unsigned created=0,released=0; };
Counts* counts=nullptr;
using Query=BOOLEAN (NTAPI*)();
const auto shuttingDown=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlDllShutdownInProgress"));
struct Texture final : IDirect3DTexture9 {
    Texture(){++counts->created;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return 1;}
    ULONG STDMETHODCALLTYPE Release() override {
        // Deterministic equivalent of a driver waiting for a worker that
        // ExitProcess has already terminated. Never leave a hanging test.
        if(!shuttingDown||shuttingDown())TerminateProcess(GetCurrentProcess(),91);
        ++counts->released;delete this;return 0;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override{return E_NOTIMPL;}
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override{return 0;}
    DWORD STDMETHODCALLTYPE GetPriority() override{return 0;}
    void STDMETHODCALLTYPE PreLoad() override{}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override{return D3DRTYPE_TEXTURE;}
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override{return 0;}
    DWORD STDMETHODCALLTYPE GetLOD() override{return 0;}
    DWORD STDMETHODCALLTYPE GetLevelCount() override{return 1;}
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override{return E_NOTIMPL;}
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override{return D3DTEXF_NONE;}
    void STDMETHODCALLTYPE GenerateMipSubLevels() override{}
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT,D3DSURFACE_DESC* desc) override{*desc={};desc->Width=desc->Height=4;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT,IDirect3DSurface9**) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE LockRect(UINT,D3DLOCKED_RECT*,const RECT*,DWORD) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override{return E_NOTIMPL;}
};
std::array<unsigned char,512> row{};
int table=0;void* tablePointer=&table;
void* Grid(){return row.data();}
bool __fastcall Find(void*,void*,const unsigned char** out,unsigned,int,char){*out=row.data();return true;}
void* Allocate(unsigned size){return new unsigned char[size]{};}
void* __fastcall Construct(void* object,void*){std::memcpy(object,&tablePointer,sizeof(void*));return object;}
unsigned char __fastcall Load(void* object,void*,const sdgo::hangar::ui::NativeString*,int,DWORD,int,int,int,DWORD){
    auto* texture=new Texture;std::memcpy(static_cast<unsigned char*>(object)+0x14,&texture,sizeof texture);return 1;
}
void* __fastcall Delete(void* object,void*,unsigned){
    Texture* texture=nullptr;std::memcpy(&texture,static_cast<unsigned char*>(object)+0x14,sizeof texture);
    if(texture)texture->Release();delete[] static_cast<unsigned char*>(object);return nullptr;
}
void* Symbol(SdgoMod*,const char* name){
    if(!std::strcmp(name,"grid.data"))return reinterpret_cast<void*>(&Grid);
    if(!std::strcmp(name,"grid.find"))return reinterpret_cast<void*>(&Find);
    if(!std::strcmp(name,"crt.operator_new"))return reinterpret_cast<void*>(&Allocate);
    if(!std::strcmp(name,"surface_decal.texture_construct"))return reinterpret_cast<void*>(&Construct);
    if(!std::strcmp(name,"surface_decal.texture_load"))return reinterpret_cast<void*>(&Load);
    if(!std::strcmp(name,"surface_decal.texture_delete"))return reinterpret_cast<void*>(&Delete);
    if(!std::strcmp(name,"surface_decal.texture_vtable"))return tablePointer;
    return nullptr;
}
sdgo::surface_decal::native::Textures textures;
std::vector<sdgo::surface_decal::render::Mask> masks;
}
extern "C" __declspec(dllexport) bool __cdecl Exercise(Counts* result,bool maskOnly){
    if(!shuttingDown||shuttingDown())return false;
    counts=result;
    if(maskOnly){
        using sdgo::surface_decal::render::Mask;
        // Exercise the production move/eviction and destructor paths too.
        {Mask first;first.texture=new Texture;Mask moved(std::move(first));if(first.texture)return false;}
        if(counts->released!=1)return false;
        masks.emplace_back();masks.back().texture=new Texture;
        Mask replacement;replacement.texture=new Texture;masks.back()=std::move(replacement);
        if(replacement.texture||counts->released!=2)return false;
        return counts->created==3;
    }
    const wchar_t path[]=L"test.dds";std::memcpy(row.data()+0xd1,path,sizeof path);
    SdgoApi api{};api.Symbol=&Symbol;
    if(!textures.Bind(&api,reinterpret_cast<SdgoMod*>(1)))return false;
    auto* first=textures.Get(1);
    if(!first||textures.Get(1)!=first||counts->created!=1)return false;
    for(unsigned i=2;i<=257;++i)if(!textures.Get(i))return false;
    if(counts->created!=257||counts->released!=1)return false; // bounded cache eviction
    textures.Clear();if(counts->released!=257)return false;
    {sdgo::surface_decal::native::Textures local;if(!local.Bind(&api,reinterpret_cast<SdgoMod*>(1))||!local.Get(1))return false;}
    if(counts->released!=258)return false; // ordinary destruction still releases
    return textures.Get(1)&&textures.Get(2)&&counts->created==260&&counts->released==258;
}
