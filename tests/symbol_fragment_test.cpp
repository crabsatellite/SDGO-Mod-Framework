#include "../core/symbols.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace
{
    unsigned char g_shared[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    unsigned char g_private[] = {0x61, 0x62, 0x63, 0x64, 0x65};
    unsigned char g_other[] = {0x71, 0x72, 0x73, 0x74, 0x75};
    int g_failures = 0;

    void Check(bool condition, const char* description)
    {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
        if (!condition)
            ++g_failures;
    }

    bool Write(const std::wstring& path, const std::string& text)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") || !file)
            return false;
        const bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
        fclose(file);
        return ok;
    }

    std::string HostHeader(HMODULE host, const char* image)
    {
        const auto base = reinterpret_cast<uintptr_t>(host);
        const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        char text[256] = {};
        sprintf_s(text, "[host]\nimage=%s\ntimestamp=0x%08X\nsize_of_image=0x%08X\n\n",
                  image, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return text;
    }

    std::string DataSymbol(const char* name, HMODULE host, const unsigned char* address,
                           const char* expect)
    {
        const auto rva = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(host);
        char text[256] = {};
        sprintf_s(text, "[%s]\nkind=data\nrva=0x%08X\nexpect=%s\n\n",
                  name, static_cast<unsigned>(rva), expect);
        return text;
    }
}

int main()
{
    const HMODULE host = GetModuleHandleW(nullptr);
    wchar_t executable[MAX_PATH * 2] = {};
    GetModuleFileNameW(host, executable, MAX_PATH * 2);
    const wchar_t* slash = wcsrchr(executable, L'\\');
    const wchar_t* imageWide = slash ? slash + 1 : executable;
    char image[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_ACP, 0, imageWide, -1, image, sizeof(image), nullptr, nullptr);

    wchar_t temp[MAX_PATH] = {}, directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    swprintf_s(directory, L"%ssymbol-fragment-%lu-%lu", temp, GetCurrentProcessId(), GetTickCount());
    Check(CreateDirectoryW(directory, nullptr) != 0, "created isolated symbol fixture directory");

    const std::wstring globalPath = std::wstring(directory) + L"\\global.ini";
    const std::wstring fragmentPath = std::wstring(directory) + L"\\fragment.ini";
    const std::string header = HostHeader(host, image);
    Check(Write(globalPath, header + DataSymbol("shared", host, g_shared, "11 22 33 44 55")),
          "wrote global symbols");

    sdgo::SymbolTable global;
    Check(global.Load(globalPath, host, "fixture"), "global symbols match the fixture host");
    Check(global.VerifiedCount() == 1, "global data bytes verify");

    Check(Write(fragmentPath, header +
              DataSymbol("shared", host, g_shared, "11 22 33 44 55") +
              DataSymbol("private", host, g_private, "61 62 63 64 65")),
          "wrote matching fragment");
    sdgo::SymbolTable fragment;
    std::string error;
    Check(fragment.Load(fragmentPath, host, "fixture", "test", true),
          "fragment requires and accepts the exact image identity");
    Check(fragment.ValidateOverlay(global, error), "identical global definitions may be repeated");
    Check(fragment.FindWithFallback("private", global) == fragment.Find("private"),
          "fragment resolves its private symbol first");
    Check(fragment.FindWithFallback("shared", global) == fragment.Find("shared"),
          "matching repeated definition remains scoped to its owner");
    Check(fragment.FindWithFallback("missing", global) == nullptr, "missing symbols stay unavailable");

    Check(Write(fragmentPath, header + DataSymbol("second", host, g_other, "71 72 73 74 75")),
          "wrote second independent fragment");
    sdgo::SymbolTable second;
    Check(second.Load(fragmentPath, host, "fixture", "test", true) &&
              second.ValidateOverlay(global, error),
          "a second fragment validates independently");
    Check(second.FindWithFallback("private", global) == nullptr,
          "one Mod cannot see another Mod's private symbols");
    Check(second.FindWithFallback("shared", global) == global.Find("shared"),
          "a Mod without an override falls back to the global table");

    Check(Write(fragmentPath, header + DataSymbol("shared", host, g_private, "61 62 63 64 65")),
          "wrote conflicting-name fragment");
    sdgo::SymbolTable conflict;
    Check(conflict.Load(fragmentPath, host, "fixture", "test", true) &&
              !conflict.ValidateOverlay(global, error),
          "a fragment cannot replace a global name");

    Check(Write(fragmentPath, header + DataSymbol("alias", host, g_shared, "11 22 33 44 55")),
          "wrote global-alias fragment");
    sdgo::SymbolTable alias;
    Check(alias.Load(fragmentPath, host, "fixture", "test", true) &&
              alias.ValidateOverlay(global, error),
          "an equivalent feature-specific alias remains compatible");

    Check(Write(fragmentPath, header + DataSymbol("alias", host, g_shared, "11 22 33 44")),
          "wrote incompatible global-alias fragment");
    sdgo::SymbolTable badAlias;
    Check(badAlias.Load(fragmentPath, host, "fixture", "test", true) &&
              !badAlias.ValidateOverlay(global, error),
          "a global address cannot be given an incompatible alias");

    Check(Write(fragmentPath, header +
              DataSymbol("first_alias", host, g_private, "61 62 63 64 65") +
              DataSymbol("second_alias", host, g_private, "61 62 63 64")),
          "wrote incompatible internal-alias fragment");
    sdgo::SymbolTable internalAlias;
    Check(internalAlias.Load(fragmentPath, host, "fixture", "test", true) &&
              !internalAlias.ValidateOverlay(global, error),
          "one address cannot carry incompatible definitions");

    Check(Write(fragmentPath, header + DataSymbol("bad_bytes", host, g_private, "00 62 63 64 65")),
          "wrote byte-mismatch fragment");
    sdgo::SymbolTable badBytes;
    Check(badBytes.Load(fragmentPath, host, "fixture", "test", true) &&
              !badBytes.ValidateOverlay(global, error),
          "one rejected entry rejects the complete fragment");

    Check(Write(fragmentPath,
                "[host]\nimage=wrong.exe\ntimestamp=0\nsize_of_image=0\n\n" +
                DataSymbol("private", host, g_private, "61 62 63 64 65")),
          "wrote wrong-host fragment");
    sdgo::SymbolTable wrongHost;
    Check(!wrongHost.Load(fragmentPath, host, "fixture", "test", true),
          "a fragment for another executable is rejected");

    const auto base = reinterpret_cast<uintptr_t>(host);
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    char withoutImage[256] = {};
    sprintf_s(withoutImage, "[host]\ntimestamp=0x%08X\nsize_of_image=0x%08X\n\n",
              nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    Check(Write(fragmentPath, std::string(withoutImage) +
              DataSymbol("private", host, g_private, "61 62 63 64 65")),
          "wrote fragment without image name");
    sdgo::SymbolTable missingImage;
    Check(!missingImage.Load(fragmentPath, host, "fixture", "test", true),
          "per-Mod fragments require the executable image name");

    DeleteFileW(fragmentPath.c_str());
    DeleteFileW(globalPath.c_str());
    RemoveDirectoryW(directory);
    std::printf("Per-Mod symbol fragment regression: %d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
