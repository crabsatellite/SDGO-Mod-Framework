#include "../mods/friend_unit_gift_server/gift_policy.h"
#include "../mods/friend_unit_gift_server/sql_policy.h"
#include "../mods/friend_unit_gift_server/unit_payload.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

using namespace sdgo::friend_unit_gift;

namespace {

bool Contains(const std::wstring& value, std::wstring_view needle) {
    return value.find(needle) != std::wstring::npos;
}

} // namespace

int main() {
    static_assert(kNativeUnitColumns.size() == 50);
    static_assert(kManagerDefaults.enabled == false);
    static_assert(kManagerDefaults.minimum_battles == 20);
    static_assert(kManagerDefaults.minimum_player_kills == 1);
    static_assert(kManagerDefaults.required_tiers == 12);
    static_assert(kManagerDefaults.minimum_distinct_per_tier == 2);

    assert(ValidPolicy(kManagerDefaults));
    auto enabled = kManagerDefaults;
    enabled.enabled = true;
    assert(CanCreateOffer(enabled));
    assert(!CanCreateOffer(kManagerDefaults));
    assert(!MakeInstallSql(L"Fixture", L"Fixture", kManagerDefaults));
    assert(!MakeSetPolicySql(enabled, false));
    assert(ResolutionAvailable(true, Operation::accept));
    assert(ResolutionAvailable(true, Operation::return_to_sender));
    assert(ResolutionAvailable(true, Operation::claim_returned));
    assert(!ResolutionAvailable(true, Operation::offer));
    assert(!ResolutionAvailable(false, Operation::accept));

    assert(ValidNote(std::wstring(200, L'x')));
    assert(!ValidNote(std::wstring(201, L'x')));
    assert(!ValidNote(std::wstring_view(L"x\0y", 3)));
    assert(NativeCapacity(0) == 24);
    assert(NativeCapacity(1176) == 1200);
    assert(NativeCapacity(1177) == 0);

    UnitInvariantSnapshot unit{};
    unit.owner_account = 100;
    unit.user_unit_number = 9001;
    assert(EligibleUnit(unit, 100));
    unit.lock_state = 1;
    assert(!EligibleUnit(unit, 100));
    unit.lock_state = 0;
    unit.referenced_by_ship_or_leader = true;
    assert(!EligibleUnit(unit, 100));

    const auto modules = Modules(L"Fixture");
    assert(modules.size() == 11);
    for (const auto& module : modules) {
        assert(Contains(module.definition, L"CREATE PROCEDURE"));
        assert(Contains(module.definition, L"\nEND;\n"));
        assert(!Contains(module.definition, L"tbl_UserUnits(("));
    }
    const auto offer = std::find_if(
        modules.begin(), modules.end(),
        [](const SqlModule& module) { return module.name == L"Offer"; });
    assert(offer != modules.end());
    assert(Contains(offer->definition, L"DECLARE @Captured TABLE"));
    assert(Contains(offer->definition, L"OUTPUT DELETED.[UserUnitNumber]"));
    assert(Contains(offer->definition, L"INTO @Captured("));
    assert(!Contains(offer->definition,
                     L"INTO [sdgo_friend_unit_gift].[EscrowUnit]"));
    assert(Contains(offer->definition, L"@Note nvarchar(max)"));
    assert(Contains(offer->definition, L"DATALENGTH(@Note)>400"));
    assert(Contains(offer->definition, L"UNICODE(SUBSTRING(@Note"));
    assert(Contains(offer->definition,
                    L"DATALENGTH(@ReceiptNote)=DATALENGTH(@Note)"));
    assert(Contains(offer->definition,
                    L"@ReceiptResultGift=GiftId"));
    assert(Contains(offer->definition,
                    L"@GiftId=@ReceiptResultGift"));
    assert(Contains(offer->definition, L"(@@OPTIONS & 2)<>0"));
    assert(Contains(offer->definition, L"@ActorAccount IS NULL"));
    assert(Contains(offer->definition, L"@UserUnitNumber IS NULL"));
    assert(Contains(offer->definition, L"@Enabled=0"));

    const auto install = MakeInstallSql(L"Fixture", L"Fixture", enabled);
    assert(install);
    assert(Contains(*install, L"DB_ID(N'Fixture')"));
    assert(Contains(*install, L"[sdgo_friend_unit_gift].[ModuleState]"));
    assert(Contains(*install, L"OBJECT_DEFINITION"));
    assert(Contains(*install, L"Latin1_General_100_BIN2"));
    assert(!Contains(*install, L"HASHBYTES"));
    assert(!Contains(*install, L"ALTER PROCEDURE"));
    assert(Contains(*install, L"ArgRecipientAccount"));
    assert(Contains(*install, L"QueryReturned"));
    assert(Contains(*install, L"PreviewOffer"));
    assert(Contains(*install, L"QueryGift"));
    assert(Contains(*install, L"QueryReceipt"));
    assert(DefinitionHash(L"Fixture").size() == 16);
    assert(DefinitionHash(L"Fixture") == DefinitionHash(L"Fixture"));
    assert(DefinitionHash(L"Fixture") != DefinitionHash(L"Other"));
    assert(static_cast<int>(Result::request_conflict) == 12);

    const auto disable = MakeSetPolicySql(kManagerDefaults, true);
    assert(disable && Contains(*disable, L"@Enabled=0"));

    std::cout << "friend_unit_gift_policy_test: PASS\n";
    return 0;
}
