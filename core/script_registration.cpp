#include "sdgo/script_registration.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <iterator>
#include <set>
#include <stdexcept>

namespace sdgo::scripts {
namespace {
constexpr int kNil = 0;
constexpr int kFunction = 6;
constexpr std::size_t kMaximumBytes = 128u * 1024u * 1024u;

bool Identifier(const std::string& value, bool owner) {
    if (value.empty() || value.size() > (owner ? 48u : 255u)) return false;
    for (std::size_t i = 0; i != value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        const bool letter = (c >= 'a' && c <= 'z') || (!owner && c >= 'A' && c <= 'Z');
        if (!letter && c != '_' && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

// Lua 5.0's actual 32-bit little-endian dump, not LuaJIT or a textual script.
// The root is restricted to publishing closures. Function bodies remain opaque
// original programs and execute later through the game's ordinary consumers.
struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t position = 0, prototypes = 0, items = 0;
    void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
    void skip(std::size_t n) { require(n <= bytes.size() - position, "truncated Lua chunk"); position += n; }
    std::uint8_t byte() { require(position < bytes.size(), "truncated Lua byte"); return bytes[position++]; }
    std::uint32_t word() {
        require(bytes.size() - position >= 4, "truncated Lua integer");
        const auto* p = bytes.data() + position; position += 4;
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    }
    std::uint32_t count() { auto n = word(); require(n <= 1000000 && items <= 5000000 - n, "Lua vector budget"); items += n; return n; }
    std::string string(bool constant = false) {
        auto n = word(); if (!n) { require(!constant, "null Lua string constant"); return {}; }
        require(n <= bytes.size() - position && bytes[position + n - 1] == 0, "Lua string extent/terminator");
        std::string result(reinterpret_cast<const char*>(bytes.data() + position), n - 1); position += n; return result;
    }
    struct Prototype {
        unsigned upvalues = 0, parameters = 0, vararg = 0, stack = 0;
        std::vector<std::string> constants;
        std::vector<bool> stringConstants;
        std::vector<Prototype> children;
        std::vector<std::uint32_t> code;
    };
    Prototype prototype(unsigned depth) {
        require(depth <= 128 && ++prototypes <= 100000, "Lua prototype budget");
        Prototype p; string(); word(); p.upvalues = byte(); p.parameters = byte(); p.vararg = byte(); p.stack = byte();
        require(p.stack > 0 && p.parameters <= p.stack, "Lua register frame");
        auto n = count(); skip(std::size_t(n) * 4);
        n = count(); for (std::uint32_t i = 0; i < n; ++i) { string(); word(); word(); }
        n = count(); for (std::uint32_t i = 0; i < n; ++i) string();
        n = count();
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto tag = byte(); std::string value;
            if (tag == 3) skip(8);
            else if (tag == 4) value = string(true);
            else require(tag == 0, "unsupported Lua constant tag");
            p.constants.push_back(std::move(value)); p.stringConstants.push_back(tag == 4);
        }
        n = count(); for (std::uint32_t i = 0; i < n; ++i) p.children.push_back(prototype(depth + 1));
        n = count(); for (std::uint32_t i = 0; i < n; ++i) { auto instruction = word(); require((instruction & 63) <= 34, "unknown Lua opcode"); p.code.push_back(instruction); }
        return p;
    }
    std::vector<std::string> closureExports() {
        const std::uint8_t header[] = {0x1b,'L','u','a',0x50,1,4,4,4,6,8,9,9,8};
        require(bytes.size() >= sizeof(header) + 8 && std::equal(std::begin(header), std::end(header), bytes.begin()), "Lua 5.0 x86 header required");
        position = sizeof(header); double number = 0; std::memcpy(&number, bytes.data() + position, 8); skip(8);
        require(number == 3.14159265358979323846e7, "Lua numeric representation");
        const auto p = prototype(0); require(position == bytes.size(), "Lua trailing bytes");
        require(p.upvalues == 0 && p.parameters == 0, "registration root parameters/upvalues");
        std::set<std::string> names; std::set<unsigned> children;
        std::size_t pc = 0;
        while (pc + 1 < p.code.size()) {
            auto closure = p.code[pc]; if ((closure & 63) == 27) break;
            auto publication = p.code[pc + 1];
            require((closure & 63) == 34 && (publication & 63) == 7, "registration root may only publish closures");
            const auto child = (closure >> 6) & 0x3ffff, key = (publication >> 6) & 0x3ffff;
            require((closure >> 24) < p.stack && (closure >> 24) == (publication >> 24), "registration closure register");
            require(child < p.children.size() && p.children[child].upvalues == 0 && children.insert(child).second, "registration captured/duplicate closure");
            require(key < p.constants.size() && p.stringConstants[key] && names.insert(p.constants[key]).second, "registration global publication");
            pc += 2;
        }
        require(pc + 1 == p.code.size() && (p.code[pc] & 63) == 27 && ((p.code[pc] >> 15) & 511) == 1 && ((p.code[pc] >> 6) & 511) == 0 && (p.code[pc] >> 24) < p.stack, "registration root must return no values from a valid register");
        require(!names.empty() && children.size() == p.children.size(), "registration must publish every root closure");
        return {names.begin(), names.end()};
    }
};

Result Failure(Status status, const std::string& reason) { Result r; r.status = status; r.reason = reason; return r; }

std::string Fingerprint(const Candidate& candidate) {
    std::vector<std::uint8_t> bytes;
    for (const auto& p : candidate.programs) {
        bytes.insert(bytes.end(), p.resource.begin(), p.resource.end()); bytes.push_back(0);
        bytes.insert(bytes.end(), p.sha256.begin(), p.sha256.end()); bytes.push_back(0);
    }
    return Sha256(bytes);
}
}

std::string Sha256(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> object; std::uint8_t digest[32] = {}; DWORD length = 0, received = 0;
    auto close = [&] { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (bytes.size() > std::numeric_limits<ULONG>::max() || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &received, 0) < 0) { close(); throw std::runtime_error("SHA-256 provider unavailable"); }
    object.resize(length);
    if (BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) < 0
        || (!bytes.empty() && BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0)
        || BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) { close(); throw std::runtime_error("SHA-256 computation failed"); }
    close(); static const char digits[] = "0123456789abcdef"; std::string result;
    for (auto v : digest) { result += digits[v >> 4]; result += digits[v & 15]; } return result;
}

Validation Validate(const Candidate& candidate) {
    Validation result;
    try {
        if (!Identifier(candidate.owner, true) || candidate.revision.empty() || candidate.revision.size() > 128 || candidate.programs.empty() || candidate.programs.size() > 512) throw std::runtime_error("candidate identity/count");
        std::set<std::string> names, resources; std::size_t total = 0;
        for (const auto& p : candidate.programs) {
            const auto prefix = "scp/" + candidate.owner + "_";
            if (p.resource.compare(0, prefix.size(), prefix) != 0 || p.resource.size() > 240 || p.resource.size() < 5 || p.resource.substr(p.resource.size() - 4) != ".scp" || !resources.insert(p.resource).second) throw std::runtime_error("candidate resource namespace");
            for (std::size_t i = 4; i < p.resource.size(); ++i) { const char c = p.resource[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) throw std::runtime_error("candidate resource path"); }
            if (p.resource.find("..") != std::string::npos || p.bytes.empty() || p.bytes.size() > 32u * 1024u * 1024u || total > kMaximumBytes - p.bytes.size()) throw std::runtime_error("candidate byte budget/path");
            total += p.bytes.size(); if (Sha256(p.bytes) != p.sha256) throw std::runtime_error("candidate chunk hash");
            auto actual = Reader{p.bytes}.closureExports(); auto declared = p.exports; std::sort(declared.begin(), declared.end());
            if (actual != declared) throw std::runtime_error("candidate declared exports do not match chunk");
            for (const auto& name : actual) {
                if (!Identifier(name, false) || name.compare(0, candidate.owner.size() + 1, candidate.owner + "_") != 0 || !names.insert(name).second) throw std::runtime_error("candidate export namespace/collision");
            }
        }
        result.valid = true; result.exports.assign(names.begin(), names.end());
    } catch (const std::exception& e) { result.reason = e.what(); }
    return result;
}

Result Registry::activate(const Candidate& candidate, Vm& vm) {
    if (poisoned_) { auto r = Failure(Status::FatalClientState, "candidate client is already quarantined"); r.clientMustClose = true; return r; }
    const auto validation = Validate(candidate); if (!validation.valid) return Failure(Status::InvalidCandidate, validation.reason);
    if (!vm.atRootCompletionBoundary() || !vm.identity()) return Failure(Status::BoundaryUnavailable, "original root completion / initialization thread / same VM gate missing");
    if (vm_ && vm_ != vm.identity()) return Failure(Status::RestartRequired, "registry belongs to a different native VM lifetime");
    const auto fingerprint = Fingerprint(candidate); auto existing = owners_.find(candidate.owner);
    if (existing != owners_.end()) {
        if (existing->second.revision != candidate.revision || existing->second.contentFingerprint != fingerprint) return Failure(Status::RestartRequired, "registered owner revision cannot be replaced in a live VM");
        for (const auto& e : existing->second.exports) if (vm.globalType(e.name) != kFunction || vm.functionIdentity(e.name) != e.functionIdentity) {
            poisoned_ = true; auto r = Failure(Status::FatalClientState, "previously registered function identity has changed"); r.clientMustClose = true; vm.failClosed(r.reason); return r;
        }
        Result r; r.status = Status::AlreadyRegistered; r.exports = existing->second.exports; return r;
    }
    for (const auto& name : validation.exports) if (vm.globalType(name) != kNil) return Failure(Status::NameConflict, "export name already exists: " + name);
    const int top = vm.stackTop(); Result result; result.executionStarted = true;
    auto fatal = [&](const std::string& why) {
        poisoned_ = true; result.status = Status::FatalClientState; result.reason = why; result.clientMustClose = true;
        try { vm.restoreStack(top); } catch (...) { result.reason += "; VM stack restoration failed"; }
        vm.failClosed(result.reason); return result;
    };
    try {
        for (const auto& p : candidate.programs) { std::string error; if (!vm.execute(p, error)) return fatal("native chunk load/execute failed: " + p.resource + ": " + error); vm.restoreStack(top); }
        for (const auto& name : validation.exports) {
            const auto identity = vm.functionIdentity(name);
            if (vm.globalType(name) != kFunction || !identity) return fatal("native function readback failed: " + name);
            result.exports.push_back({name, identity});
        }
        vm.restoreStack(top); result.status = Status::Registered; vm_ = vm.identity(); owners_[candidate.owner] = {candidate.revision, fingerprint, result.exports}; return result;
    } catch (const std::exception& e) { return fatal(std::string("native registration exception: ") + e.what()); }
}
} // namespace sdgo::scripts
