#pragma once
#include "sdgo/native_unit_form_bank.h"
namespace sdgo::unit_forms {
enum class IdentityCommit { refused, committed, restored, unrecoverable };
// Caller owns the original native room lock and independently verified actor,
// UID, generation, registry, directed edge, unit semantics and fresh-bank
// equivalence. Only the prepared inactive ID/definition DWORDs may differ.
// Does not select a bank/weapon or send notifications. Failure never deliberately
// overwrites unrelated actor state. An unrecoverable memory fault is terminal
// for a test caller; never continue a native transform with a partial header.
IdentityCommit CommitEquivalentIdentity(void* live,const Slot& before,const Slot& staged) noexcept;
// Replace the complete inactive bank (0x2f0 bytes) and its stat slice (0xc bytes)
// with the staged image prepared by StageInactive. Same caller duties as above;
// additionally the retired inactive form must have no live use the caller still
// needs (its ammo/cooldown state is discarded). Every other slot byte must match.
IdentityCommit CommitInactiveBank(void* live,const Slot& before,const Slot& staged) noexcept;
} // namespace sdgo::unit_forms
