#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace sdgo::announcements::json {
struct Value {
    enum Kind { Null, Boolean, Number, String, Array, Object } kind=Null;
    bool boolean=false;
    double number=0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string,Value> object;
    const Value& operator[](const std::string& name) const {
        static const Value empty;auto p=object.find(name);return p==object.end()?empty:p->second;
    }
    int64_t Integer(int64_t fallback=0) const {
        return kind==Number && std::isfinite(number) && std::floor(number)==number &&
            number>-9223372036854775808.0 && number<9223372036854775807.0?static_cast<int64_t>(number):fallback;
    }
};
inline void Utf8(std::string& out,uint32_t cp) {
    if(cp<=0x7f)out.push_back(static_cast<char>(cp));
    else if(cp<=0x7ff){out.push_back(static_cast<char>(0xc0|(cp>>6)));out.push_back(static_cast<char>(0x80|(cp&63)));}
    else if(cp<=0xffff){out.push_back(static_cast<char>(0xe0|(cp>>12)));out.push_back(static_cast<char>(0x80|((cp>>6)&63)));out.push_back(static_cast<char>(0x80|(cp&63)));}
    else {out.push_back(static_cast<char>(0xf0|(cp>>18)));out.push_back(static_cast<char>(0x80|((cp>>12)&63)));out.push_back(static_cast<char>(0x80|((cp>>6)&63)));out.push_back(static_cast<char>(0x80|(cp&63)));}
}
class Parser {
    const std::string& input;size_t at=0,nodes=0;
    [[noreturn]] void Fail() const {throw std::runtime_error("invalid bounded news JSON");}
    void Space(){while(at<input.size() && (input[at]==' '||input[at]=='\t'||input[at]=='\n'||input[at]=='\r'))++at;}
    char Take(){if(at>=input.size())Fail();return input[at++];}
    uint32_t Hex(){uint32_t result=0;for(int i=0;i<4;++i){char c=Take();int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(n<0)Fail();result=result*16+static_cast<uint32_t>(n);}return result;}
    std::string Text(){
        if(Take()!='"')Fail();std::string out;
        for(;;){const unsigned char c=static_cast<unsigned char>(Take());if(c=='"')return out;if(c<32)Fail();
            if(c!='\\'){out.push_back(static_cast<char>(c));continue;}
            switch(Take()){
            case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{uint32_t cp=Hex();if(cp>=0xd800 && cp<=0xdbff){if(Take()!='\\'||Take()!='u')Fail();uint32_t low=Hex();if(low<0xdc00||low>0xdfff)Fail();cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}else if(cp>=0xdc00&&cp<=0xdfff)Fail();Utf8(out,cp);break;}
            default:Fail();}
        }
    }
    Value Read(int depth){
        if(depth>24 || ++nodes>40000)Fail();Space();if(at>=input.size())Fail();Value v;const char c=input[at];
        if(c=='"'){v.kind=Value::String;v.string=Text();return v;}
        if(c=='{'||c=='['){++at;v.kind=c=='{'?Value::Object:Value::Array;Space();if(at<input.size()&&input[at]==(c=='{'?'}':']')){++at;return v;}
            for(;;){Space();if(c=='{'){const auto key=Text();Space();if(Take()!=':')Fail();if(!v.object.emplace(key,Read(depth+1)).second)Fail();}else v.array.push_back(Read(depth+1));Space();char end=Take();if(end==(c=='{'?'}':']'))return v;if(end!=',')Fail();}
        }
        for(const auto& literal:std::vector<std::pair<std::string,int>>{{"null",0},{"true",1},{"false",2}})
            if(input.compare(at,literal.first.size(),literal.first)==0){at+=literal.first.size();v.kind=literal.second?Value::Boolean:Value::Null;v.boolean=literal.second==1;return v;}
        const size_t begin=at;if(input[at]=='-')++at;if(at>=input.size())Fail();
        if(input[at]=='0')++at;else {if(input[at]<'1'||input[at]>'9')Fail();while(at<input.size()&&input[at]>='0'&&input[at]<='9')++at;}
        if(at<input.size()&&input[at]=='.'){++at;const size_t first=at;while(at<input.size()&&input[at]>='0'&&input[at]<='9')++at;if(first==at)Fail();}
        if(at<input.size()&&(input[at]=='e'||input[at]=='E')){++at;if(at<input.size()&&(input[at]=='+'||input[at]=='-'))++at;const size_t first=at;while(at<input.size()&&input[at]>='0'&&input[at]<='9')++at;if(first==at)Fail();}
        const auto raw=input.substr(begin,at-begin);char* end=nullptr;v.number=std::strtod(raw.c_str(),&end);if(!end||*end||!std::isfinite(v.number))Fail();v.kind=Value::Number;return v;
    }
public:
    explicit Parser(const std::string& text):input(text){if(text.size()>524288)Fail();}
    Value Parse(){auto v=Read(0);Space();if(at!=input.size())Fail();return v;}
};
inline std::string Quote(const std::string& text){
    const char* digits="0123456789abcdef";std::string out="\"";
    for(unsigned char c:text){if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}else if(c<32){out+="\\u00";out+=digits[c>>4];out+=digits[c&15];}else out+=static_cast<char>(c);}
    return out+'"';
}
}
