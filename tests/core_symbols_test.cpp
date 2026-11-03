#include <windows.h>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "symbols.h"

namespace {
int checks=0,failures=0;
void Check(bool ok,const char* message) {
    ++checks;
    if(!ok) {++failures;std::printf("FAIL %s\n",message);}
}
__declspec(noinline) void Marker() {volatile int value=1;(void)value;}
std::string Key(size_t index) {char text[48];sprintf_s(text,"symbol_%06zu",index);return text;}
struct Fixture {
    wchar_t file[MAX_PATH]{};
    uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Fixture() {
        wchar_t temporary[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH,temporary)!=0&&GetTempFileNameW(temporary,L"ssi",0,file)!=0,"private symbol fixture created");
    }
    ~Fixture() {DeleteFileW(file);}
    void Write(size_t count,bool validHost=true) {
        FILE* stream=nullptr;_wfopen_s(&stream,file,L"wb");
        Check(stream!=nullptr,"symbol fixture opens");if(!stream)return;
        const auto nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        std::fprintf(stream,"[host]\ntimestamp=0x%08lX\nsize_of_image=0x%08lX\n",
                     validHost?nt->FileHeader.TimeDateStamp:nt->FileHeader.TimeDateStamp+1,
                     nt->OptionalHeader.SizeOfImage);
        const auto rva=reinterpret_cast<uintptr_t>(&Marker)-base;
        // Deliberately unsorted input; Find must keep the original Symbol objects.
        for(size_t i=count;i>0;--i)
            std::fprintf(stream,"[%s]\nkind=data\nrva=0x%08lX\n",Key(i-1).c_str(),static_cast<unsigned long>(rva));
        if(count) {
            auto code=reinterpret_cast<const unsigned char*>(&Marker);
            std::fprintf(stream,"[verified.code]\nkind=code\nrva=0x%08lX\nexpect=",static_cast<unsigned long>(rva));
            for(int i=0;i<8;++i)std::fprintf(stream,"%02X ",code[i]);
            std::fprintf(stream,"\n[rejected.code]\nkind=code\nrva=0x%08lX\nexpect=%02X %02X %02X %02X %02X\n",
                         static_cast<unsigned long>(rva),code[0]^0xff,code[1],code[2],code[3],code[4]);
            std::fprintf(stream,"[long.symbol.name.exceeding.small.string.storage.for.lookup]\nkind=data\nrva=0x%08lX\n",
                         static_cast<unsigned long>(rva));
            // Windows preserves section enumeration order and returns the first
            // section's values. The index must never prefer a later duplicate.
            std::fprintf(stream,"[duplicate]\nkind=code\nrva=0x%08lX\nexpect=00\n",
                         static_cast<unsigned long>(rva));
            std::fprintf(stream,"[duplicate]\nkind=data\nrva=0x%08lX\n",
                         static_cast<unsigned long>(rva));
        }
        std::fclose(stream);
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,file);
    }
};
}

int main(int argc,char**) {
    static_assert(sizeof(void*)==4,"core symbol fixture must use the native Win32 ABI");
    Fixture fixture;
    sdgo::SymbolTable table;
    Check(table.Find("missing")==nullptr,"empty index misses safely");
    for(size_t count:{size_t(1),size_t(64),size_t(1024),size_t(2048)}) {
        fixture.Write(count);
        Check(table.Load(fixture.file,GetModuleHandleW(nullptr),"fixture"),"matching host loads");
        Check(table.Count()>=count+4,"all source sections remain represented");
        std::vector<const sdgo::Symbol*> found;
        for(size_t i=0;i<count;++i) {
            const auto* value=table.Find(Key(i).c_str());
            Check(value&&value->verified&&value->name==Key(i)&&value->address==reinterpret_cast<uintptr_t>(&Marker),"unsorted section resolves to its verified native address");
            found.push_back(value);
        }
        for(size_t i=0;i<count;++i)Check(table.Find(Key(i).c_str())==found[i],"repeated lookups retain Symbol pointer identity");
        Check(!table.Find("Symbol_000000")&&!table.Find("symbol_999999")&&!table.Find(""),"case and missing-name semantics remain exact");
        const auto* good=table.Find("verified.code");
        const auto* bad=table.Find("rejected.code");
        const auto* duplicate=table.Find("duplicate");
        Check(good&&good->verified,"expected code bytes still validate");
        Check(bad&&!bad->verified&&!bad->problem.empty(),"indexed rejected signature never becomes verified");
        Check(duplicate&&!duplicate->verified&&duplicate->kind=="code","duplicate keeps first source entry even when rejected");
        Check(table.Find("long.symbol.name.exceeding.small.string.storage.for.lookup")!=nullptr,"long names survive vector reallocation");

        sdgo::SymbolTable copied=table;
        Check(copied.Find("symbol_000000")&&copied.Find("symbol_000000")!=found[0],"copy owns its Symbol storage and valid numeric index");
        sdgo::SymbolTable assigned;assigned=table;
        Check(assigned.Find("symbol_000000")&&assigned.Find("symbol_000000")!=found[0],"copy assignment retains lookup semantics");
        sdgo::SymbolTable moved=std::move(copied);
        Check(moved.Find("symbol_000000")!=nullptr,"moved symbol table retains its index");
        sdgo::SymbolTable moveAssigned;moveAssigned=std::move(assigned);
        Check(moveAssigned.Find("symbol_000000")!=nullptr,"move assignment retains its index");

        if(argc>1) {
            LARGE_INTEGER frequency{},start{},finish{};QueryPerformanceFrequency(&frequency);
            std::vector<std::string> names;names.reserve(count);
            for(size_t i=0;i<count;++i)names.push_back(Key(i));
            volatile uintptr_t checksum=0;
            constexpr size_t repetitions=100000;
            QueryPerformanceCounter(&start);
            for(size_t i=0;i<repetitions;++i) {
                const auto* value=table.Find(names[(i*137)%count].c_str());
                checksum+=value?value->address-fixture.base:0;
                checksum+=table.Find("symbol_missing")?1:0;
            }
            QueryPerformanceCounter(&finish);
            std::printf("BENCH symbols=%zu ns_per_lookup=%.3f checksum_nonzero=%u\n",count,
                        double(finish.QuadPart-start.QuadPart)*1e9/double(frequency.QuadPart)/(repetitions*2),checksum!=0);
        }
    }
    Fixture empty;empty.Write(0);
    Check(table.Load(empty.file,GetModuleHandleW(nullptr),"fixture")&&table.Count()==0&&!table.Find("symbol_000000"),"empty reload invalidates all old keys");
    fixture.Write(64);Check(table.Load(fixture.file,GetModuleHandleW(nullptr),"fixture"),"table reloads after empty input");
    Fixture mismatch;mismatch.Write(1,false);
    Check(!table.Load(mismatch.file,GetModuleHandleW(nullptr),"wrong")&&table.Count()==0&&!table.Find("symbol_000000"),"host mismatch clears the previous index");
    Check(table.Load(fixture.file,GetModuleHandleW(nullptr),"fixture"),"table reloads after rejected host");
    Check(!table.Load(std::wstring(fixture.file)+L".missing",GetModuleHandleW(nullptr),"missing")&&table.Count()==0&&!table.Find("symbol_000000"),"missing file clears the previous index");
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
