#include "sdgo/sdgo_api.h"
#include "sdgo/friend_unit_gift_protocol.h"
#include "sdgo/surface_decal_sql.h"
#include "../../oc_custom_server/oc_session.h"
#include <fstream>
#include <mutex>

namespace {
using namespace sdgo::surface_decal;
using namespace sdgo::oc_custom::server;
using sdgo::friend_unit_gift::WireStatus;
std::wstring schema;
std::mutex schemaLock;
bool schemaReady=false;
bool Initialize(Session& db) {
    std::lock_guard<std::mutex> hold(schemaLock);
    if(!schemaReady)schemaReady=db.Execute(schema);return schemaReady;
}
std::int32_t __cdecl Handle(void*,std::uint32_t actor,std::uint16_t command,const std::uint8_t* data,std::uint32_t size,
                           void*,void* database,std::uint8_t* output,std::uint32_t capacity,std::uint32_t* produced) {
    if(produced)*produced=0;
    if(!actor||!data||!output||!produced||capacity<wire::appearanceBytes+4)return static_cast<int>(WireStatus::invalid_argument);
    try {
        Appearance proposal;const bool saving=command==wire::save;
        if((saving&&!wire::Decode(data,size,proposal))||(!saving&&(command!=wire::get||size!=8)))return static_cast<int>(WireStatus::invalid_argument);
        const auto uid=saving?proposal.unitSerial:wire::Get(data,8);
        const auto batch=sql::Request(actor,uid,saving?&proposal:nullptr);
        if(batch.empty())return static_cast<int>(WireStatus::invalid_argument);
        Session db(5);if(!db.Open(database)||!Initialize(db))return static_cast<int>(WireStatus::database_unavailable);
        ResultSets sets;if(!db.Query(batch,&sets)||sets.size()!=1||sets[0].size()!=1)return static_cast<int>(WireStatus::database_unavailable);
        const auto& row=sets[0][0];const auto result=Int64Or(row,0,-1);
        if(row.size()==1&&(result==6||result==9))return static_cast<int>(result);
        if(row.size()!=9||(result!=0&&result!=12))return static_cast<int>(WireStatus::database_unavailable);
        Appearance current;current.unitSerial=uid;
        const auto type=Int64Or(row,1,-1),revision=Int64Or(row,2,-1);
        if(type<=0||type>INT32_MAX||revision<=0)return static_cast<int>(WireStatus::database_unavailable);
        current.unitType=static_cast<std::uint32_t>(type);current.revision=static_cast<std::uint64_t>(revision);
        for(unsigned i=0;i<3;++i){const auto id=Int64Or(row,3+i,-1);if(id<0||id>INT32_MAX)return static_cast<int>(WireStatus::database_unavailable);current.slots[i].sticker=static_cast<std::uint32_t>(id);}
        wire::Bytes encoded;if(!wire::Encode(current,encoded))return static_cast<int>(WireStatus::database_unavailable);
        for(unsigned i=0;i<3;++i)if(!row[6+i].empty()&&!sql::Unhex(row[6+i],encoded.data()+wire::headerBytes+i*wire::slotBytes,wire::slotBytes))return static_cast<int>(WireStatus::database_unavailable);
        Appearance decoded;if(!wire::Decode(encoded.data(),encoded.size(),decoded))return static_cast<int>(WireStatus::database_unavailable);
        for(unsigned i=0;i<3;++i)if(decoded.slots[i].sticker!=current.slots[i].sticker)return static_cast<int>(WireStatus::database_unavailable);
        wire::Put(output,static_cast<std::uint64_t>(result),4);std::memcpy(output+4,encoded.data(),encoded.size());*produced=static_cast<std::uint32_t>(encoded.size()+4);
        return static_cast<int>(WireStatus::success);
    }catch(...){return static_cast<int>(WireStatus::database_unavailable);}
}
}
sdgo::friend_unit_gift::ExtensionHandlerFn PrepareDecals(const SdgoApi* api, SdgoMod* mod) {
    if(!api||!api->SettingInt||!api->ModDirectory||!api->host_key||std::strcmp(api->host_key,"WorldServer-548A86E3-0010C000")||!api->SettingInt(mod,"DecalsEnabled",1))return 0;
    std::ifstream file(std::wstring(api->ModDirectory(mod))+L"\\decal_schema.sql",std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)),{});
    if(bytes.empty()||bytes.size()>65536)return 0;
    for(unsigned char c:bytes){if(c>127)return 0;schema+=static_cast<wchar_t>(c);}
    return &Handle;
}
