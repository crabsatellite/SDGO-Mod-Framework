#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sdgo::scripts {

// Private content supplies bytes and names; this contract contains no game content.
struct Program {
    std::string resource;
    std::string sha256;
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> exports;
};

struct Candidate {
    std::string owner;
    std::string revision;
    std::vector<Program> programs;
};

enum class Status {
    Registered, AlreadyRegistered, InvalidCandidate, BoundaryUnavailable,
    NameConflict, RestartRequired, FatalClientState
};

struct Export {
    std::string name;
    std::uintptr_t functionIdentity = 0;
};

struct Result {
    Status status = Status::InvalidCandidate;
    std::string reason;
    std::vector<Export> exports;
    bool executionStarted = false;
    bool clientMustClose = false;
    bool active() const { return status == Status::Registered || status == Status::AlreadyRegistered; }
};

// Supplied only by a native adapter on the original VM's initialization thread.
class Vm {
public:
    virtual ~Vm() = default;
    virtual bool atRootCompletionBoundary() const = 0;
    virtual std::uintptr_t identity() const = 0;
    virtual int stackTop() const = 0;
    virtual void restoreStack(int top) = 0;
    virtual int globalType(const std::string& name) = 0;
    virtual std::uintptr_t functionIdentity(const std::string& name) = 0;
    virtual bool execute(const Program& program, std::string& error) = 0;
    // This must close or permanently quarantine the current candidate client.
    // There is deliberately no claimed rollback of a partially executed VM.
    virtual void failClosed(const std::string& reason) noexcept = 0;
};

struct Validation {
    bool valid = false;
    std::string reason;
    std::vector<std::string> exports;
};

std::string Sha256(const std::vector<std::uint8_t>& bytes);
Validation Validate(const Candidate& candidate);

// One instance per native Lua-state lifetime. A different pointer is not silently
// adopted: the root hook must create a fresh registry for a newly initialized VM.
class Registry {
public:
    Result activate(const Candidate& candidate, Vm& vm);
    // Preflight all owners before executing any chunk. The caller retains the
    // immutable candidate array for this call. Partial execution is fatal.
    Result activateBatch(const Candidate* candidates, std::size_t count, Vm& vm);
    bool poisoned() const { return poisoned_; }
private:
    struct Registration {
        std::string revision;
        std::string contentFingerprint;
        std::vector<Export> exports;
    };
    std::map<std::string, Registration> owners_;
    std::uintptr_t vm_ = 0;
    bool poisoned_ = false;
};

} // namespace sdgo::scripts
