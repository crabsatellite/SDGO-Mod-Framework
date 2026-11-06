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
} // namespace sdgo::unit_forms
