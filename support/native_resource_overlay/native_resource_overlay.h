#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "sdgo/sdgo_api.h"

struct SdgoResourceOverlayBrokerApiV1
{
    uint32_t size;
    uint32_t version;
    int (__cdecl *RegisterBundle)(
        const wchar_t* mod_directory,
        const wchar_t* config_name,
        const char* owner,
        char* error,
        uint32_t error_capacity);
};

extern "C" __declspec(dllexport)
const SdgoResourceOverlayBrokerApiV1* __cdecl SdgoGetResourceOverlayBrokerV1();

namespace sdgo::resource_overlay
{
    struct NativeFileInfo
    {
        std::array<uint32_t, 9> words = {};
    };

    struct OverlayEntry
    {
        NativeFileInfo info;
    };

    struct BundleRegistration
    {
        std::string owner;
        std::string identity;
        std::wstring archivePath;
        uint32_t chunkSize = 0;
        std::unordered_map<std::wstring, OverlayEntry> entries;
    };

    using OpenObserver = void (__cdecl*)(
        const char* owner,
        const wchar_t* normalized_path,
        void* native_stream,
        void* context);

    class Provider
    {
    public:
        Provider();
        ~Provider();
        Provider(const Provider&) = delete;
        Provider& operator=(const Provider&) = delete;

        // Used by providers which already validated their own sealed archive.
        bool RegisterTrustedBundle(const BundleRegistration& bundle, std::string& error);

        // Used by the public broker ABI. The provider selects a strict parser
        // and allowlist from owner; callers cannot widen another provider's domain.
        bool RegisterBundleFiles(const wchar_t* modDirectory, const wchar_t* configName,
                                 const char* owner, std::string& error);

        // Installs the only CZoaPack::OpenFile hook owned by this provider.
        bool Start(const SdgoApi* api, SdgoMod* mod, OpenObserver observer,
                   void* observerContext, std::string& error);

        const SdgoResourceOverlayBrokerApiV1* BrokerApi() const;
        bool Active() const;

    private:
        static void* __fastcall OpenDetour(
            void* pack, void*, const void* nativeName, bool fullMemory);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    Provider& LocalProvider();
}
