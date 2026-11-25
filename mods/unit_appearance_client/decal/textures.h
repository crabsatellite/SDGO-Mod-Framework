#pragma once
#include "sdgo/sdgo_api.h"
#include "process_shutdown.h"
#include "../../hangar_manager/native_ui.h"
#include <d3d9.h>
#ifdef small
#undef small
#endif
#include <map>
#include <string>

namespace sdgo::surface_decal::native {
// Independent sticker textures. No indexing past the native model's three
// decal materials: that getter is an unchecked array access in this host.
class Textures {
public:
    bool Bind(const SdgoApi*,SdgoMod*);
    IDirect3DBaseTexture9* Get(std::uint32_t sticker);
    std::wstring Name(std::uint32_t sticker)const;
    void Clear();
    ~Textures(){if(!ProcessShutdown())Clear();}
private:
    using Grid=void*(__cdecl*)();
    using Find=bool(__thiscall*)(void*,const unsigned char**,unsigned,int,char);
    using Allocate=void*(__cdecl*)(unsigned);
    using Construct=void*(__thiscall*)(void*);
    using Load=unsigned char(__thiscall*)(void*,const sdgo::hangar::ui::NativeString*,int,DWORD,int,int,int,DWORD);
    using Delete=void*(__thiscall*)(void*,unsigned);
    struct Entry{void* object=nullptr;IDirect3DBaseTexture9* texture=nullptr;std::uint64_t age=0;std::size_t bytes=0;};
    Grid grid_=nullptr;Find find_=nullptr;Allocate allocate_=nullptr;Construct construct_=nullptr;Load load_=nullptr;Delete delete_=nullptr;
    void* vtable_=nullptr;std::map<std::uint32_t,Entry> entries_;std::uint64_t sequence_=0;std::size_t bytes_=0;
    std::map<std::uint32_t,ULONGLONG> failures_;
    const unsigned char* Record(std::uint32_t)const;
};
}
