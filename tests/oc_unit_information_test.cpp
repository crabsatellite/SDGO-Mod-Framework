#include "ai_test_support.h"
#include "../mods/oc_custom_client/unit_information.h"

#include <cstring>
#include <map>
#include <memory>

namespace oc = sdgo::oc_custom;
namespace info = oc::client::information;
namespace ui = oc::client::ui;
namespace {
template<class T> void Put(void* p, std::size_t at, T value) {
    std::memcpy(static_cast<std::uint8_t*>(p) + at, &value, sizeof(value));
}
template<class T> T Get(void* p, std::size_t at) {
    T value{}; std::memcpy(&value, static_cast<std::uint8_t*>(p) + at, sizeof(value)); return value;
}
std::wstring String(const ui::NativeString* value) {
    return {value->capacity < 8 ? value->data.small : value->data.pointer, value->length};
}
struct Control {
    std::array<std::uint8_t, 0x280> memory{};
    std::wstring text;
    std::vector<std::wstring> skins;
    int x = 0, y = 0, width = 0, height = 0;
};
std::map<std::wstring, std::unique_ptr<Control>> controls;
int writes = 0, refreshes = 0, parts_refreshes = 0;
std::array<std::uint8_t, oc::kUnitRecordBytes> rendered_parts{};
std::array<std::uint8_t, 601> unit_row{};
std::array<std::uint8_t, 112> weapon_row{};
const std::uint8_t* Lookup(std::int32_t id, int type) {
    if (id == 100 && type == 2) return unit_row.data();
    if (id == 90001 && type == 10) return weapon_row.data();
    return nullptr;
}
void __fastcall RefreshParts(void* child, void*) {
    ++parts_refreshes;
    std::memcpy(rendered_parts.data(),static_cast<std::uint8_t*>(child)+0x94,rendered_parts.size());
}
std::uint32_t AddControl(void* parent, const ui::NativeString* name, int x, int y, int width, int height) {
    auto control = std::make_unique<Control>();
    control->x = x; control->y = y; control->width = width; control->height = height;
    Put(control->memory.data(), 0x15C, parent);
    Put(control->memory.data(), 0x148, float(x));
    Put(control->memory.data(), 0x14C, float(y));
    Put(control->memory.data(), 0x154, width);
    Put(control->memory.data(), 0x158, height);
    controls[String(name)] = std::move(control);
    return 1;
}
std::uint32_t __fastcall Add(void* parent, void*, const ui::NativeString* name, int x, int y, int w, int h, std::uint32_t) {
    return AddControl(parent, name, x, y, w, h);
}
std::uint32_t __fastcall Icon(void* parent, void*, const ui::NativeString* name, int x, int y, int w, int h) {
    return AddControl(parent, name, x, y, w, h);
}
void* __fastcall Find(void*, void*, const ui::NativeString* name, int) {
    auto it = controls.find(String(name));
    return it == controls.end() ? nullptr : it->second->memory.data();
}
void __fastcall Text(void* control, void*, int, const ui::NativeString* value, int, int, int, int,
                     std::uint32_t, std::uint32_t, std::uint8_t, std::uint32_t, int) {
    ++writes;
    for (auto& item : controls) if (item.second->memory.data() == control) item.second->text = String(value);
}
void __fastcall Skin(void* control, void*, const ui::NativeString* path, std::uint8_t, std::uint32_t, std::uint32_t) {
    for(auto& item:controls)if(item.second->memory.data()==control)item.second->skins.push_back(String(path));
}
std::wstring SkinOf(const std::wstring& name) {
    auto& c=*controls[name];return c.skins.at(Get<unsigned>(c.memory.data(),0x18C));
}
void __fastcall Refresh(void*, void*) { ++refreshes; }
std::array<std::uint8_t, oc::kUnitRecordBytes> Record(std::uint64_t uid, std::uint8_t level) {
    std::array<std::uint8_t, oc::kUnitRecordBytes> record{};
    Put<std::uint32_t>(record.data(), 0, 100);
    Put(record.data(), 8, uid);
    oc::Carrier carrier;
    carrier.flags = oc::carrier_room_rule | oc::carrier_profile;
    carrier.oc_level = level; carrier.normal_points = 2; carrier.over_points = 7;
    carrier.delta = {3, 1600, 4, 5, 6, 700};
    oc::WriteCarrier(record.data(), carrier);
    return record;
}
} // namespace

