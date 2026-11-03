#pragma once

struct SdgoApi;
struct SdgoMod;

namespace room_style_preview_overlay {

// Validates the generated one-resource overlay and installs the executable's
// native CZoaPack::OpenFile detour.  The host loader still allocates, owns and
// destroys every generated UI control.
bool Initialize(const SdgoApi* api, SdgoMod* mod);

} // namespace room_style_preview_overlay
