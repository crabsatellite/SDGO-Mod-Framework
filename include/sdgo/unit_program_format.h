#pragma once
#include "script_registration.h"
#include "unit_catalog_format.h"
#include <set>
#include <stdexcept>

namespace sdgo::unit_catalog {

// Optional SDGOPR1 sidecar. It binds the exact catalog and imported weapon bytes;
// no runtime address, baseline replacement or arbitrary file pointer is accepted.
class ProgramBundle {
public:
    std::vector<sdgo::scripts::Candidate> candidates;

    bool Parse(const std::vector<uint8_t>& bytes, const std::string& catalogSha256,
               const Catalog& catalog, std::string& error) {
        candidates.clear(); error.clear();
        try {
            Reader reader{bytes};
            reader.require(bytes.size() >= 64 && bytes.size() <= 64u + 128u * 1024u * 1024u,
                           "native program bundle byte extent");
            reader.require(std::memcmp(bytes.data(), "SDGOPR1\0", 8) == 0, "native program bundle magic");
            reader.offset = 8;
            reader.require(reader.word() == 1 && reader.word() == 64, "native program bundle version/header");
            const auto count = reader.word(), payload = reader.word();
            reader.require(count && count <= 128 && payload == bytes.size() - 64, "native program bundle owner/payload count");
            reader.require(reader.hash() == catalogSha256, "native program bundle catalog SHA-256 differs");
            reader.require(reader.word() == 0 && reader.word() == 0, "native program bundle reserved fields");
            std::set<std::string> owners, exports, resources;
            std::set<uint32_t> weapons;
            std::size_t programs = 0, total = 0;
            std::vector<sdgo::scripts::Candidate> prepared;
            for (uint32_t i = 0; i < count; ++i) {
                sdgo::scripts::Candidate candidate;
                candidate.owner = reader.text(48); candidate.revision = reader.text(128);
                reader.require(candidate.revision.size() == 64 && std::all_of(candidate.revision.begin(), candidate.revision.end(),
                    [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }),
                    "native program revision must be a SHA-256");
                reader.require(owners.insert(candidate.owner).second, "duplicate native program owner");
                const auto programCount = reader.word();
                reader.require(programCount && programCount <= 512, "native program count budget");
                std::set<std::string> ownerExports;
                for (uint32_t j = 0; j < programCount; ++j) {
                    reader.require(++programs <= 4096, "native program batch count budget");
                    sdgo::scripts::Program program;
                    program.resource = reader.text(240); program.sha256 = reader.hash();
                    reader.require(resources.insert(program.resource).second, "duplicate native program resource");
                    const auto names = reader.word();
                    reader.require(names && names <= 100000, "native program export count budget");
                    for (uint32_t k = 0; k < names; ++k) {
                        auto name = reader.text(255);
                        reader.require(exports.insert(name).second && ownerExports.insert(name).second,
                                       "native program owners share an export");
                        program.exports.push_back(std::move(name));
                    }
                    const auto size = reader.word();
                    reader.require(size && size <= 32u * 1024u * 1024u && size <= 128u * 1024u * 1024u - total,
                                   "native program chunk byte budget");
                    total += size;
                    reader.require(size <= bytes.size() - reader.offset, "truncated native program chunk");
                    program.bytes.assign(bytes.begin() + reader.offset, bytes.begin() + reader.offset + size);
                    reader.offset += size;
                    candidate.programs.push_back(std::move(program));
                }
                const auto valid = sdgo::scripts::Validate(candidate);
                reader.require(valid.valid, valid.reason.c_str());
                const auto bindings = reader.word();
                reader.require(bindings && bindings <= 4096, "native program initializer count budget");
                for (uint32_t j = 0; j < bindings; ++j) {
                    const auto weapon = reader.word(); const auto hash = reader.hash();
                    const auto name = reader.text(49);
                    reader.require(weapons.insert(weapon).second && ownerExports.count(name),
                                   "native initializer weapon/export ownership differs");
                    const auto position = catalog.Find(weapon);
                    reader.require(position != catalog.records.size(), "native initializer weapon is not imported");
                    const auto& entry = catalog.records[position];
                    reader.require(entry.kind == kWeaponType && entry.bytes.size() == kWeaponBytes &&
                        sdgo::scripts::Sha256(entry.bytes) == hash, "native initializer weapon bytes differ");
                    const auto* field = entry.bytes.data() + 1771;
                    std::string actual; bool terminated = false;
                    for (std::size_t k = 0; k < 50; ++k) {
                        const auto value = uint32_t(field[k * 2]) | (uint32_t(field[k * 2 + 1]) << 8);
                        if (!value) { terminated = true; break; }
                        reader.require(value < 128, "native initializer export encoding");
                        actual += static_cast<char>(value);
                    }
                    reader.require(terminated && actual == name, "native weapon initializer field differs");
                }
                prepared.push_back(std::move(candidate));
            }
            reader.require(reader.offset == bytes.size(), "native program bundle trailing bytes");
            candidates.swap(prepared);
            return true;
        } catch (const std::exception& exception) { error = exception.what(); return false; }
    }

private:
    struct Reader {
        const std::vector<uint8_t>& bytes;
        std::size_t offset = 0;
        void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
        uint32_t word() {
            require(offset <= bytes.size() && bytes.size() - offset >= 4, "truncated native program integer");
            const auto value = U32(bytes.data() + offset); offset += 4; return value;
        }
        std::string hash() {
            require(offset <= bytes.size() && bytes.size() - offset >= 32, "truncated native program hash");
            static const char hex[] = "0123456789abcdef"; std::string result;
            for (std::size_t i = 0; i < 32; ++i) { const auto value = bytes[offset++]; result += hex[value >> 4]; result += hex[value & 15]; }
            return result;
        }
        std::string text(uint32_t maximum) {
            const auto size = word();
            require(size && size <= maximum && size <= bytes.size() - offset, "native program text extent");
            std::string result(reinterpret_cast<const char*>(bytes.data() + offset), size); offset += size;
            for (const unsigned char character : result)
                require(character >= 32 && character <= 126, "native program text must be printable ASCII");
            return result;
        }
    };
};
} // namespace sdgo::unit_catalog
