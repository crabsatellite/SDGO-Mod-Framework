#include "../client_policy.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
using namespace sdgo::mission_behavior;

int checks = 0;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(2);
    }
}

std::array<std::uint8_t, kNpcRecordBytes> Record(
    std::uint32_t id,
    const char* ai) {
    std::array<std::uint8_t, kNpcRecordBytes> record{};
    std::memcpy(record.data(), &id, sizeof(id));
    Check(
        WriteAiName(record.data(), record.size(), ai),
        "fixture AI name fits the native field");
    return record;
}

const char* Ai(
    const std::array<std::uint8_t, kNpcRecordBytes>& record,
    std::array<char, kNpcAiNameCharacters>& output) {
    Check(
        ReadAiName(record.data(), record.size(), output),
        "native AI name reads back");
    return output.data();
}
}

int main(int argc, char** argv) {
    // Exercise the same bounded parser used by both runtime DLLs. These are
    // fabricated rows and contain no owner's reference task data.
    NpcOverride entry{};
    entry.baseline = {1,100,1,1,200,-10,20,30,0,0,0,1,1,0};
    entry.oc = entry.baseline;
    entry.oc.npc = 201;
    entry.mask = kChangeNpc;
    Header header{};
    std::memcpy(header.magic,"SDGOMBP1",8);
    header.version=kVersion;header.header_bytes=sizeof(Header);
    header.flags=kBossBindings|kNpcOverrides;
    header.entry_bytes=sizeof(NpcOverride);header.entry_count=1;
    header.payload_bytes=sizeof(NpcOverride);
    header.bindings_generation=kBindingsGeneration;
    auto parse = [&](const Header& input, const NpcOverride& row, bool checksum=true) {
        auto h=input;
        if(checksum)h.payload_crc32=Crc32(
            reinterpret_cast<const std::uint8_t*>(&row),sizeof(row));
        std::array<std::uint8_t,sizeof(Header)+sizeof(NpcOverride)> raw{};
        std::memcpy(raw.data(),&h,sizeof(h));
        std::memcpy(raw.data()+sizeof(h),&row,sizeof(row));
        Bundle bundle;std::string error;
        return ParseBundle(raw.data(),raw.size(),bundle,error);
    };
    Check(parse(header,entry),"signed native profile row parses");
    auto invalid=entry;invalid.mask|=kChangeX;
    Check(!parse(header,invalid),"redundant mask is rejected like the Python codec");
    invalid=entry;invalid.oc.part=2;
    Check(!parse(header,invalid),"immutable native row fields cannot change");
    invalid=entry;invalid.oc.after_time=65536;invalid.mask|=kChangeDelay;
    Check(!parse(header,invalid),"native uint16 delay overflow is rejected");
    Check(!parse(header,entry,false),"payload checksum is verified");
    Header empty=header;empty.entry_count=0;empty.payload_bytes=0;empty.flags=kBossBindings;
    Bundle emptyBundle;std::string emptyError;
    Check(!ParseBundle(reinterpret_cast<const std::uint8_t*>(&empty),sizeof(empty),
                       emptyBundle,emptyError),"empty OC bundle cannot activate");

    Profile parsed = Profile::Gco;
    Check(
        ParseClientProfile("oc", parsed) && parsed == Profile::Oc,
        "OC profile parses");
    Check(
        ParseClientProfile("OC", parsed) && parsed == Profile::Oc,
        "profile parsing is case insensitive");
    Check(
        ParseClientProfile("gco", parsed) && parsed == Profile::Gco,
        "GCO profile parses");
    Check(
        !ParseClientProfile("current", parsed),
        "unknown profile is refused");

    std::uint32_t previous = 0;
    std::size_t boss001 = 0;
    std::size_t boss002 = 0;
    for (const auto& binding : kM1Bindings) {
        Check(binding.npc_id > previous, "M1 identities are unique and sorted");
        previous = binding.npc_id;
        boss001 += std::strcmp(binding.oc_ai, "MM001_NpcBoss001") == 0;
        boss002 += std::strcmp(binding.oc_ai, "MM001_NpcBoss002") == 0;

        auto record = Record(binding.npc_id, kGcoM1BossAi);
        const auto applied =
            ApplyM1Binding(record.data(), record.size(), Profile::Oc);
        Check(applied.result == RecordResult::Applied, "OC changes a GCO binding");
        Check(
            std::strcmp(applied.before.data(), kGcoM1BossAi) == 0 &&
                std::strcmp(applied.after.data(), binding.oc_ai) == 0,
            "OC decision records exact before and after names");
        std::array<char, kNpcAiNameCharacters> name{};
        Check(std::strcmp(Ai(record, name), binding.oc_ai) == 0,
              "OC target is present in the native record");

        const auto idempotent =
            ApplyM1Binding(record.data(), record.size(), Profile::Oc);
        Check(idempotent.result == RecordResult::Unchanged,
              "repeated OC application is idempotent");

        const auto restored =
            ApplyM1Binding(record.data(), record.size(), Profile::Gco);
        Check(restored.result == RecordResult::Restored,
              "GCO restores the exact current binding");
        Check(std::strcmp(Ai(record, name), kGcoM1BossAi) == 0,
              "restored native record uses the GCO binding");
    }
    Check(boss001 == 10 && boss002 == 2, "OC scope is exactly ten Boss001 and two Boss002 bindings");

    std::array<std::array<std::uint8_t, kNpcRecordBytes>, kM1Bindings.size()>
        records{};
    std::array<const std::uint8_t*, kM1Bindings.size()> pointers{};
    for (std::size_t index = 0; index < records.size(); ++index) {
        records[index] = Record(kM1Bindings[index].npc_id, kGcoM1BossAi);
        pointers[index] = records[index].data();
    }
    const auto setPlan = PlanM1Set(pointers, kNpcRecordBytes, Profile::Oc);
    Check(
        setPlan.valid && setPlan.changed == 12 && setPlan.unchanged == 0,
        "whole M1 set preflights before any write");
    const auto setBefore = records;
    records[7] = Record(kM1Bindings[7].npc_id, "Foreign_Mission_AI");
    const auto refusedSet = PlanM1Set(pointers, kNpcRecordBytes, Profile::Oc);
    Check(
        !refusedSet.valid &&
            refusedSet.conflict_npc_id == kM1Bindings[7].npc_id,
        "one foreign binding refuses the whole set");
    for (std::size_t index = 0; index < 7; ++index)
        Check(
            records[index] == setBefore[index],
            "whole-set preflight does not partially write earlier records");

    for (const std::uint32_t ve : {85145u, 85146u, 85147u}) {
        auto record = Record(ve, kGcoM1BossAi);
        const auto decision =
            ApplyM1Binding(record.data(), record.size(), Profile::Oc);
        Check(decision.result == RecordResult::Ignored,
              "Very Easy M1 binding remains native");
    }

    auto unknown = Record(85054, "Foreign_Mission_AI");
    const auto before = unknown;
    const auto conflict =
        ApplyM1Binding(unknown.data(), unknown.size(), Profile::Oc);
    Check(conflict.result == RecordResult::Conflict && unknown == before,
          "foreign target data is refused without a write");

    auto shortRecord = Record(85054, kGcoM1BossAi);
    const auto shortBefore = shortRecord;
    const auto shortResult =
        ApplyM1Binding(shortRecord.data(), shortRecord.size() - 1, Profile::Oc);
    Check(shortResult.result == RecordResult::Conflict &&
              shortRecord == shortBefore,
          "unexpected native record length is refused without a write");

    auto unrelated = Record(90000, "Another_Npc_AI");
    const auto unrelatedBefore = unrelated;
    const auto ignored =
        ApplyM1Binding(unrelated.data(), unrelated.size(), Profile::Oc);
    Check(ignored.result == RecordResult::Ignored &&
              unrelated == unrelatedBefore,
          "unrelated NPC records remain byte-identical");

    if (argc == 2) {
        Bundle ownerBundle;
        std::string error;
        const bool loaded =
            LoadProfileFile(argv[1], ownerBundle, error);
        Check(
            loaded,
            error.empty() ? "real owner bundle loads"
                          : error.c_str());
        Check(
            (ownerBundle.header.flags & kBossBindings) != 0,
            "real owner bundle declares the client boss bindings");
    } else {
        Check(argc == 1, "optional argument is one owner profile.bin path");
    }

    std::printf(
        "mission behavior client policy passed: %d checks, 12 OC bindings, "
        "3 VE bindings preserved\n",
        checks);
    return 0;
}
