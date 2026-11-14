#include "sdgo/sdgo_api.h"
#include "sdgo/friend_unit_gift_protocol.h"
#include "sdgo/rgb_paint_sql.h"
#include "../../oc_custom_server/oc_session.h"
#include <fstream>
#include <mutex>
namespace {
using namespace sdgo::rgb_paint;
using namespace sdgo::oc_custom::server;
using sdgo::friend_unit_gift::WireStatus;
std::wstring schema;
std::mutex schemaLock;
bool ready=false;
bool Initialize(Session& db){std::lock_guard<std::mutex> hold(schemaLock);if(!ready)ready=db.Execute(schema);return ready;}
std::int32_t __cdecl Handle(void*,std::uint32_t actor,std::uint16_t command,const std::uint8_t* data,std::uint32_t size,void*,void* database,std::uint8_t* output,std::uint32_t capacity,std::uint32_t* produced){
    if(produced)*produced=0;
    if(!actor||!data||!output||!produced||capacity<wire::bytes+4)return static_cast<int>(WireStatus::invalid_argument);
    try {
        Record proposal;const bool saving=command==wire::save;
        if((saving&&!wire::Decode(data,size,proposal))||(!saving&&(command!=wire::get||size!=8)))return static_cast<int>(WireStatus::invalid_argument);
        const auto uid=saving?proposal.state.identity.unit:wire::Get(data,8);const auto batch=sql::Request(actor,uid,saving?&proposal:nullptr);
        if(batch.empty())return static_cast<int>(WireStatus::invalid_argument);
        Session db(5);if(!db.Open(database)||!Initialize(db))return static_cast<int>(WireStatus::database_unavailable);
        ResultSets sets;if(!db.Query(batch,&sets)||sets.size()!=1||sets[0].size()!=1)return static_cast<int>(WireStatus::database_unavailable);
        const auto& row=sets[0][0];const auto result=Int64Or(row,0,-1);
        if(row.size()==1&&(result==6||result==9))return static_cast<int>(result);
        if(row.size()!=5||(result!=0&&result!=12))return static_cast<int>(WireStatus::database_unavailable);
        Record current;const auto type=Int64Or(row,1,-1),revision=Int64Or(row,2,-1);
        if(type<=0||type>INT32_MAX||revision<=0)return static_cast<int>(WireStatus::database_unavailable);
        current.type=static_cast<std::uint32_t>(type);current.state.identity.unit=uid;current.state.revision=static_cast<std::uint64_t>(revision);
        std::uint8_t native[12]{};if(!sql::Unhex(row[3],native,12))return static_cast<int>(WireStatus::database_unavailable);
        for(unsigned i=0;i<6;++i){current.native[i]=static_cast<std::uint16_t>(wire::Get(native+i*2,2));current.state.colors[i]=FromNative(current.native[i]);}
        wire::Bytes encoded;if(!wire::Encode(current,encoded))return static_cast<int>(WireStatus::database_unavailable);
        if(!row[4].empty()&&!sql::Unhex(row[4],encoded.data()+36,24))return static_cast<int>(WireStatus::database_unavailable);
        Record verified;if(!wire::Decode(encoded.data(),encoded.size(),verified))return static_cast<int>(WireStatus::database_unavailable);
        wire::Put(output,static_cast<std::uint64_t>(result),4);std::memcpy(output+4,encoded.data(),encoded.size());*produced=static_cast<unsigned>(encoded.size()+4);
        return static_cast<int>(WireStatus::success);
    }catch(...){return static_cast<int>(WireStatus::database_unavailable);}
}
}
sdgo::friend_unit_gift::ExtensionHandlerFn PrepareRgb(const SdgoApi* api, SdgoMod* mod) {
    if(!api||!api->SettingInt||!api->ModDirectory||!api->host_key||std::strcmp(api->host_key,"WorldServer-548A86E3-0010C000")||!api->SettingInt(mod,"RgbEnabled",1))return 0;
    std::ifstream file(std::wstring(api->ModDirectory(mod))+L"\\rgb_schema.sql",std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(file)),{});
    if(bytes.empty()||bytes.size()>65536)return 0;for(unsigned char c:bytes){if(c>127)return 0;schema+=static_cast<wchar_t>(c);}
    return &Handle;
}
