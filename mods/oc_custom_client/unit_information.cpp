#include "unit_information.h"
#include "sdgo/unit_catalog_format.h"

#include <windows.h>
#include <cstring>
#include <algorithm>

namespace sdgo::oc_custom::client::information {
namespace {
constexpr std::size_t kRoot = 0x6C, kRecord = 0x8C;
constexpr std::size_t kZ = 0x150;
constexpr std::size_t kWidth = 0x154, kHeight = 0x158, kParent = 0x15C;
constexpr std::size_t kVisible = 0x162, kVisible2 = 0x163;
constexpr int kColumnShift = 296;

template<class T> T Read(const void* p, std::size_t offset = 0) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(p) + offset, sizeof(value));
    return value;
}
template<class T> void Write(void* p, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(p) + offset, &value, sizeof(value));
}
void Show(void* p, bool visible) {
    if (!p) return;
    Write<std::uint8_t>(p, kVisible, visible ? 1 : 0);
    Write<std::uint8_t>(p, kVisible2, visible ? 1 : 0);
}
void* Find(const ui::Bindings& bindings, void* root, const std::wstring& name, int type) {
    const auto key = ui::ViewOf(name);
    auto* p = bindings.find(root, &key, type);
    return p && Read<void*>(p, kParent) == root ? p : nullptr;
}
std::wstring Caption(void* control) {
    if (!control) return {};
    const auto value = Read<ui::NativeString>(control, 0x21C);
    if (value.length > 256 || value.capacity < value.length) return {};
    const auto* text = value.capacity < 8 ? value.data.small : value.data.pointer;
    if (!text || text[value.length]) return {};
    return std::wstring(text, value.length);
}

