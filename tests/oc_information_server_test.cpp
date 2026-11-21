#include "ai_test_support.h"
#include "../mods/oc_custom_server/oc_custom_server.cpp"

namespace {
struct Database {
    ResultSets result;
    std::wstring query;
    bool available = true;
    unsigned calls = 0;
    bool Query(const std::wstring& sql, ResultSets* output) {
        ++calls;
        query = sql;
        *output = result;
        return available;
    }
};
}

int main() {
    SCENARIO("information returns the inspected build without an owner identity or mutation");
    Database database;
    TextRow row(61, L"0");
    row[0]=L"42"; row[1]=L"999"; row[2]=L"100"; row[3]=L"15";
    row[4]=L"4"; row[5]=L"7"; row[9]=L"2"; row[16]=L"3";
    row[27]=L"50"; row[28]=L"14000"; row[32]=L"5500";
    row[45]=L"4"; row[52]=L"4";
    for (unsigned i=0;i<4;++i) {row[53+i]=std::to_wstring(30101+i);row[57+i]=std::to_wstring(30001+i);}
    database.result={{row}};
    g_settings.g1=true; g_g1_state=1;
    UnitRequest request;request.unit_uid=42;
    std::array<std::uint8_t,kUnitRequestBytes> payload{};
    WriteUnitRequest(payload.data(),request);
    std::array<std::uint8_t,kInformationReplyBytes> reply{};
    std::uint32_t produced=0;
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==0);
    CHECK(database.calls==1 && produced==reply.size());
    Information value;std::int32_t result=-1;
    CHECK(ReadInformationReply(reply.data(),produced,&result,&value) && result==0);
    CHECK(value.profile.unit_uid==42 && value.profile.unit_number==100 && value.profile.revision==7);
    CHECK(value.profile.base[5]==5500 && value.profile.normal[0]==2 && value.profile.over[0]==3);
    CHECK(value.g2_slots==4 && value.g2_skills[3]==30104 && value.g1_skills[3]==30004);
    for(const auto* verb:{L"EXEC ",L"INSERT ",L"UPDATE ",L"DELETE ",L"MERGE ",L"EnsureProfile"})
        CHECK(database.query.find(verb)==std::wstring::npos);
    CHECK(database.query.find(L"P.UserUnitNumber=42")!=std::wstring::npos);
    CHECK(database.query.find(L"G.AccountNumber=UU.AccountNumber")!=std::wstring::npos);
    CHECK(database.query.find(L"G.UnitNumber=UU.UnitNumber")!=std::wstring::npos);

    SCENARIO("missing records and unavailable databases cannot masquerade as empty successful builds");
    database.result={{}};
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==0);
    CHECK(ReadInformationReply(reply.data(),produced,&result,&value) && result==1 && value.profile.unit_uid==0);
    database.available=false;
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==static_cast<int>(WireStatus::database_unavailable));
    database.available=true;database.result={{TextRow(60,L"0")}};
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==static_cast<int>(WireStatus::database_unavailable));
    row[52]=L"5";database.result={{row}};
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==static_cast<int>(WireStatus::database_unavailable));
    const auto calls=database.calls;
    CHECK(HandleInformation(database,payload.data(),payload.size()-1,reply.data(),&produced)==static_cast<int>(WireStatus::invalid_argument));
    request.unit_uid=0;WriteUnitRequest(payload.data(),request);
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==static_cast<int>(WireStatus::invalid_argument));
    request.unit_uid=UINT64_MAX;WriteUnitRequest(payload.data(),request);
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==static_cast<int>(WireStatus::invalid_argument));
    CHECK(database.calls==calls);

    SCENARIO("G1-disabled servers do not query an optional table");
    request.unit_uid=42;WriteUnitRequest(payload.data(),request);
    g_settings.g1=false;row[52]=L"2";
    for(unsigned i=0;i<4;++i)row[57+i]=L"0";
    row[3]=L"0"; // no OC allocation: native base and G2 still have an information view
    database.result={{row}};
    CHECK(HandleInformation(database,payload.data(),payload.size(),reply.data(),&produced)==0);
    CHECK(database.query.find(L"sdgo_mod_g1_UserUnitSkill")==std::wstring::npos);
    CHECK(ReadInformationReply(reply.data(),produced,&result,&value) && result==0);
    CHECK(value.profile.oc_level==0 && value.profile.base[5]==5500 && value.g2_slots==2);
    CHECK((value.g1_skills==std::array<std::uint16_t,4>{}));
    return ai_test::Finish();
}
