#pragma once

// Supply mapped TAB input and opt-in gameplay input to the original consumers
// inside this isolated client. Native input, animation, motion and drawing
// still execute normally; no panel, score, position or FSM state is fabricated.
class ScoreboardActions {
public:
    bool Install(const SdgoApi* api, SdgoMod* mod) {
        active_ = this;
        return api->HookCode(mod, "test.input_action_pressed", reinterpret_cast<void*>(&Pressed),
                             reinterpret_cast<void**>(&pressedOriginal_)) &&
               api->HookCode(mod, "test.input_action_held", reinterpret_cast<void*>(&Held),
                             reinterpret_cast<void**>(&heldOriginal_)) &&
               api->HookCode(mod, "test.input_action_down", reinterpret_cast<void*>(&Down),
                             reinterpret_cast<void**>(&downOriginal_));
    }
    void Hold(bool value) { if (value && !held_) pressed_ = true; held_ = value; }
    uint32_t Queries() const { return queries_; }
    void* NativeInput() const { return nativeInput_; }
    void HoldGameplay(uint32_t action, bool value) {
        if (action >= 32) return;
        const uint32_t bit=1u << action;
        if (value && !(gameplayMask_ & bit)) gameplayEdgeMask_ |= bit;
        else gameplayEdgeMask_ &= ~bit;
        if (value) gameplayMask_ |= bit;
        else gameplayMask_ &= ~bit;
    }
    uint32_t GameplayQueries(uint32_t action) const { return action < 32 ? gameplayQueries_[action] : 0; }
private:
    using Query = uint32_t(__thiscall*)(void*, uint32_t);
    inline static ScoreboardActions* active_ = nullptr;
    Query pressedOriginal_ = nullptr, heldOriginal_ = nullptr;
    Query downOriginal_ = nullptr;
    bool held_ = false, pressed_ = false;
    uint32_t queries_ = 0;
    uint32_t gameplayMask_ = 0, gameplayQueries_[32] = {};
    uint32_t gameplayEdgeMask_ = 0;
    void* nativeInput_ = nullptr;
    static uint32_t __fastcall Pressed(void* input, void*, uint32_t action) {
        active_->nativeInput_ = input;
        if (action < 32 && (active_->gameplayEdgeMask_ & (1u << action))) return 1;
        if (active_->held_ && action == 13) {
            ++active_->queries_;
            if (active_->pressed_) { active_->pressed_ = false; return 1; }
            return 0;
        }
        return active_->pressedOriginal_(input, action);
    }
    static uint32_t __fastcall Held(void* input, void*, uint32_t action) {
        active_->nativeInput_ = input;
        if (action < 32 && (active_->gameplayMask_ & (1u << action))) {
            ++active_->gameplayQueries_[action]; return 1;
        }
        if (active_->held_ && action == 13) { ++active_->queries_; return 1; }
        return active_->heldOriginal_(input, action);
    }
    static uint32_t __fastcall Down(void* input, void*, uint32_t action) {
        active_->nativeInput_ = input;
        if (action < 32 && (active_->gameplayEdgeMask_ & (1u << action))) return 1;
        return active_->downOriginal_(input, action);
    }
};
