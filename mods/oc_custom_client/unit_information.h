#pragma once

#include "original_ui.h"
#include "radar.h"
#include <vector>

namespace sdgo::oc_custom::client::information {

struct View {
    bool visible = false;
    std::wstring title;
    std::wstring points;
    std::array<std::wstring, stat_count> gains;
};

// Only the displayed native record is an authority. Never fall back to the
// active factory unit, a previous page, or a lobby query for a different UID.
View Describe(const std::uint8_t* record);

using GridLookup = const std::uint8_t* (*)(std::int32_t id, int type);
radar::Shape InformationChart(std::uint32_t unit, const Carrier& carrier,
                             const Profile* saved, GridLookup lookup);

// Native I..IV skins: unavailable, available, allocated.
std::array<unsigned,4> NormalSlots(const Profile* profile) noexcept;

struct Layout {
    int width = 0;
    int top = 0;
    int height = 0;
    int cell = 0;
    bool valid = false;
};
Layout Expand(int width, int height, int content_bottom) noexcept;

class Page {
public:
    using PartsRefresh = void(__thiscall*)(void*);
    explicit Page(const ui::Bindings* bindings, PartsRefresh parts_refresh = nullptr)
        : bindings_(bindings), parts_refresh_(parts_refresh) {}
    void Update(void* service);
    bool shown() const { return shown_; }
    void Disable(void* service);
    void AcceptInformation(const Information& information, std::uintptr_t generation);
    std::uintptr_t generation() const { return generation_; }
    // Called before the native service releases its children. No retained
    // control or native record outlives its owner.
    void Forget(void* service) noexcept;

private:
    void Hide();
    void RestoreParts();
    void UpdateParts(const std::uint8_t* record);
    bool Attach(void* service, void* root);
    void Label(std::size_t index, const std::wstring& text);
    void Move(void* control, float x, float y);
    void* Icon(const std::wstring& name, int x, int y, int width, int height, const wchar_t* sprite);
    bool Skin(const std::wstring& name, const radar::Rect& source, const radar::Rect& target, const wchar_t* path, bool chart=false);
    void UpdateChart(const std::uint8_t* record, const Carrier& carrier);
    struct Position { void* control; float x, y, target_x, target_y; };
    std::vector<Position> moved_;
    std::vector<void*> added_;
    std::array<void*,4> g1_icons_{};
    std::array<void*,4> normal_icons_{};
    std::array<unsigned,4> normal_skins_{};
    std::array<std::uint16_t,4> g1_shown_{};
    void* native_g2_title_ = nullptr;
    void* frame_sprite_ = nullptr;
    void* first_native_child_ = nullptr;
    float frame_scale_x_ = 1, frame_scale_y_ = 1;
    std::uint8_t frame_scaled_ = 0, g2_title_visible_ = 0;
    int native_width_ = 0;
    float native_x_ = 0, native_y_ = 0;
    std::uint64_t uid_ = 0;
    std::uint32_t unit_ = 0, revision_ = 0;
    std::uint8_t carrier_flags_ = 0;
    std::uintptr_t generation_ = 0;
    bool requested_ = false, profile_known_ = false;
    Profile profile_{};
    Information information_{};
    std::array<std::uint8_t, kUnitRecordBytes> requested_record_{};
    bool information_known_ = false, parts_applied_ = false;
    void* parts_service_ = nullptr;
    std::array<std::uint8_t, kUnitRecordBytes> parts_record_{}, parts_display_{};
    struct Visibility { void* control; std::uint8_t first, second; };
    std::vector<Visibility> parts_visibility_;
    const ui::Bindings* bindings_;
    PartsRefresh parts_refresh_;
    void* service_ = nullptr;
    void* root_ = nullptr;
    void* native_level_ = nullptr;
    std::uint8_t level_visible_ = 0;
    int native_height_ = 0;
    Layout layout_{};
    std::array<void*, 18> labels_{};
    std::array<std::wstring, 18> captions_{};
    bool shown_ = false;
    bool disabled_ = false;
};

bool Bind(const SdgoApi* api, SdgoMod* mod, const ui::Bindings* bindings);

} // namespace sdgo::oc_custom::client::information
