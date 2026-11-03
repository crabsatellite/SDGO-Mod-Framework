#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sdgo
{
    // One entry of sdgo/symbols/<host_key>.ini.
    //   kind=code  callable or detourable function; `expect` must match its first bytes
    //   kind=site  any other code bytes a mod depends on; `expect` must match
    //   kind=slot  pointer-sized slot (vtable entry); it must currently hold base+`target_rva`
    //   kind=data  plain address, verified only when `expect` is given
    struct Symbol
    {
        std::string name;
        std::string kind;
        uintptr_t address = 0;
        std::vector<int> expect; // byte values, -1 for a "??" wildcard
        uintptr_t expectedTarget = 0;
        bool verified = false;
        std::string problem;
    };

    class SymbolTable
    {
    public:
        // Loads and verifies every entry against the running host. Returns false when the
        // file is absent or was written for a different build. Per-Mod fragments require
        // the executable image name as well as the timestamp and image size to match.
        bool Load(const std::wstring& path, HMODULE host, const char* hostKey,
                  const char* logSource = "core", bool requireImage = false);
        const Symbol* Find(const char* name) const;
        const Symbol* FindWithFallback(const char* name, const SymbolTable& fallback) const;
        // A fragment may repeat an equivalent global binding or alias, but it may not
        // replace a name or attach an incompatible definition to an existing address.
        bool ValidateOverlay(const SymbolTable& base, std::string& error) const;
        size_t Count() const { return m_symbols.size(); }
        size_t VerifiedCount() const;

    private:
        std::vector<Symbol> m_symbols;
    };
}
