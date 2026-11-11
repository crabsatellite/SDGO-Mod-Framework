#pragma once
#include <windows.h>
#include <string>
#include <cstring>
#include <cstdio>

inline bool ReadSealedRoomTitle(const std::wstring& job, char (&title)[21]) {
    wchar_t declared[64] = {};
    const DWORD length=GetPrivateProfileStringW(L"run",L"room_title",L"",declared,64,job.c_str());
    if (!length) return true; // Older jobs retain their generated default.
    return length < 63 && WideCharToMultiByte(CP_UTF8,0,declared,-1,title,21,nullptr,nullptr)!=0;
}

inline bool ReadPvpRoomTitle(const std::wstring& job, const char* nonce, bool random,
                             unsigned enemies, unsigned allies, char (&title)[21]) {
    const size_t length=std::strlen(nonce);
    if(random&&((length!=4&&length!=5)||std::strspn(nonce,"0123456789abcdef")!=length))return false;
    // A reused server session has a unique five-digit nonce and a compact
    // sealed title. Read that title before constructing any legacy default.
    // The unused e/a default would exceed the native limit for that nonce.
    title[0]=0;
    if(!ReadSealedRoomTitle(job,title))return false;
    if(title[0])return !random||(!std::strncmp(title,nonce,length)&&title[length]==' ');
    if(random&&length!=4)return false;
    return sprintf_s(title,random?"%s !ai e%u a%u !rand":"%s !ai e%u a%u",nonce,enemies,allies)>=0;
}

inline bool ReadMissionRoomTitle(const std::wstring& job, const char* nonce,
                                 bool infinite, bool plainMission, char (&title)[21]) {
    if(!nonce || std::strlen(nonce)!=6 || std::strncmp(nonce,"gt",2) ||
       std::strspn(nonce+2,"0123456789abcdef")!=4)return false;
    wchar_t difficulty[16]{};
    GetPrivateProfileStringW(L"run",L"mission_ai_difficulty",L"",difficulty,16,job.c_str());
    const char* command="!ai3 lv2";
    if(*difficulty) {
        if(!plainMission)return false;
        if(!wcscmp(difficulty,L"Easy"))command="!ai easy";
        else if(!wcscmp(difficulty,L"Medium"))command="!ai medium";
        else if(!wcscmp(difficulty,L"Hard"))command="!ai hard";
        else return false;
    }
    const char* suffix=infinite?" inf":"";
    if(std::strlen(nonce)+1+std::strlen(command)+std::strlen(suffix)>20 ||
       sprintf_s(title,"%s %s%s",nonce,command,suffix)<0)return false;
    char declared[21]{};
    return ReadSealedRoomTitle(job,declared) && declared[0] && !std::strcmp(title,declared);
}

inline bool ReadSealedAiTeamCounts(const std::wstring& job, unsigned count,
                                  const char* title, unsigned& allies, unsigned& enemies) {
    allies=GetPrivateProfileIntW(L"run",L"ai_allies",count/2,job.c_str());
    if(allies>=count)return false;
    enemies=count-allies;
    wchar_t layout[32]{};
    if(!GetPrivateProfileStringW(L"run",L"ai_layout",L"",layout,32,job.c_str())) return true;
    if(wcscmp(layout,L"allied") || count!=6) return false;
    wchar_t difficulty[32]{},nonce[16]{},pool[16]{};
    GetPrivateProfileStringW(L"run",L"ai_difficulty",L"",difficulty,32,job.c_str());
    GetPrivateProfileStringW(L"run",L"title_nonce",L"",nonce,16,job.c_str());
    if(wcscmp(difficulty,L"Hard") || wcslen(nonce)!=6 || wcsncmp(nonce,L"gt",2) ||
       wcsspn(nonce+2,L"0123456789abcdef")!=4 ||
       GetPrivateProfileStringW(L"run",L"ai_pool_level",L"",pool,16,job.c_str())) return false;
    char prefix[16]{},expected[32]{};
    if(!WideCharToMultiByte(CP_UTF8,0,nonce,-1,prefix,16,nullptr,nullptr) ||
       sprintf_s(expected,"%s !ai1 a5 hard",prefix)<0 || strcmp(title,expected)) return false;
    allies=5;enemies=1;
    return true;
}
