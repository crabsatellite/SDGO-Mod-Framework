#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../mods/game_test_client/sealed_room_title.h"
int main() {
    wchar_t temporary[MAX_PATH]={},file[MAX_PATH]={};
    if(!GetTempPathW(MAX_PATH,temporary)||!GetTempFileNameW(temporary,L"sdt",0,file))return 2;
    char title[21]="gt1234 !ai e2 a1";
    bool ok=ReadSealedRoomTitle(file,title)&&!strcmp(title,"gt1234 !ai e2 a1");
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"gt1234abcd !ai e2 a1",file);
    ok=ok&&ReadSealedRoomTitle(file,title)&&!strcmp(title,"gt1234abcd !ai e2 a1");
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"gt1234abcde !ai e2 a1",file);
    ok=ok&&!ReadSealedRoomTitle(file,title);
    unsigned allies=0,enemies=0;
    ok=ok&&ReadSealedAiTeamCounts(file,3,"gt1234 !ai e2 a1",allies,enemies)&&allies==1&&enemies==2;
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_layout",L"allied",file);
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_difficulty",L"Hard",file);
    ok=ok&&WritePrivateProfileStringW(L"run",L"title_nonce",L"gt1234",file);
    ok=ok&&ReadSealedAiTeamCounts(file,6,"gt1234 !ai1 a6 hard",allies,enemies)&&allies==5&&enemies==1;
    ok=ok&&!ReadSealedAiTeamCounts(file,5,"gt1234 !ai1 a6 hard",allies,enemies);
    ok=ok&&!ReadSealedAiTeamCounts(file,6,"gt1234 !ai e3 a3",allies,enemies);
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_layout",L"Allied",file);
    ok=ok&&!ReadSealedAiTeamCounts(file,6,"gt1234 !ai1 a6 hard",allies,enemies);
    DeleteFileW(file);
    std::puts(ok?"PASS sealed native room title: fallback, exact twenty bytes, overflow rejection":"FAIL sealed native room title");
    return ok?0:1;
}
