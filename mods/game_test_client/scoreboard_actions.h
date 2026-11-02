#pragma once

// Supply only the mapped TAB action to the original input consumers inside
// this isolated test client. Native HUD input, animation, sorting and drawing
// still execute normally; no panel visibility or score field is fabricated.
class ScoreboardActions {
public:
    bool Install(const SdgoApi* api, SdgoMod* mod) {
        active_ = this;
        return api->HookCode(mod, "test.input_action_pressed", reinterpret_cast<void*>(&Pressed),
                             reinterpret_cast<void**>(&pressedOriginal_)) &&
               api->HookCode(mod, "test.input_action_held", reinterpret_cast<void*>(&Held),
                             reinterpret_cast<void**>(&heldOriginal_));
    }
    void Hold(bool value) { if (value && !held_) pressed_ = true; held_ = value; }
    uint32_t Queries() const { return queries_; }
private:
    using Query = uint32_t(__thiscall*)(void*, uint32_t);
    inline static ScoreboardActions* active_ = nullptr;
    Query pressedOriginal_ = nullptr, heldOriginal_ = nullptr;
    bool held_ = false, pressed_ = false;
    uint32_t queries_ = 0;
    static uint32_t __fastcall Pressed(void* input, void*, uint32_t action) {
        if (active_->held_ && action == 13) {
            ++active_->queries_;
            if (active_->pressed_) { active_->pressed_ = false; return 1; }
            return 0;
        }
        return active_->pressedOriginal_(input, action);
    }
    static uint32_t __fastcall Held(void* input, void*, uint32_t action) {
        if (active_->held_ && action == 13) { ++active_->queries_; return 1; }
        return active_->heldOriginal_(input, action);
    }
};
