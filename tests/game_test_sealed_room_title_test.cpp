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
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"abcd0 !ai hard !rand",file);
    ok=ok&&ReadPvpRoomTitle(file,"abcd0",true,4,3,title)&&!strcmp(title,"abcd0 !ai hard !rand");
    ok=ok&&!ReadPvpRoomTitle(file,"abcd1",true,4,3,title);
    ok=ok&&!ReadPvpRoomTitle(file,"abcde0",true,4,3,title);
    ok=ok&&!ReadPvpRoomTitle(file,"abcdx",true,4,3,title);
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"abcde0 !ai hard !rand",file);
    ok=ok&&!ReadPvpRoomTitle(file,"abcd0",true,4,3,title);
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",nullptr,file);
    ok=ok&&!ReadPvpRoomTitle(file,"abcd0",true,4,3,title);
    ok=ok&&ReadPvpRoomTitle(file,"abcd",true,4,3,title)&&!strcmp(title,"abcd !ai e4 a3 !rand");
    ok=ok&&ReadPvpRoomTitle(file,"gt1234",false,2,1,title)&&!strcmp(title,"gt1234 !ai e2 a1");
    unsigned allies=0,enemies=0;
    ok=ok&&ReadSealedAiTeamCounts(file,3,"gt1234 !ai e2 a1",allies,enemies)&&allies==1&&enemies==2;
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_layout",L"allied",file);
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_difficulty",L"Hard",file);
    ok=ok&&WritePrivateProfileStringW(L"run",L"title_nonce",L"gt1234",file);
    ok=ok&&ReadSealedAiTeamCounts(file,6,"gt1234 !ai1 a5 hard",allies,enemies)&&allies==5&&enemies==1;
    ok=ok&&!ReadSealedAiTeamCounts(file,5,"gt1234 !ai1 a5 hard",allies,enemies);
    ok=ok&&!ReadSealedAiTeamCounts(file,6,"gt1234 !ai e3 a3",allies,enemies);
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_layout",L"Allied",file);
    ok=ok&&!ReadSealedAiTeamCounts(file,6,"gt1234 !ai1 a5 hard",allies,enemies);
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_layout",nullptr,file);
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_allies",L"0",file);
    ok=ok&&ReadSealedAiTeamCounts(file,7,"gt1234 !ai coop",allies,enemies)&&allies==0&&enemies==7;
    ok=ok&&WritePrivateProfileStringW(L"run",L"ai_allies",L"7",file);
    ok=ok&&!ReadSealedAiTeamCounts(file,7,"gt1234 !ai coop",allies,enemies);
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"gt1234 !ai3 lv2",file);
    ok=ok&&ReadMissionRoomTitle(file,"gt1234",false,true,title)&&!strcmp(title,"gt1234 !ai3 lv2");
    ok=ok&&WritePrivateProfileStringW(L"run",L"mission_ai_difficulty",L"Hard",file);
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"gt1234 !ai hard",file);
    ok=ok&&ReadMissionRoomTitle(file,"gt1234",false,true,title)&&!strcmp(title,"gt1234 !ai hard");
    ok=ok&&!ReadMissionRoomTitle(file,"gt1234",false,false,title);
    ok=ok&&!ReadMissionRoomTitle(file,"gt5678",false,true,title);
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"gt1234 !ai hard inf",file);
    ok=ok&&ReadMissionRoomTitle(file,"gt1234",true,true,title);
    ok=ok&&WritePrivateProfileStringW(L"run",L"mission_ai_difficulty",L"Medium",file);
    ok=ok&&WritePrivateProfileStringW(L"run",L"room_title",L"gt1234 !ai medium",file);
    ok=ok&&ReadMissionRoomTitle(file,"gt1234",false,true,title);
    ok=ok&&!ReadMissionRoomTitle(file,"gt1234",true,true,title);
    ok=ok&&WritePrivateProfileStringW(L"run",L"mission_ai_difficulty",L"Easy",file);
    ok=ok&&!ReadMissionRoomTitle(file,"gt1234",false,true,title);
    ok=ok&&WritePrivateProfileStringW(L"run",L"mission_ai_difficulty",L"Bogus",file);
    ok=ok&&!ReadMissionRoomTitle(file,"gt1234",false,true,title);
    DeleteFileW(file);
    std::puts(ok?"PASS sealed native room title: fallback, exact twenty bytes, overflow rejection":"FAIL sealed native room title");
    return ok?0:1;
}
