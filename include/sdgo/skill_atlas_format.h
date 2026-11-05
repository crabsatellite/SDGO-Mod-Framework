#pragma once
#include "unit_catalog_format.h"
#include "script_registration.h"
#include <map>
#include <set>
#include <stdexcept>

namespace sdgo::skill_atlas {
struct Entry {
    std::uint32_t id=0,frameCount=0;
    std::string owner,revision;
    std::wstring sprite;
    std::vector<std::uint8_t> record;
};

class Bundle {
public:
    std::map<std::uint32_t,Entry> entries;
    bool Parse(const std::vector<std::uint8_t>& bytes,const std::string& catalogHash,
        const std::string& resourceIdentity,const std::set<std::wstring>& publishedPaths,
        const unit_catalog::Catalog& catalog,std::string& error){
        entries.clear();error.clear();
        try{
            Reader reader{bytes};
            reader.require(bytes.size()>=128&&bytes.size()<=128+8u*1024u*1024u,"skill atlas sidecar byte extent");
            reader.require(!std::memcmp(bytes.data(),"SDGOAT1\0",8),"skill atlas sidecar magic");reader.position=8;
            reader.require(reader.word()==1&&reader.word()==128,"skill atlas sidecar header/version");
            const auto count=reader.word(),size=reader.word();
            reader.require(count&&count<=4096&&size==bytes.size()-128,"skill atlas sidecar record/payload extent");
            reader.require(reader.hash()==catalogHash,"skill atlas catalog SHA-256 differs");
            const auto index=reader.hash(),archive=reader.hash();
            reader.require(index+":"+archive==resourceIdentity,"skill atlas sealed resource identity differs");
            reader.require(reader.word()==0&&reader.word()==0,"skill atlas sidecar reserved fields");
            std::map<std::uint32_t,Entry> prepared;
            for(std::uint32_t i=0;i<count;++i){
                Entry entry;entry.id=reader.word();const auto recordHash=reader.hash();
                entry.owner=reader.text(64);entry.revision=reader.text(64);const auto path=reader.text(80);entry.frameCount=reader.word();
                for(std::size_t j=0;j<entry.owner.size();++j){const char c=entry.owner[j];
                    reader.require((c>='a'&&c<='z')||(c>='0'&&c<='9')||(j&&(c=='_'||c=='.'||c=='-')),"skill atlas owner namespace");
                }
                reader.require(entry.id&&entry.id<=9999999&&entry.frameCount&&entry.frameCount<=1000,"skill atlas identity/frame count");
                reader.require(entry.revision.size()==64,"skill atlas revision length");
                for(const auto c:entry.revision)reader.require((c>='0'&&c<='9')||(c>='a'&&c<='f'),"skill atlas revision SHA-256");
                reader.require(path.size()>9&&path.compare(0,5,"sprs/")==0&&path.substr(path.size()-4)==".spr"&&
                    path!="sprs/0594.spr"&&path.find("..") == std::string::npos,"skill atlas independent sprite path required");
                for(std::size_t j=0;j<path.size();++j){const char c=path[j];
                    reader.require((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||(j==4&&c=='/'),"skill atlas canonical sprite path");
                }
                entry.sprite.assign(path.begin(),path.end());for(auto& c:entry.sprite)if(c==L'/')c=L'\\';
                reader.require(publishedPaths.count(entry.sprite)!=0,"skill atlas sprite is absent from the sealed unit archive");
                const auto slot=catalog.Find(entry.id);
                reader.require(slot<catalog.records.size(),"skill atlas skill is absent from the catalog");
                const auto& record=catalog.records[slot];
                reader.require(record.kind==13&&record.bytes.size()==1132&&scripts::Sha256(record.bytes)==recordHash,"skill atlas complete skill record differs");
                std::uint16_t frame=0;std::memcpy(&frame,record.bytes.data()+426,2);
                reader.require(frame<entry.frameCount,"skill atlas source frame is outside the owner sprite");
                entry.record=record.bytes;
                reader.require(prepared.emplace(entry.id,std::move(entry)).second,"skill atlas skill repeats");
            }
            reader.require(reader.position==bytes.size(),"skill atlas sidecar trailing bytes");
            entries.swap(prepared);return true;
        }catch(const std::exception& exception){error=exception.what();return false;}
    }
private:
    struct Reader{
        const std::vector<std::uint8_t>& bytes;std::size_t position=0;
        void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
        std::uint32_t word(){require(bytes.size()-position>=4,"truncated skill atlas integer");const auto value=unit_catalog::U32(bytes.data()+position);position+=4;return value;}
        std::string hash(){require(bytes.size()-position>=32,"truncated skill atlas hash");const char hex[]="0123456789abcdef";std::string result;
            for(unsigned i=0;i<32;++i){const auto value=bytes[position++];result+=hex[value>>4];result+=hex[value&15];}return result;}
        std::string text(std::uint32_t maximum){const auto length=word();require(length&&length<=maximum&&length<=bytes.size()-position,"skill atlas text extent");
            std::string result(reinterpret_cast<const char*>(bytes.data()+position),length);position+=length;
            for(const unsigned char c:result)require(c>=32&&c<=126,"skill atlas text encoding");return result;}
    };
};
}
