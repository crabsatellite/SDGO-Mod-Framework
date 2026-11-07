#pragma once
#include "skill_identity.h"
#include "script_registration.h"
#include <cstring>
#include <set>
#include <stdexcept>

namespace sdgo::skill_identity {
namespace format {
constexpr std::size_t kMaxBytes=8u*1024u*1024u;
class Reader {
public:
    const std::vector<std::uint8_t>& bytes;std::size_t position=0;
    explicit Reader(const std::vector<std::uint8_t>& input):bytes(input){}
    void require(bool valid,const char* error){if(!valid)throw std::runtime_error(error);}
    std::uint32_t word(){require(position<=bytes.size()&&bytes.size()-position>=4,"truncated skill identity integer");
        std::uint32_t value=0;std::memcpy(&value,bytes.data()+position,4);position+=4;return value;}
    std::string hash(){require(position<=bytes.size()&&bytes.size()-position>=32,"truncated skill identity SHA-256");
        const char hex[]="0123456789abcdef";std::string result;
        for(unsigned i=0;i<32;++i){const auto value=bytes[position++];result+=hex[value>>4];result+=hex[value&15];}return result;}
    std::string text(std::size_t maximum){const auto size=word();require(size&&size<=maximum&&size<=bytes.size()-position,"skill identity text budget");
        std::string result(reinterpret_cast<const char*>(bytes.data()+position),size);position+=size;
        for(const unsigned char c:result)require(c>=32&&c<=126,"skill identity text encoding");return result;}
    void magic(const char* value){require(bytes.size()>=8&&!std::memcmp(bytes.data(),value,8),"skill identity runtime magic");position=8;}
    std::vector<Candidate> owners(std::uint32_t count){
        require(count&&count<=128,"skill identity runtime owner count");
        std::vector<Candidate> result;std::set<std::string> names;std::set<std::uint32_t> targets;
        for(std::uint32_t i=0;i<count;++i){
            Candidate owner;owner.owner=text(64);owner.revision=text(64);
            require(names.insert(owner.owner).second,"skill identity runtime repeated owner");
            const auto size=word();require(size&&size<=4096,"skill identity runtime binding count");
            for(std::uint32_t j=0;j<size;++j){
                Skill skill;skill.target=word();skill.source=word();
                require(skill.target&&skill.target<=9999999&&skill.source&&skill.source<=9999999&&
                    targets.size()<4096&&targets.insert(skill.target).second,"skill identity runtime repeated/invalid destination");
                skill.clientSha256=hash();skill.serverSha256=hash();skill.templateSha256=hash();owner.skills.push_back(std::move(skill));
            }
            result.push_back(std::move(owner));
        }
        return result;
    }
};
}

class RuntimeAuthority final:public Authority {
public:
    std::string revision(const std::string& owner)const override{
        const auto found=owners_.find(owner);return found==owners_.end()?"":found->second.revision;
    }
    bool originalSkill(std::uint32_t id)const override{return originals_.count(id)!=0;}
    bool templateSkill(std::uint32_t id,const std::string& hash)const override{
        const auto found=templates_.find(id);return found!=templates_.end()&&found->second==hash;
    }
    bool allocatedSkill(const std::string& owner,std::uint32_t id,const std::string& client,const std::string& server)const override{
        const auto found=allocations_.find(id);
        return found!=allocations_.end()&&found->second.first==owner&&
            found->second.second.clientSha256==client&&found->second.second.serverSha256==server;
    }
private:
    friend class RuntimeInput;
    std::set<std::uint32_t> originals_;
    std::map<std::string,Candidate> owners_;
    std::map<std::uint32_t,std::string> templates_;
    std::map<std::uint32_t,std::pair<std::string,Skill>> allocations_;
};

class RuntimeInput {
public:
    std::vector<Candidate> candidates;
    RuntimeAuthority authority;
    std::string serverFingerprint,candidateSha256;
    bool Parse(const std::vector<std::uint8_t>& candidate,const std::vector<std::uint8_t>& trusted,std::string& error){
        candidates.clear();authority=RuntimeAuthority{};serverFingerprint.clear();candidateSha256.clear();error.clear();
        try{
            format::Reader content(candidate),installation(trusted);
            content.require(candidate.size()>=64&&candidate.size()<=64+format::kMaxBytes,"skill identity candidate byte budget");
            content.magic("SDGOSI1\0");content.require(content.word()==1&&content.word()==64,"skill identity candidate header/version");
            const auto count=content.word(),size=content.word();
            content.require(size==candidate.size()-64,"skill identity candidate payload extent");
            const auto digest=content.hash();content.require(content.word()==0&&content.word()==0,"skill identity candidate reserved bytes");
            content.require(scripts::Sha256(std::vector<std::uint8_t>(candidate.begin()+64,candidate.end()))==digest,"skill identity payload SHA-256 differs");
            auto requested=content.owners(count);content.require(content.position==candidate.size(),"skill identity candidate trailing bytes");
            installation.require(trusted.size()>=128&&trusted.size()<=128+format::kMaxBytes+400000,"skill identity authority byte budget");
            installation.magic("SDGOSA1\0");installation.require(installation.word()==1&&installation.word()==128,"skill identity authority header/version");
            const auto originals=installation.word(),owners=installation.word(),extent=installation.word();
            installation.require(originals&&originals<=100000&&owners==count&&extent==trusted.size()-128,"skill identity authority extent/counts");
            const auto candidateHash=installation.hash();const auto fingerprint=installation.hash();
            installation.require(installation.hash()=="f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4"&&installation.word()==0,
                "skill identity authority host/reserved bytes");
            installation.require(candidateHash==scripts::Sha256(candidate),"candidate differs from the independently installed authority");
            RuntimeAuthority resolved;std::uint32_t previous=0;
            for(std::uint32_t i=0;i<originals;++i){const auto id=installation.word();
                installation.require(id>previous&&id<=9999999,"skill identity original ID order/range");resolved.originals_.insert(id);previous=id;}
            auto allowed=installation.owners(owners);
            installation.require(installation.position==trusted.size(),"skill identity authority trailing bytes");
            // The trusted controller authorizes the complete candidate payload,
            // including the selected source, not just a matching destination ID.
            installation.require(trusted.size()-128-originals*4==candidate.size()-64&&
                !std::memcmp(trusted.data()+128+originals*4,candidate.data()+64,candidate.size()-64),
                "skill identity installed allocation payload differs");
            for(const auto& owner:allowed){
                resolved.owners_.emplace(owner.owner,owner);
                for(const auto& skill:owner.skills){
                    const auto found=resolved.templates_.find(skill.source);
                    installation.require(found==resolved.templates_.end()||found->second==skill.templateSha256,"skill identity template hashes disagree");
                    resolved.templates_[skill.source]=skill.templateSha256;
                    resolved.allocations_.emplace(skill.target,std::make_pair(owner.owner,skill));
                }
            }
            std::string policyError;
            installation.require(bool(Bindings::Build(requested,resolved,policyError)),policyError.empty()?"skill identity policy rejected":policyError.c_str());
            candidates=std::move(requested);authority=std::move(resolved);serverFingerprint=fingerprint;candidateSha256=candidateHash;return true;
        }catch(const std::exception& exception){error=exception.what();return false;}
    }
};
}