int main() {
    SCENARIO("all six native base axes are stable across OC allocation and delayed information");
    unit_row[542] = 61;
    Put<int>(unit_row.data(), 321, 15200);
    Put<std::uint16_t>(unit_row.data(), 556, 109);
    Put<std::uint16_t>(unit_row.data(), 558, 117);
    unit_row[545] = 76;
    Put<int>(unit_row.data(), 597, 90001);
    Put<int>(weapon_row.data(), 108, 5900);
    oc::Carrier preview{};
    oc::Profile native{};
    native.base = {61, 15200, 109, 117, 76, 5900};
    const auto expected = oc::client::radar::FromProfile(native, oc::client::radar::kDefaultRanges);
    auto first = info::InformationChart(100, preview, nullptr, Lookup);
    CHECK(first.visible && first.layer == expected.layer);
    oc::Profile reply{};
    reply.base = {32, 9000, 88, 92, 41, 3900};
    CHECK(info::InformationChart(100, preview, &reply, Lookup).layer == expected.layer);
    preview.flags = oc::carrier_profile;
    preview.delta[5] = 400;
    reply.over[5] = 2;
    reply.over_stat[5] = 200;
    first = info::InformationChart(100, preview, nullptr, Lookup);
    const auto settled = info::InformationChart(100, preview, &reply, Lookup);
    CHECK(first.layer == settled.layer && settled.layer[3] == expected.layer[3]);
    CHECK(settled.layer[1][5] > settled.layer[3][5]);
    preview.delta[oc::stat_speed] = 6;
    reply.normal[oc::stat_speed] = 2;
    reply.normal_stat[oc::stat_speed] = 3;
    const auto combined = info::InformationChart(100, preview, nullptr, Lookup);
    const auto allocated = info::InformationChart(100, preview, &reply, Lookup);
    CHECK(combined.layer[0] == allocated.layer[0] && combined.layer[3] == allocated.layer[3]);
    CHECK(allocated.layer[3] == expected.layer[3]);
    CHECK(allocated.layer[2][oc::stat_speed] > combined.layer[2][oc::stat_speed]);
    CHECK(allocated.layer[2][oc::stat_special] == expected.layer[3][oc::stat_special]);
    Put<int>(unit_row.data(), 597, 90002); // weapon definition missing on this client
    CHECK(!info::InformationChart(100, preview, nullptr, Lookup).visible);
    native.base[5] = reply.base[5];
    CHECK(info::InformationChart(100, preview, &reply, Lookup).layer[3] ==
          oc::client::radar::FromProfile(native, oc::client::radar::kDefaultRanges).layer[3]);
    CHECK(!info::InformationChart(101, preview, nullptr, Lookup).visible);
    CHECK(info::InformationChart(101, preview, &reply, Lookup).layer ==
          oc::client::radar::FromProfile(reply, oc::client::radar::kDefaultRanges).layer);
    Put<int>(unit_row.data(), 597, 0); // the unit defines no fatal weapon
    const auto unarmed = info::InformationChart(100, preview, nullptr, Lookup);
    CHECK(unarmed.visible && unarmed.layer[3][oc::stat_special] == 0.0f);
    CHECK(info::InformationChart(100, preview, &reply, Lookup).layer[3] == unarmed.layer[3]);

    SCENARIO("public information round-trip preserves both builds and rejects malformed replies");
    oc::Information publicInfo, decoded;
    publicInfo.profile.unit_uid=42;publicInfo.profile.unit_number=100;publicInfo.profile.oc_level=15;
    publicInfo.profile.base={50,10000,100,100,50,4000};
    publicInfo.profile.normal_max_lv=4;publicInfo.profile.normal_budget=3;
    publicInfo.profile.normal={1,0,1,0,0,0};publicInfo.profile.over={0,2,0,0,0,1};
    publicInfo.profile.normal_stat={1,100,2,2,1,200};publicInfo.profile.over_stat={2,200,3,3,2,300};
    publicInfo.g1_skills={30001,30002,0,30004};publicInfo.g2_skills={30101,30102,30103,30104};publicInfo.g2_slots=4;
    std::array<std::uint8_t,oc::kInformationReplyBytes> wire{};int result=-1;
    oc::WriteInformationReply(wire.data(),0,publicInfo);
    CHECK(oc::ReadInformationReply(wire.data(),wire.size(),&result,&decoded) && result==0);
    CHECK(decoded.g1_skills==publicInfo.g1_skills && decoded.g2_skills==publicInfo.g2_skills && decoded.g2_slots==4);
    CHECK(decoded.profile.base==publicInfo.profile.base && decoded.profile.over==publicInfo.profile.over);
    CHECK(!oc::ReadInformationReply(wire.data(),wire.size()-1,&result,&decoded));
    CHECK(!oc::ReadInformationReply(nullptr,wire.size(),&result,&decoded));
    wire.back()=5;CHECK(!oc::ReadInformationReply(wire.data(),wire.size(),&result,&decoded));

    SCENARIO("displayed records, both carrier versions, all six axes and remaining points");
    auto record = Record(42, 15);
    auto view = info::Describe(record.data());
    CHECK(view.visible && view.title.find(L"EX") != std::wstring::npos);
    CHECK(view.points == L"剩余普通点 2 / Over 点 7");
    CHECK(view.gains[0] == L"推进力 +3" && view.gains[1] == L"耐久力 +1600");
    CHECK(view.gains[2] == L"攻击力 +4" && view.gains[3] == L"防御力 +5");
    CHECK(view.gains[4] == L"机动力 +6" && view.gains[5] == L"必杀技 +700");
    oc::Carrier carrier;
    CHECK(oc::ReadCarrier(record.data(), &carrier));
    carrier.flags |= oc::carrier_g1_profile;
    carrier.g1_skills = {30001, 30002, 30003, 30004};
    oc::WriteCarrier(record.data(), carrier);
    CHECK(info::Describe(record.data()).gains == view.gains);
    record[oc::kCarrierOffset + 8] ^= 1;
    CHECK(!info::Describe(record.data()).visible);
    CHECK(!info::Describe(nullptr).visible);
    record = Record(0, 9);
    CHECK(!info::Describe(record.data()).visible);
    record = Record(9, 9);
    CHECK(info::Describe(record.data()).title == L"OC 六维强化 · OVER CUSTOM 1");
    carrier.flags = oc::carrier_room_rule;
    oc::WriteCarrier(record.data(), carrier);
    CHECK(info::Describe(record.data()).points.empty());
    CHECK(info::Describe(record.data()).gains[0] == L"推进力 —");

    SCENARIO("native-page ownership, long level caption, switch, refresh, hide and recreation");
    ui::Bindings bindings;
    bindings.add = reinterpret_cast<ui::Bindings::Add>(&Add);
    bindings.add_icon = reinterpret_cast<ui::Bindings::AddIcon>(&Icon);
    bindings.find = reinterpret_cast<ui::Bindings::Find>(&Find);
    bindings.text = reinterpret_cast<ui::Bindings::Text>(&Text);
    bindings.skin = reinterpret_cast<ui::Bindings::Skin>(&Skin);
    bindings.refresh = reinterpret_cast<ui::Bindings::Refresh>(&Refresh);
    std::array<std::uint8_t, 0x280> root{}, service{};
    Put<int>(root.data(), 0x154, 629); Put<int>(root.data(), 0x158, 492);
    Put(service.data(), 0x6C, static_cast<void*>(root.data()));
    std::wstring levelName = L"LevelStatic";
    auto nativeName = ui::ViewOf(levelName);
    AddControl(root.data(), &nativeName, 114, 408, 207, 12);
    const std::wstring gaugeName = L"LevelGageIcon";
    const auto gaugeKey = ui::ViewOf(gaugeName);
    AddControl(root.data(), &gaugeKey, 134, 421, 167, 2);
    auto* level = controls[levelName]->memory.data();
    const std::wstring fullLevel = L"[尊级 终级强化Ex] 1000/1000(100%)";
    Put(level, 0x21C, ui::ViewOf(fullLevel)); Put<std::uint8_t>(level, 0x162, 1);
    record = Record(42, 15);
    std::memcpy(service.data() + 0x8C, record.data(), record.size());
    for (int i=0;i<4;++i) {
        const auto name=L"AttributeSkillIcon"+std::to_wstring(i);const auto key=ui::ViewOf(name);
        AddControl(root.data(),&key,330+i*65,367,48,48);
    }
    const auto before = service;
    info::Page page(&bindings,reinterpret_cast<info::Page::PartsRefresh>(&RefreshParts));
    page.Update(service.data());
    CHECK(service == before); // no writes to the authoritative/native record
    CHECK(Get<int>(root.data(), 0x158) == 492);
    CHECK(Get<std::uint8_t>(level, 0x162) == 0);
    CHECK(controls[L"OcInformationText0"]->text == fullLevel);
    CHECK(controls.count(L"OcInformationText15")==0);
    CHECK(controls.count(L"OcInformationBackground")==0);
    CHECK(controls.count(L"OcInformationTablePanel")==1);
    CHECK(controls.count(L"OcInformationWeaponsPanel")==1);
    CHECK(controls.count(L"OcInformationSkillsPanel")==1);
    for (std::size_t i = 0; i < 18; ++i) {
        if(i==1 || i==2 || (i>=9 && i<=15)){CHECK(controls.count(L"OcInformationText"+std::to_wstring(i))==0);continue;}
        const auto& control = *controls[L"OcInformationText" + std::to_wstring(i)];
        CHECK(control.x >= 0 && control.x + control.width <= 925);
        CHECK(control.y >= 66 && control.y + control.height <= 450);
    }
    CHECK(Get<int>(root.data(),0x154)==925);
    CHECK(Get<float>(controls[L"AttributeSkillIcon0"]->memory.data(),0x148)==626);
    const auto refreshesBefore = refreshes;
    const auto writesBefore = writes;
    page.Update(service.data());
    CHECK(writes == writesBefore && refreshes == refreshesBefore); // no repeated native text writes
    SCENARIO("self and other-player cards use identical snapshots without changing native records");
    std::array<std::uint8_t,0x150> parts{};
    auto ownRecord=record;ownRecord[136]=4;
    for(std::size_t i=0;i<4;++i)Put(ownRecord.data(),138+i*2,publicInfo.g2_skills[i]);
    std::memcpy(service.data()+0x8C,ownRecord.data(),ownRecord.size());
    std::memcpy(parts.data()+0x94,ownRecord.data(),ownRecord.size());
    Put(service.data(),0x13C,static_cast<void*>(parts.data()));
    const auto ownService=service;
    page.Update(service.data());
    page.AcceptInformation(publicInfo,page.generation());page.Update(service.data());
    const auto ownShape=oc::client::radar::CurrentInformation();
    const auto ownParts=rendered_parts;
    CHECK(ownShape.visible && service==ownService && parts_refreshes==1);
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5548.spr");
    page.Update(service.data());CHECK(parts_refreshes==1);
    oc::ClearCarrier(service.data()+0x8C);page.Update(service.data());
    CHECK(std::memcmp(parts.data()+0x94,ownRecord.data(),ownRecord.size())==0);
    const auto hiddenGeneration=page.generation();page.AcceptInformation(publicInfo,hiddenGeneration);
    std::memcpy(service.data()+0x8C,record.data(),record.size()); // remote record has no G2 slots
    std::memcpy(parts.data()+0x94,record.data(),record.size());
    const std::wstring blindName=L"GBlindStatic";const auto blindKey=ui::ViewOf(blindName);
    AddControl(root.data(),&blindKey,620,380,80,18);
    auto* blind=controls[blindName]->memory.data();Put<std::uint8_t>(blind,0x162,1);Put<std::uint8_t>(blind,0x163,1);
    page.Update(service.data());
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5546.spr");
    const auto remoteService=service;
    page.AcceptInformation(publicInfo,hiddenGeneration);page.Update(service.data());
    CHECK(Get<std::uint8_t>(blind,0x162)==1); // stale/hidden reply rejected
    page.AcceptInformation(publicInfo,page.generation());page.Update(service.data());
    CHECK(oc::client::radar::CurrentInformation().layer==ownShape.layer);
    CHECK(rendered_parts==ownParts && service==remoteService);
    CHECK(Get<std::uint8_t>(blind,0x162)==0 && Get<std::uint8_t>(blind,0x163)==0);
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5548.spr");
    // Navigation can happen before our frame hook processes the new UID.
    auto next=Record(99,9);std::memcpy(service.data()+0x8C,next.data(),next.size());
    auto wrong=publicInfo;wrong.profile.normal={};
    page.AcceptInformation(wrong,page.generation());
    std::memcpy(service.data()+0x8C,record.data(),record.size());page.Update(service.data());
    CHECK(oc::client::radar::CurrentInformation().layer==ownShape.layer);
    oc::ClearCarrier(service.data()+0x8C);page.Update(service.data());
    CHECK(Get<std::uint8_t>(blind,0x162)==1 && Get<std::uint8_t>(blind,0x163)==1);
    CHECK(std::memcmp(parts.data()+0x94,record.data(),record.size())==0);
    std::memcpy(service.data()+0x8C,record.data(),record.size());page.Update(service.data());
    SCENARIO("G1 changes invalidate a cached snapshot even when the OC revision stays unchanged");
    page.AcceptInformation(publicInfo,page.generation());page.Update(service.data());
    const auto oldLoadoutGeneration=page.generation();
    oc::Carrier changed;CHECK(oc::ReadCarrier(service.data()+0x8C,&changed));
    changed.flags|=oc::carrier_g1_profile;changed.g1_skills={30002,0,0,0};
    oc::WriteCarrier(service.data()+0x8C,changed);page.Update(service.data());
    CHECK(page.generation()!=oldLoadoutGeneration);
    const auto firstLoadoutGeneration=page.generation();
    changed.g1_skills={30003,0,0,0};oc::WriteCarrier(service.data()+0x8C,changed);
    page.Update(service.data());CHECK(page.generation()!=firstLoadoutGeneration);
    page.AcceptInformation(publicInfo,firstLoadoutGeneration);page.Update(service.data());
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5546.spr");
    std::memcpy(service.data()+0x8C,record.data(),record.size());page.Update(service.data());
    SCENARIO("hiding the page preserves a newer native child record for the same UID");
    page.AcceptInformation(publicInfo,page.generation());page.Update(service.data());
    auto newer=ownRecord;Put<std::uint16_t>(newer.data(),138,30109);
    std::memcpy(parts.data()+0x94,newer.data(),newer.size());
    oc::ClearCarrier(service.data()+0x8C);page.Update(service.data());
    CHECK(std::memcmp(parts.data()+0x94,newer.data(),newer.size())==0);
    std::memcpy(service.data()+0x8C,record.data(),record.size());page.Update(service.data());
    SCENARIO("late profile replies cannot replace the newly inspected unit");
    oc::Profile saved;
    saved.unit_uid=42;saved.unit_number=100;saved.state=0;saved.oc_level=9;
    saved.normal_max_lv=4;saved.normal_budget=3;saved.normal={1,0,1,0,0,0};saved.over={6,5,4,3,2,1};
    const auto firstGeneration=page.generation();
    page.AcceptInformation(oc::Information{saved},firstGeneration);
    page.Update(service.data());
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5548.spr");
    CHECK(SkinOf(L"OcInformationNormalSlot2")==L"sprs\\5553.spr");
    CHECK(SkinOf(L"OcInformationNormalSlot3")==L"sprs\\5555.spr");
    record = Record(43, 9);
    std::memcpy(service.data() + 0x8C, record.data(), record.size());
    page.Update(service.data());

    page.AcceptInformation(oc::Information{saved},firstGeneration);
    page.Update(service.data());
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5546.spr");
    saved.unit_uid=43;page.AcceptInformation(oc::Information{saved},page.generation());
    page.Update(service.data());
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5548.spr");
    oc::Carrier noProfile;noProfile.flags=oc::carrier_room_rule;
    oc::WriteCarrier(service.data()+0x8C,noProfile);
    page.Update(service.data());
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5546.spr");
    oc::ClearCarrier(service.data() + 0x8C);
    page.Update(service.data());
    CHECK(Get<int>(root.data(), 0x158) == 492);
    CHECK(Get<std::uint8_t>(level, 0x162) == 1);
    CHECK(Get<std::uint8_t>(controls[L"OcInformationNormalSlot0"]->memory.data(), 0x162) == 0);
    CHECK(Get<int>(root.data(),0x154)==629);
    CHECK(Get<float>(controls[L"AttributeSkillIcon0"]->memory.data(),0x148)==330);
    std::memcpy(service.data()+0x8C,record.data(),record.size());
    page.Update(service.data());
    CHECK(Get<int>(root.data(),0x154)==925);
    CHECK(Get<float>(controls[L"AttributeSkillIcon0"]->memory.data(),0x148)==626);
    page.Forget(service.data());
    controls.clear(); // the native owner destroys all children
    record = Record(44, 8);
    std::memcpy(service.data() + 0x8C, record.data(), record.size());
    page.Update(service.data());
    CHECK(controls.empty() && Get<int>(root.data(), 0x158) == 492); // wait for native child load
    AddControl(root.data(), &nativeName, 114, 408, 207, 12);
    AddControl(root.data(), &gaugeKey, 134, 421, 167, 2);
    level = controls[levelName]->memory.data();
    Put(level, 0x21C, ui::ViewOf(fullLevel)); Put<std::uint8_t>(level, 0x162, 1);
    page.Update(service.data());
    CHECK(Get<int>(root.data(), 0x158) == 492);
    CHECK(SkinOf(L"OcInformationNormalSlot0")==L"sprs\\5546.spr");
    page.Disable(service.data());
    CHECK(Get<int>(root.data(), 0x158) == 492);
    CHECK(Get<std::uint8_t>(level, 0x162) == 1);
    const auto disabledWrites = writes;
    page.Update(service.data());
    CHECK(writes == disabledWrites && Get<int>(root.data(), 0x158) == 492);
    page.Forget(service.data());
    CHECK(!info::Expand(320, 200, 180).valid);
    CHECK(!info::Expand(1024, 768, 700).valid);
    SCENARIO("ordinary-point indicators use earned budget and allocation, never Over points");
    oc::Profile slots;slots.normal_max_lv=4;slots.normal_budget=4;
    CHECK((info::NormalSlots(&slots)==std::array<unsigned,4>{1,1,1,1}));
    slots.normal={1,0,0,1,0,0};slots.over={9,9,9,9,9,9};
    CHECK((info::NormalSlots(&slots)==std::array<unsigned,4>{2,2,1,1}));
    slots.normal_max_lv=2;slots.normal_budget=1;
    CHECK((info::NormalSlots(&slots)==std::array<unsigned,4>{2,0,0,0}));
    CHECK((info::NormalSlots(nullptr)==std::array<unsigned,4>{0,0,0,0}));
    SCENARIO("factory and information charts share normalization and separate normal/Over layers");
    oc::Profile profile;
    profile.base={50,10000,100,100,50,4000};
    profile.normal={1,2,3,4,5,6};profile.over={2,1,4,1,3,2};
    profile.normal_stat={1,100,2,2,1,200};profile.over_stat={2,200,3,3,2,300};
    const auto shape=oc::client::radar::FromProfile(profile,oc::client::radar::kDefaultRanges);
    for(std::size_t i=0;i<oc::stat_count;++i){
        CHECK(shape.layer[0][i]==shape.layer[1][i]);
        CHECK(shape.layer[1][i]>shape.layer[2][i]);
        CHECK(shape.layer[2][i]>shape.layer[3][i]);
    }
    profile.base[0]=1;profile.normal[0]=0;profile.over[0]=0;
    CHECK(oc::client::radar::FromProfile(profile,oc::client::radar::kDefaultRanges).layer[3][0]==0);
    profile.base[0]=10000;
    CHECK(oc::client::radar::FromProfile(profile,oc::client::radar::kDefaultRanges).layer[3][0]==1);
    return ai_test::Finish();
}
