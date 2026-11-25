#include "ai_test_support.h"
#include "../mods/oc_custom_server/oc_custom_server.cpp"

namespace {
struct Database {
    ResultSets result;
    std::wstring query;
    bool available = true;
    unsigned calls = 0;
    bool Query(const std::wstring& sql, ResultSets* output) {
        ++calls; query = sql; *output = result; return available;
    }
};
}

int main() {
    SCENARIO("EX capability is available before the first database request after a World restart");
    g_settings.enabled=true;g_settings.oc_mode=false;g_settings.room_mode=true;
    for(int state:{0,1,2}) {
        g_schema_state.store(state);
        std::array<std::uint8_t,kModeReplyBytes> wire{};
        std::uint32_t bytes=0;std::int32_t status=0;ModeReply mode;
        CHECK(HandleMode(wire.data(),&bytes)==0);
        CHECK(ReadModeReply(wire.data(),bytes,&status,&mode));
        CHECK(mode.enabled && mode.oc_mode && (mode.flags&kModeFlagEx));
        CHECK(bool(mode.flags&kModeFlagSchemaReady)==(state==1));
    }
    std::array<std::uint8_t,kModeReplyBytes> wire{};
    std::uint32_t bytes=0;std::int32_t status=0;ModeReply mode;
    for(bool enabled:{false,true}) {
        g_settings.enabled=enabled;g_settings.room_mode=false;
        for(int state:{0,1,2}) {
            g_schema_state.store(state);
            CHECK(HandleMode(wire.data(),&bytes)==0 && ReadModeReply(wire.data(),bytes,&status,&mode));
            CHECK(!mode.oc_mode && !(mode.flags&kModeFlagEx));
        }
    }
    Database database;
    UnitRequest request; request.unit_uid = 42;
    std::array<std::uint8_t,kUnitRequestBytes> payload{};
    WriteUnitRequest(payload.data(),request);
    std::array<std::uint8_t,kChallengeItemsReplyBytes> reply{};
    std::uint32_t produced = 0;
    for (bool challenge : {false,true}) {
        const auto call = [&] {
            return challenge ? HandleChallengeItems(database,17,payload.data(),payload.size(),reply.data(),&produced)
                             : HandleOwnedItems(database,17,payload.data(),payload.size(),reply.data(),&produced);
        };
        SCENARIO("retained OC coupons keep their actual inventory identities, counts and effects on the wire");
        database.result = {{{L"3240003",L"3",L"0",L"1",L"1",L"18"},
                            {L"3240004",L"7",L"10000",L"0",L"1",L"18"},
                            {L"3240005",L"2",L"10000",L"0",L"9",L"15"}}};
        CHECK(call()==0 && produced==reply.size());
        ChallengeItemsReply value;
        CHECK(ReadChallengeItemsReply(reply.data(),produced,&value));
        CHECK(value.ret_val==0 && value.count==3);
        CHECK(value.items[0].item==3240003 && value.items[0].count==3 && value.items[0].lv_prevention==1);
        CHECK(value.items[1].item==3240004 && value.items[1].count==7 && value.items[1].probability_plus==10000);
        CHECK(value.items[1].min_level==1 && value.items[1].max_level==18);
        CHECK(value.items[2].item==3240005 && value.items[2].min_level==9 && value.items[2].max_level==15);
        CHECK(database.query.find(L"TUEV.AccountNumber = 17")!=std::wstring::npos);
        CHECK((database.query.find(L"@lv BETWEEN")!=std::wstring::npos)==challenge);
        for(const auto* verb:{L"EXEC ",L"INSERT ",L"UPDATE ",L"DELETE ",L"MERGE "})
            CHECK(database.query.find(verb)==std::wstring::npos);
        SCENARIO("empty holdings are distinct from failed database queries");
        database.result={{}};
        CHECK(call()==0 && ReadChallengeItemsReply(reply.data(),produced,&value) && value.count==0);
        database.available=false;
        CHECK(call()==static_cast<int>(WireStatus::database_unavailable));
        database.available=true;
    }
    return ai_test::Finish();
}