Page* g_page = nullptr;
bool g_runtime = false;
std::uint8_t** g_grid = nullptr;
friend_unit_gift::ClientExtensionSendFn g_send = nullptr;
friend_unit_gift::ClientExtensionStateFn g_transport = nullptr;
using Frame = void(__thiscall*)(void*, unsigned);
Frame g_frame = nullptr;
using PartsRefresh = void(__thiscall*)(void*);
PartsRefresh g_parts_refresh = nullptr;
const std::uint8_t* Grid(std::int32_t id, int type) {
    if (!g_grid || !*g_grid) return nullptr;
    auto* head = Read<std::uint8_t*>(*g_grid, 4);
    auto* node = Read<std::uint8_t*>(head, 4);
    auto* best = head;
    for (unsigned guard = 0; node && !Read<std::uint8_t>(node, 0x15) && guard < 64; ++guard) {
        if (Read<std::int32_t>(node, 0xC) < id) node = Read<std::uint8_t*>(node, 8);
        else { best = node; node = Read<std::uint8_t*>(node, 0); }
    }
    if (best == head || Read<std::int32_t>(best, 0xC) != id) return nullptr;
    auto* value = Read<std::uint8_t*>(best, 0x10);
    return value && Read<int>(value, 0) == type ? Read<std::uint8_t*>(value, 4) : nullptr;
}
void* Sprite(void* control) {
    auto** skins = control ? Read<std::uint8_t**>(control, 0x1A4) : nullptr;
    return skins && *skins ? Read<void*>(*skins, 0) : nullptr;
}
void __cdecl ProfileReply(void* ticket, std::uint16_t command, std::int32_t status,
                         const std::uint8_t* payload, std::uint32_t bytes) {
    if (command != static_cast<std::uint16_t>(Command::information) ||
        status != static_cast<int>(friend_unit_gift::WireStatus::success) || !payload) return;
    int result = 0; Information information;
    if (g_page && ReadInformationReply(payload, bytes, &result, &information) && result == 0)
        g_page->AcceptInformation(information, reinterpret_cast<std::uintptr_t>(ticket));
}
using Native = void(__thiscall*)(void*);
Native g_update = nullptr;
using Destroy = void*(__thiscall*)(void*, unsigned);
Destroy g_destroy = nullptr;
void UpdatePage(void* service) {
    __try { g_page->Update(service); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Retain the owner until its release hook. Forgetting here would add
        // another set of controls on every frame after a persistent fault.
        __try { g_page->Disable(service); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
void __fastcall UpdateHook(void* service, void*) {
    // Install the expanded layout before the native update positions sprites
    // for this frame. Newly constructed children may only be ready next tick.
    if (!g_page->shown()) UpdatePage(service);
    g_update(service);
    if (g_page->shown()) UpdatePage(service);
}
void* __fastcall DestroyHook(void* service, void*, unsigned flags) {
    g_page->Forget(service);
    return g_destroy(service, flags);
}
} // namespace

View Describe(const std::uint8_t* record) {
    View view;
    Carrier carrier;
    if (!record || !Read<std::uint32_t>(record, 0) || !Read<std::uint64_t>(record, 8) ||
        !ReadCarrier(record, &carrier) || !(carrier.flags & (carrier_oc_mode | carrier_room_rule)))
        return view;
    view.visible = true;
    view.title = L"OC 六维强化";
    constexpr const wchar_t* names[] = {L"推进力", L"耐久力", L"攻击力", L"防御力", L"机动力", L"必杀技"};
    if (!(carrier.flags & carrier_profile)) {
        view.title += L" · 未提供强化档案";
        for (std::size_t i = 0; i < stat_count; ++i) view.gains[i] = std::wstring(names[i]) + L" —";
        return view;
    }
    if (carrier.oc_level == 15) view.title += L" · OVER CUSTOM EX";
    else if (carrier.oc_level >= 9 && carrier.oc_level <= 14)
        view.title += L" · OVER CUSTOM " + std::to_wstring(carrier.oc_level - 8);
    else view.title += L" · 等级 " + std::to_wstring(carrier.oc_level);
    view.points = L"剩余普通点 " + std::to_wstring(carrier.normal_points) +
                  L" / Over 点 " + std::to_wstring(carrier.over_points);
    for (std::size_t i = 0; i < stat_count; ++i)
        view.gains[i] = std::wstring(names[i]) + L" " + (carrier.delta[i] >= 0 ? L"+" : L"") +
                        std::to_wstring(carrier.delta[i]);
    return view;
}

std::array<unsigned,4> NormalSlots(const Profile* profile) noexcept {
    std::array<unsigned,4> slots{};
    if(!profile)return slots;
    const unsigned budget=(std::min)({4u,static_cast<unsigned>(profile->normal_max_lv),static_cast<unsigned>(profile->normal_budget)});
    unsigned allocated=0;for(const auto points:profile->normal)allocated+=points;
    allocated=(std::min)(allocated,budget);
    for(unsigned i=0;i<slots.size();++i)slots[i]=i<allocated?2u:i<budget?1u:0u;
    return slots;
}

Layout Expand(int width, int height, int content_bottom) noexcept {
    Layout result;
    if (width < 600 || width > 1000 || height < 300 || height > 600 ||
        content_bottom < 300 || content_bottom > height) return result;
    result.width = (std::max)(925, width) - 64;
    result.top = content_bottom + 12;
    result.height = height;
    result.cell = 0;
    result.valid = true;
    return result;
}

void Page::Move(void* control, float x, float y) {
    if (!control) return;
    const auto found = std::find_if(moved_.begin(), moved_.end(), [&](const Position& p) { return p.control == control; });
    if (found == moved_.end()) moved_.push_back({control, Read<float>(control,0x148), Read<float>(control,0x14C),x,y});
    Write<float>(control,0x148,x); Write<float>(control,0x14C,y);
    bindings_->refresh(control);
}
void* Page::Icon(const std::wstring& name, int x, int y, int width, int height, const wchar_t* path) {
    const auto key = ui::ViewOf(name);
    if(path) bindings_->add(root_, &key, x, y, width, height,0);
    else bindings_->add_icon(root_, &key, x, y, width, height);
    auto* control = Find(*bindings_, root_, name, path?4:1);
    if (!control) return nullptr;
    added_.push_back(control);
    Write<std::uint8_t>(control,0x168,0);
    Write<std::uint8_t>(control,0x167,0);
    Write<float>(control,kZ,Read<float>(native_level_,kZ)+0.5f);
    if(path) {const std::wstring filename=path;const auto skin=ui::ViewOf(filename);bindings_->skin(control,&skin,0,0,0);}
    if(path && g_runtime && first_native_child_) {
        // CDialog traverses its child list for drawing. Put opaque backgrounds
        // before native children, retaining both their order and hit targets.
        auto* head=Read<void*>(root_,0x23C);
        unsigned guard=0;
        for(auto* link=Read<void*>(head);link && link!=head && guard++<256;link=Read<void*>(link)) {
            if(Read<void*>(link,8)!=control)continue;
            auto* next=Read<void*>(link);auto* prev=Read<void*>(link,4);
            Write<void*>(prev,0,next);Write<void*>(next,4,prev);
            prev=Read<void*>(first_native_child_,4);
            Write<void*>(link,0,first_native_child_);Write<void*>(link,4,prev);
            Write<void*>(prev,0,link);Write<void*>(first_native_child_,4,link);
            break;
        }
    }
    Show(control, false);
    return control;
}
bool Page::Skin(const std::wstring& name, const radar::Rect& source, const radar::Rect& target, const wchar_t* path, bool chart) {
    const float scale_x=target.width/source.width,scale_y=target.height/source.height;
    auto* control=Icon(name,static_cast<int>(target.x-source.x*scale_x),static_cast<int>(target.y-source.y*scale_y),
                       static_cast<int>(target.width),static_cast<int>(target.height),path);
    if(!control)return false;
    Write<float>(control,0x148,target.x-source.x*scale_x);Write<float>(control,0x14C,target.y-source.y*scale_y);
    if(auto* sprite=Sprite(control)) {
        auto* data=Read<std::uint8_t*>(sprite,0x168);
        auto** frames=data?Read<std::uint8_t**>(data,0xC):nullptr;
        if(!frames || !*frames)return false;
        Write<int>(control,kWidth,static_cast<int>(Read<int>(*frames,4)*scale_x));
        Write<int>(control,kHeight,static_cast<int>(Read<int>(*frames,8)*scale_y));
        Write<float>(sprite,0x94,scale_x);Write<float>(sprite,0x98,scale_y);Write<std::uint8_t>(sprite,0xA0,1);
    } else if(g_runtime)return false;
    bindings_->refresh(control);
    return !g_runtime || radar::InstallInformationSkin(control,target,chart);
}
bool Page::Attach(void* service, void* root) {
    const auto& b = *bindings_;
    auto* level = Find(b, root, L"LevelStatic", 3);
    auto* gauge = Find(b, root, L"LevelGageIcon", 1);
    if (!level || Caption(level).empty() || !gauge) return false;
    layout_ = Expand(Read<int>(root,kWidth), Read<int>(root,kHeight),
        static_cast<int>(Read<float>(gauge,0x14C)) + Read<int>(gauge,kHeight));
    if (!layout_.valid || (g_runtime && !radar::InstallInformation(root))) return false;
    service_ = service; root_ = root; native_level_ = level;
    if(g_runtime)first_native_child_=Read<void*>(Read<void*>(root,0x23C));
    native_width_ = Read<int>(root,kWidth); native_height_ = Read<int>(root,kHeight);
    native_x_ = Read<float>(root,0x148); native_y_ = Read<float>(root,0x14C);
    level_visible_ = Read<std::uint8_t>(level,kVisible);
    frame_sprite_ = Sprite(root);
    if (frame_sprite_) {
        frame_scale_x_ = Read<float>(frame_sprite_,0x94); frame_scale_y_ = Read<float>(frame_sprite_,0x98);
        frame_scaled_ = Read<std::uint8_t>(frame_sprite_,0xA0);
    }
    native_g2_title_ = Find(b,root,L"SkillTitleStatic1",3);
    if (native_g2_title_) g2_title_visible_ = Read<std::uint8_t>(native_g2_title_,kVisible);
    // Keep the native G2 icons, frames, locks and tooltips: only their geometry moves.
    for (int i=0; i<4; ++i) {
        for (const auto* prefix : {L"AttributeSkillIcon",L"SkillFrameIcon",L"LockIcon"}) {
            auto* c = Find(b,root,std::wstring(prefix)+std::to_wstring(i),1);
            if (c && Read<float>(c,0x148)<600) Move(c,Read<float>(c,0x148)+kColumnShift,Read<float>(c,0x14C));
        }
    }
    for (const auto* name : {L"GBlindIcon",L"AttributeSkillBlindIcon",L"GBlindStatic"}) {
        auto* c = Find(b,root,name,std::wcscmp(name,L"GBlindStatic") ? 1 : 3);
        if (c && Read<float>(c,0x148)<600) Move(c,Read<float>(c,0x148)+kColumnShift,Read<float>(c,0x14C));
    }
    if (auto* close = Find(b,root,L"CloseBtn",4)) Move(close,static_cast<float>(layout_.width+21),Read<float>(close,0x14C));
    // Extend the client's own information artwork without rescaling its text,
    // slots or line weights. Each child owns a native sprite; only its visible
    // source region is clipped. No replacement game art is shipped.
    constexpr const wchar_t* art=L"sprs\\17121.spr";
    if(!Skin(L"OcInformationFrameLeft",{0,0,310,465},{0,0,310,465},art) ||
       !Skin(L"OcInformationFrameMiddle",{310,0,4,465},{310,0,300,465},art) ||
       !Skin(L"OcInformationFrameRight",{314,0,309,465},{610,0,309,465},art) ||
       !Skin(L"OcInformationWeaponsPanel",{314,66,274,158},{314,66,274,158},art) ||
       !Skin(L"OcInformationSkillsPanel",{314,224,274,109},{314,224,274,109},art) ||
       !Skin(L"OcInformationG1Panel",{314,337,274,93},{314,337,274,93},art))return false;
    // The third column retains the native information frame and cell fill.
    // Its OC values use the shared chart renderer on the neutral hexagon.
    if(!Skin(L"OcInformationRadarLeft",{315,68,5,264},{611,68,5,264},art) ||
       !Skin(L"OcInformationRadarRight",{583,68,5,264},{879,68,5,264},art) ||
       !Skin(L"OcInformationRadarTop",{320,68,263,5},{616,68,263,5},art) ||
       !Skin(L"OcInformationRadarBottom",{320,327,263,5},{616,327,263,5},art) ||
       !Skin(L"OcInformationRadarPanel",{425,107,150,20},{616,73,263,254},art,true))return false;
    // Keep all six table rows, including the single-line level cell and
    // experience track, at their original dimensions.
    if(!Skin(L"OcInformationTablePanel",{41,315,261,108},{41,315,261,108},art))return false;
    constexpr const wchar_t* table_titles[]={L"PositionTitleStatic",L"TerrainTitleStatic",L"InfluenceTitleStatic0",L"InfluenceTitleStatic1",L"BattleTitleStatic"};
    constexpr const wchar_t* table_values[]={L"PositionStatic",L"TerrainStatic",L"InfluenceStatic0",L"InfluenceStatic1",L"BattleStatic"};
    for(int row=0;row<5;++row) {
        for(const auto* name:{table_titles[row],table_values[row]}) {
            auto* control=Find(b,root,name,3);
            if(control)Move(control,Read<float>(control,0x148),317.0f+row*18);
        }
    }
    if(auto* title=Find(b,root,L"LevelTitleStatic",3))Move(title,41,407);
    Move(gauge,134,421);
    for (int i=0;i<4;++i) {
        auto* icon = Icon(L"OcInformationG1Icon"+std::to_wstring(i),330+i*65,367,48,48,nullptr);
        if (!icon) return false;
        g1_icons_[i] = icon;
        auto* source = Find(b,root,L"AttributeSkillIcon"+std::to_wstring(i),1);
        auto** skins = source ? Read<std::uint8_t**>(source,0x1A4) : nullptr;
        if (skins && *skins) b.skin(icon,reinterpret_cast<const ui::NativeString*>(*skins+4),
            Read<std::uint8_t>(*skins,0x44),Read<unsigned>(*skins,0x3C),Read<unsigned>(*skins,0x40));
    }
    for(unsigned i=0;i<normal_icons_.size();++i) {
        normal_icons_[i]=Icon(L"OcInformationNormalSlot"+std::to_wstring(i),648+i*51,304,49,19,nullptr);
        if(!normal_icons_[i])return false;
        for(unsigned state=0;state<3;++state) {
            const auto path=L"sprs\\"+std::to_wstring(5546+i*3+state)+L".spr";const auto key=ui::ViewOf(path);
            b.skin(normal_icons_[i],&key,0,0,0);
        }
    }
    constexpr int axis_x[6] = {610,850,642,770,770,642};
    constexpr int axis_y[6] = {184,184,90,280,90,280};
    for (std::size_t i=0; i<labels_.size(); ++i) {
        if(i==1 || i==2 || (i>=9 && i<=15))continue;
        int x=134,y=407,width=168;
        if (i>=3 && i<=8) {const auto axis=i-3;x=axis_x[axis];y=axis_y[axis];width=axis==1?62:90;}
        else if (i==16) {x=334;y=344;width=240;}
        else if (i==17) {x=630;y=344;width=240;}
        const auto name=L"OcInformationText"+std::to_wstring(i);const auto key=ui::ViewOf(name);
        b.add(root,&key,x,y,width,i==0?14:18,0);
        labels_[i]=Find(b,root,name,4);
        if (!labels_[i]) return false;
        added_.push_back(labels_[i]);
        Write<float>(labels_[i],kZ,27.0f);
        Write<std::uint8_t>(labels_[i],0x167,0);Write<std::uint8_t>(labels_[i],0x168,0);
        Show(labels_[i],false);
    }
    return true;
}

void Page::Label(std::size_t index, const std::wstring& text) {
    if (captions_[index] == text) return;
    const auto native = ui::ViewOf(text);
    bindings_->text(labels_[index], 0, &native, 0, 0, Read<int>(labels_[index], kWidth), Read<int>(labels_[index],kHeight),
                    index == 1 ? 0xFFFFFF99 : 0xFF9CB8C8, 0xFF000000, 0, 1, 0);
    captions_[index] = text;
}

void Page::Hide() {
    RestoreParts();
    if (uid_ || requested_ || profile_known_) {
        uid_=0; unit_=0; ++generation_;
        requested_=profile_known_=information_known_=false;
    }
    if (!root_) return;
    for (auto* control : added_) Show(control,false);
    radar::SetInformation({},0,0,0);
    if (shown_) {
        Write<int>(root_,kWidth,native_width_); Write<int>(root_,kHeight,native_height_);
        Write<float>(root_,0x148,native_x_);Write<float>(root_,0x14C,native_y_);
        for (const auto& pos : moved_) {Write<float>(pos.control,0x148,pos.x);Write<float>(pos.control,0x14C,pos.y);bindings_->refresh(pos.control);}
        if (frame_sprite_) {Write<float>(frame_sprite_,0x94,frame_scale_x_);Write<float>(frame_sprite_,0x98,frame_scale_y_);Write<std::uint8_t>(frame_sprite_,0xA0,frame_scaled_);}
        if (native_g2_title_) Show(native_g2_title_,g2_title_visible_!=0);
        Show(native_level_,level_visible_!=0);
        bindings_->refresh(root_);
    }
    shown_=false;
}
void Page::AcceptInformation(const Information& information, std::uintptr_t generation) {
    const auto& profile=information.profile;
    if (!shown_ || !service_ || generation!=generation_ || profile.unit_uid!=uid_ ||
        profile.unit_number!=unit_ || profile.state || information.g2_slots>4) return;
    // Native navigation can replace the record before the next Update call.
    const auto* record=static_cast<const std::uint8_t*>(service_)+kRecord;
    Carrier carrier;
    if (Read<std::uint64_t>(record,8)!=uid_ || Read<std::uint32_t>(record)!=unit_ ||
        !ReadCarrier(record,&carrier) || carrier.revision!=revision_ || carrier.flags!=carrier_flags_ ||
        std::memcmp(record,requested_record_.data(),requested_record_.size())) return;
    profile_=profile; profile_known_=profile.oc_level!=0;
    information_=information; information_known_=true;
}
void Page::RestoreParts() {
    if (parts_service_ && service_ && Read<void*>(service_,0x13C)==parts_service_) {
        // Restore only our currently displayed record; native navigation may
        // already have installed the next unit in the same child service.
        auto* record=static_cast<std::uint8_t*>(parts_service_)+0x94;
        if (parts_applied_ && !std::memcmp(record,parts_display_.data(),parts_display_.size())) {
            std::memcpy(record,parts_record_.data(),parts_record_.size());
            if(parts_refresh_)parts_refresh_(parts_service_);
        }
    }
    for(const auto& item:parts_visibility_) {
        Write<std::uint8_t>(item.control,kVisible,item.first);
        Write<std::uint8_t>(item.control,kVisible2,item.second);
    }
    parts_visibility_.clear(); parts_service_=nullptr; parts_applied_=false;
}
void Page::UpdateParts(const std::uint8_t* record) {
    if (!information_known_ || !parts_refresh_) return;
    auto* child=Read<void*>(service_,0x13C);
    if (!child) return;
    if (parts_service_ && parts_service_!=child) RestoreParts();
    auto* native=static_cast<std::uint8_t*>(child)+0x94;
    if (!parts_service_) {
        parts_service_=child;
        std::memcpy(parts_record_.data(),native,parts_record_.size());
        for(const auto* name:{L"GBlindIcon",L"GBlindStatic",L"AttributeSkillBlindIcon"}) {
            auto* control=Find(*bindings_,root_,name,std::wcscmp(name,L"GBlindStatic")?1:3);
            if(control)parts_visibility_.push_back({control,Read<std::uint8_t>(control,kVisible),Read<std::uint8_t>(control,kVisible2)});
        }
    }
    std::array<std::uint8_t,kUnitRecordBytes> display;
    std::memcpy(display.data(),record,display.size());
    display[136]=information_.g2_slots;
    for(std::size_t i=0;i<4;++i)Write(display.data(),kCustomSkillOffset+i*2,information_.g2_skills[i]);
    if(std::memcmp(native,display.data(),display.size()) || !parts_applied_) {
        std::memcpy(native,display.data(),display.size());
        parts_display_=display;
        parts_refresh_(child); // original icons, frames, slot locks and tooltips
        parts_applied_=true;
    }
    for(const auto& item:parts_visibility_)Show(item.control,false);
}
void Page::UpdateChart(const std::uint8_t* record, const Carrier& carrier) {
    const auto uid=Read<std::uint64_t>(record,8); const auto unit=Read<std::uint32_t>(record,0);
    if (uid_!=uid || unit_!=unit || revision_!=carrier.revision || carrier_flags_!=carrier.flags ||
        std::memcmp(record,requested_record_.data(),requested_record_.size())) {
        RestoreParts(); uid_=uid;unit_=unit;revision_=carrier.revision;carrier_flags_=carrier.flags;++generation_;
        requested_=profile_known_=information_known_=false;profile_={};
        std::memcpy(requested_record_.data(),record,requested_record_.size());
    }
    if (!requested_ && g_send && g_transport && g_transport()==1) {
        std::uint8_t payload[kUnitRequestBytes]{};UnitRequest request;request.unit_uid=uid;
        WriteUnitRequest(payload,request);
        requested_=g_send(static_cast<std::uint16_t>(Command::information),payload,sizeof(payload),&ProfileReply,
                         reinterpret_cast<void*>(generation_))!=0;
    }
    const auto shape = InformationChart(unit, carrier, information_known_ ? &information_.profile : nullptr, Grid);
    radar::SetInformation(shape,750,191,94);
    constexpr const wchar_t* names[]={L"推进力",L"耐久力",L"攻击力",L"防御力",L"机动力",L"必杀技"};
    for(std::size_t i=0;i<stat_count;++i) {
        Label(i+3,names[i]);
    }
    const auto states=NormalSlots(profile_known_?&profile_:nullptr);
    for(unsigned i=0;i<normal_icons_.size();++i) {
        // Skin() appends to the native vector; select its existing state
        // instead of appending another sprite on each allocation update.
        if(normal_skins_[i]!=states[i]+1) {
            Write<int>(normal_icons_[i],0x18C,static_cast<int>(states[i]));
            if(g_runtime)reinterpret_cast<void(__thiscall*)(void*)>(Read<void**>(normal_icons_[i])[0x1C/sizeof(void*)])(normal_icons_[i]);
            normal_skins_[i]=states[i]+1;
        }
    }
    for (std::size_t i=0;i<4;++i) {
        const unsigned skill=information_known_ ? information_.g1_skills[i] :
            (carrier.flags & carrier_g1_profile) ? carrier.g1_skills[i] : 0;
        bool loaded=skill && skill==g1_shown_[i];
        if (skill!=g1_shown_[i] && skill && g_frame) {
            for (int id=3664001;id<3668001;++id) {
                const auto* row=Grid(id,0x5D);if(!row)break;
                if(Read<unsigned>(row,5)==skill) {if(auto* sprite=Sprite(g1_icons_[i])){g_frame(sprite,row[0x16]);loaded=true;}break;}
            }
        }
        g1_shown_[i]=loaded?static_cast<std::uint16_t>(skill):0;Show(g1_icons_[i],loaded);
    }
}
radar::Shape InformationChart(std::uint32_t unit, const Carrier& carrier,
                             const Profile* saved, GridLookup lookup) {
    Profile shown{};
    if (saved) {
        shown = *saved;
    } else if (carrier.flags & carrier_profile) {
        shown.over.fill(1);
        std::copy(carrier.delta.begin(), carrier.delta.end(), shown.over_stat.begin());
    }

    // The same unit and fatal weapon define the base before and after the
    // inspection reply. Use the reply for missing definitions and OC layers.
    const auto* native = lookup(unit, unit_catalog::kUnitType);
    if (native) {
        shown.base[stat_speed] = native[542];
        shown.base[stat_hp] = Read<std::int32_t>(native, 321);
        shown.base[stat_attack] = Read<std::uint16_t>(native, 556);
        shown.base[stat_defense] = Read<std::uint16_t>(native, 558);
        shown.base[stat_agility] = native[545];
        const auto fatal = Read<std::int32_t>(native, 597);
        if (!fatal) {
            shown.base[stat_special] = 0;
        } else if (const auto* weapon = lookup(fatal, unit_catalog::kWeaponType)) {
            shown.base[stat_special] = Read<std::int32_t>(weapon, 108);
        } else if (!saved) {
            return {};
        }
    } else if (!saved) {
        return {};
    }

    auto ranges = radar::kDefaultRanges;
    for (int id = 27501; id <= 27506; ++id) {
        if (const auto* row = lookup(id, 7)) {
            const auto axis = row[5];
            const auto low = Read<float>(row, 6), high = Read<float>(row, 10);
            if (axis < stat_count && high > low) ranges[axis] = {low, high};
        }
    }
    return radar::FromProfile(shown, ranges);
}
void Page::Update(void* service) {
    if (!service) return;
    auto* root=Read<void*>(service,kRoot);if(!root)return;
    if(root_ && (root!=root_ || service!=service_))Forget(service_);
    if(disabled_)return;
    if(Find(*bindings_,root,L"GiftNotePanel",1)){Hide();return;}
    const auto* record=static_cast<const std::uint8_t*>(service)+kRecord;
    const auto view=Describe(record);
    if(!view.visible){Hide();return;}
    if(!root_ && !Attach(service,root)){Hide();return;}
    if(!labels_.back()){Hide();return;}
    const auto level=Caption(native_level_);
    Label(0,level);
    Label(16,L"部件 1.0");Label(17,L"部件 2.0");
    if(!shown_) {
        for(const auto& pos:moved_){Write<float>(pos.control,0x148,pos.target_x);Write<float>(pos.control,0x14C,pos.target_y);bindings_->refresh(pos.control);}
        Write<int>(root_,kWidth,layout_.width+64);Write<int>(root_,kHeight,layout_.height);
        Write<float>(root_,0x148,(std::min)(native_x_,static_cast<float>(1024-layout_.width-64)));
        Write<float>(root_,0x14C,(std::min)(native_y_,static_cast<float>(768-layout_.height)));
        if(frame_sprite_) {
            // The region children render the same frame with its native scale.
            Write<float>(frame_sprite_,0x94,0.0f);
            Write<std::uint8_t>(frame_sprite_,0xA0,1);
        }
        if(native_g2_title_)Show(native_g2_title_,false);
        bindings_->refresh(root_);for(auto* control:added_)Show(control,true);shown_=true;
    }
    if(native_g2_title_)Show(native_g2_title_,false);
    Show(native_level_,false);
    Carrier carrier;ReadCarrier(record,&carrier);UpdateChart(record,carrier);UpdateParts(record);
}

void Page::Disable(void* service) {
    if (service != service_) return;
    disabled_ = true;
    Hide();
}

void Page::Forget(void* service) noexcept {
    if (service != service_) return;
    radar::ForgetInformation(root_);
    moved_.clear();added_.clear();g1_icons_={};g1_shown_={};normal_icons_={};normal_skins_={};
    frame_sprite_=native_g2_title_=first_native_child_=nullptr;
    uid_=0;unit_=0;++generation_;requested_=profile_known_=information_known_=false;
    parts_service_=nullptr;parts_visibility_.clear();parts_applied_=false;
    service_ = root_ = native_level_ = nullptr;
    labels_ = {};
    for (auto& caption : captions_) caption.clear();
    shown_ = false;
    disabled_ = false;
    native_height_ = 0;
}

bool Bind(const SdgoApi* api, SdgoMod* mod, const ui::Bindings* bindings) {
    if (!api->HookSlot || !bindings->refresh) return false;
    g_grid=reinterpret_cast<std::uint8_t**>(api->Symbol(mod,"oc_custom.grid_singleton"));
    g_parts_refresh=reinterpret_cast<PartsRefresh>(api->Symbol(mod,"oc_custom.information_parts_refresh"));
    g_frame=reinterpret_cast<Frame>(api->Symbol(mod,"oc_custom.information_sprite_frame"));
    const auto gift=GetModuleHandleW(L"friend_unit_gift_client.dll");
    g_send=reinterpret_cast<friend_unit_gift::ClientExtensionSendFn>(GetProcAddress(gift,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT));
    g_transport=reinterpret_cast<friend_unit_gift::ClientExtensionStateFn>(GetProcAddress(gift,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_STATE_EXPORT));
    if(!g_grid || !g_frame || !g_parts_refresh || !g_send || !g_transport)return false;
    g_runtime=true;
    static Page page(bindings,g_parts_refresh);
    g_page = &page;
    return api->HookSlot(mod, "oc_custom.information_destroy", reinterpret_cast<void*>(&DestroyHook),
                         reinterpret_cast<void**>(&g_destroy)) &&
           api->HookSlot(mod, "oc_custom.information_update", reinterpret_cast<void*>(&UpdateHook),
                         reinterpret_cast<void**>(&g_update));
}
} // namespace sdgo::oc_custom::client::information
