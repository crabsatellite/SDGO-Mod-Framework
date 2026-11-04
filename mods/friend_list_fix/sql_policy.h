#pragma once
#include <algorithm>
#include <cwctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace sdgo::friend_fix {
inline constexpr const wchar_t* kSchema = L"sdgo_friend_list_fix";
inline constexpr const wchar_t* kNames[] = {L"sp_FriendsList"};
inline std::wstring Identifier(const std::wstring& value) {
    std::wstring out=L"["; for(wchar_t c:value){out+=c;if(c==L']')out+=c;} return out+L"]";
}
inline std::wstring Literal(const std::wstring& value) {
    std::wstring out=L"N'";for(wchar_t c:value){if(!c)throw std::runtime_error("embedded SQL null");out+=c;if(c==L'\'')out+=c;}return out+L"'";
}
inline std::wstring Lower(std::wstring value) {
    for(auto& c:value)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';return value;
}
inline std::wstring Mask(const std::wstring& text) {
    std::wstring out=text;size_t i=0;
    while(i<text.size()) {
        size_t start=i;
        if(text.compare(i,2,L"--")==0){auto n=text.find(L'\n',i);i=n==std::wstring::npos?text.size():n;}
        else if(text.compare(i,2,L"/*")==0){
            int depth=1;i+=2;
            while(i<text.size()&&depth){if(text.compare(i,2,L"/*")==0){++depth;i+=2;}else if(text.compare(i,2,L"*/")==0){--depth;i+=2;}else ++i;}
            if(depth)throw std::runtime_error("unclosed SQL comment");
        }else if(text[i]==L'\''||text[i]==L'"'||text[i]==L'['){
            const wchar_t end=text[i]==L'['?L']':text[i];bool closed=false;++i;
            while(i<text.size()){if(text[i++]==end){if(i<text.size()&&text[i]==end)++i;else{closed=true;break;}}}
            if(!closed)throw std::runtime_error("unclosed SQL quote");
        }else{++i;continue;}
        for(size_t j=start;j<i;++j)if(out[j]!=L'\r'&&out[j]!=L'\n')out[j]=L' ';
    }
    return out;
}
struct Header { size_t action, actionLength, name, nameLength; };
inline Header ParseHeader(const std::wstring& sql,const std::wstring& name,const std::wstring& schema) {
    const auto code=Mask(sql);
    static const std::wregex h(LR"(\b(CREATE|ALTER)\s+(?:PROC|PROCEDURE)\b)",std::regex::icase);
    static const std::wregex n(LR"(\s*(?:(\[[A-Za-z_][A-Za-z_0-9]*\]|[A-Za-z_][A-Za-z_0-9]*)\s*\.\s*)?(\[[A-Za-z_][A-Za-z_0-9]*\]|[A-Za-z_][A-Za-z_0-9]*))");
    std::wsmatch match,object;
    if(!std::regex_search(code,match,h)||code.substr(0,match.position()).find_first_not_of(L" \t\r\n")!=std::wstring::npos)
        throw std::runtime_error("unknown procedure header");
    size_t after=static_cast<size_t>(match.position()+match.length());
    auto tail=sql.substr(after);
    if(!std::regex_search(tail,object,n,std::regex_constants::match_continuous))throw std::runtime_error("unknown procedure name");
    auto unquote=[](std::wstring s){if(!s.empty()&&s.front()==L'[')return s.substr(1,s.size()-2);return s;};
    const auto owner=object[1].matched?unquote(object[1].str()):L"dbo";
    if(Lower(unquote(object[2].str()))!=Lower(name)||Lower(owner)!=Lower(schema))throw std::runtime_error("procedure identity mismatch");
    size_t begin=after;while(begin<sql.size()&&iswspace(sql[begin]))++begin;
    return {static_cast<size_t>(match.position(1)),static_cast<size_t>(match.length(1)),begin,
            after+static_cast<size_t>(object.position()+object.length())-begin};
}
inline std::wstring Canonical(const std::wstring& sql,const std::wstring& name,const std::wstring& schema) {
    auto h=ParseHeader(sql,name,schema);auto out=sql;out.replace(h.action,h.actionLength,L"CREATE");return out;
}
struct SqlLiteral { size_t begin, length; std::wstring value; };
inline std::vector<SqlLiteral> Literals(const std::wstring& text) {
    std::vector<SqlLiteral> result;
    for(size_t i=0;i<text.size();) {
        if(text.compare(i,2,L"--")==0) {
            const auto end=text.find(L'\n',i); i=end==std::wstring::npos?text.size():end;
        } else if(text.compare(i,2,L"/*")==0) {
            int depth=1;i+=2;
            while(i<text.size()&&depth) {
                if(text.compare(i,2,L"/*")==0){++depth;i+=2;}
                else if(text.compare(i,2,L"*/")==0){--depth;i+=2;}
                else ++i;
            }
            if(depth)throw std::runtime_error("unclosed SQL comment");
        } else if(text[i]==L'\'') {
            const auto start=i++; std::wstring decoded; bool closed=false;
            while(i<text.size()) {
                if(text[i]==L'\'') {
                    if(i+1<text.size()&&text[i+1]==L'\''){decoded+=L'\'';i+=2;}
                    else {++i;closed=true;break;}
                } else decoded+=text[i++];
            }
            if(!closed)throw std::runtime_error("unclosed SQL literal");
            result.push_back({start,i-start,std::move(decoded)});
        } else if(text[i]==L'[' || text[i]==L'"') {
            const wchar_t end=text[i++]==L'['?L']':L'"'; bool closed=false;
            while(i<text.size()) {
                if(text[i++]==end) {
                    if(i<text.size()&&text[i]==end)++i;
                    else {closed=true;break;}
                }
            }
            if(!closed)throw std::runtime_error("unclosed SQL identifier");
        } else ++i;
    }
    return result;
}
inline unsigned Replace(std::wstring& text,const std::wregex& pattern,const std::wstring& replacement) {
    std::vector<std::pair<std::size_t,std::size_t>> matches;
    for(auto i=std::wsregex_iterator(text.begin(),text.end(),pattern);i!=std::wsregex_iterator();++i)
        matches.emplace_back(static_cast<std::size_t>(i->position()),static_cast<std::size_t>(i->length()));
    for(auto i=matches.rbegin();i!=matches.rend();++i)text.replace(i->first,i->second,replacement);
    return static_cast<unsigned>(matches.size());
}
inline std::wstring Compact(std::wstring text) {
    text=Lower(text);text.erase(std::remove_if(text.begin(),text.end(),[](wchar_t c){return iswspace(c)!=0;}),text.end());return text;
}
inline std::wstring Projection(const std::wstring& alias) {
    const wchar_t* fields[]={L"f_idx2",L"f_pilot",L"f_relation",L"f_gender",L"f_cutoff",L"f_level",L"f_levelname",
        L"f_mark1",L"f_mark2",L"f_channel",L"f_loc",L"f_room",L"f_team",L"f_agent",L"f_regdate",L"f_emblem",L"f_levelstep"};
    std::wstring out;
    for(unsigned i=0;i<17;++i) {
        if(i)out+=L", ";
        std::wstring value=(i==15&&alias==L"tbl1"?L"tbl2":alias)+L"."+fields[i];
        if(i<3){out+=value;continue;}
        const wchar_t* fallback=i==3||i==4?L"'0'":i==6?L"''":i==14?L"CONVERT(datetime,0)":i==16?L"1":L"0";
        if(alias==L"BB"&&(i==7||i==8)) {
            const auto base=i==7?L"260000":L"250000";
            out+=L"ISNULL("+value+L","+base+L")-"+base;
        } else out+=L"ISNULL("+value+L","+fallback+L")";
        out+=L" AS "+std::wstring(fields[i]);
    }
    return out;
}
inline std::wstring OriginalProjection(const std::wstring& alias) {
    const wchar_t* fields[]={L"f_idx2",L"f_pilot",L"f_relation",L"f_gender",L"f_cutoff",L"f_level",L"f_levelname",
        L"f_mark1",L"f_mark2",L"f_channel",L"f_loc",L"f_room",L"f_team",L"f_agent",L"f_regdate",L"f_emblem",L"f_levelstep"};
    std::wstring out;
    for(unsigned i=0;i<17;++i) {
        if(i)out+=L", ";
        out+=(i==15&&alias==L"tbl1"?L"tbl2":alias)+L"."+fields[i];
        if(alias==L"BB"&&(i==7||i==8))out+=(i==7?L"-260000 as ":L"-250000 as ")+std::wstring(fields[i]);
    }
    return out;
}
inline std::wstring Clone(const std::wstring& source,const std::wstring& name,bool exists,
                          const std::wstring& nameCollation=L"DATABASE_DEFAULT",
                          const std::wstring& gameDatabase=L"") {
    if(name!=kNames[0])throw std::runtime_error("unexpected friend procedure");
    static const std::wregex collationName(L"[A-Za-z][A-Za-z0-9_]{0,127}");
    if(!std::regex_match(nameCollation,collationName))throw std::runtime_error("invalid native pilot-name collation");
    // Decode only actual SQL literals. Comments and SQL outside the dynamic
    // query remain unchanged, including pagination and output parameters.
    auto literals=Literals(source);
    const auto flags=std::regex::icase;
    const std::wregex pilot(LR"(\ba\.f_pilot\b)",flags);
    const std::wregex join(LR"(\btbl_Friend\s+a\s+LEFT\s+JOIN\s+tblg_userGameInfo\s+b\s+ON\s+a\.f_idx2\s*=\s*b\.f_idx\b)",flags);
    const std::wregex projection(LR"(\b(tbl1|BB)\.f_idx2\s*,([\s\S]*?)\bFROM\b)",flags);
    unsigned names=0,joins=0,first=0,later=0,levels=0,steps=0;
    for(auto& literal:literals) {
        auto& query=literal.value;
        // Member databases can use Thai while their Unicode pilot-name column
        // uses Chinese. Resolve both CASE operands explicitly before converting
        // to native ANSI; DATABASE_DEFAULT would lose canonical Chinese names.
        names+=Replace(query,pilot,L"CONVERT(varchar(20),COALESCE(NULLIF(p.f_pilot COLLATE "+nameCollation+
            L",N''),CONVERT(nvarchar(20),a.f_pilot) COLLATE "+nameCollation+L")) AS f_pilot");
        const auto gameJoin=gameDatabase.empty()?L"":L" LEFT JOIN "+Identifier(gameDatabase)+
            L".dbo.tbl_Users rank_user ON a.f_idx2=rank_user.AccountNumber";
        joins+=Replace(query,join,L"tbl_Friend a LEFT JOIN tbl_Account_Pilot p ON a.f_idx2=p.f_idx LEFT JOIN tblg_userGameInfo b ON a.f_idx2=b.f_idx"+gameJoin);
        if (!gameDatabase.empty()) {
            levels+=Replace(query,std::wregex(LR"(\bb\.f_level\b)",flags),
                L"COALESCE(b.f_level,CONVERT(int,rank_user.UserLevelNumber)) AS f_level");
            steps+=Replace(query,std::wregex(LR"(ISNULL\s*\(\s*b\.f_levelstep\s*,\s*1\s*\))",flags),
                L"COALESCE(b.f_levelstep,CONVERT(int,rank_user.UserLevelStep),1)");
        }
        std::vector<std::wsmatch> projections;
        for(auto i=std::wsregex_iterator(query.begin(),query.end(),projection);i!=std::wsregex_iterator();++i)projections.push_back(*i);
        for(auto item=projections.rbegin();item!=projections.rend();++item) {
            const auto& match=*item;
            const auto alias=Lower(match[1].str())==L"tbl1"?L"tbl1":L"BB";
            const auto projectionText=match.str().substr(0,match.length()-4);
            if(Compact(projectionText)!=Compact(OriginalProjection(alias)))
                throw std::runtime_error("unknown friend result projection");
            if(alias==std::wstring(L"tbl1"))++first;else ++later;
            query.replace(match.position(),match.length(),Projection(alias)+L" FROM");
        }
    }
    if(names!=2||joins!=2||first!=1||later!=1)throw std::runtime_error("unknown native friend query shape");
    if(!gameDatabase.empty() && (levels!=2||steps!=2))throw std::runtime_error("unknown native friend rank projection");
    auto result=source;
    for(auto i=literals.rbegin();i!=literals.rend();++i) {
        auto quoted=Literal(i->value);quoted.erase(quoted.begin()); // preserve the source's N prefix
        result.replace(i->begin,i->length,quoted);
    }
    if (!gameDatabase.empty() && Replace(result,
            std::wregex(LR"(\bDECLARE\s+@SQL\s+NVARCHAR\s*\(\s*4000\s*\))",flags),
            L"DECLARE @SQL NVARCHAR(MAX)")!=1)
        throw std::runtime_error("unknown native friend dynamic query declaration");
    auto h=ParseHeader(result,name,L"dbo");
    result.replace(h.name,h.nameLength,Identifier(kSchema)+L"."+Identifier(name));
    result.replace(h.action,h.actionLength,exists?L"ALTER":L"CREATE");
    return result;
}
}
