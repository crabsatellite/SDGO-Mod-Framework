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

inline bool ReadSealedAiTeamCounts(const std::wstring& job, unsigned count,
                                  const char* title, unsigned& allies, unsigned& enemies) {
    allies=count/2;enemies=(count+1)/2;
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
       sprintf_s(expected,"%s !ai1 a6 hard",prefix)<0 || strcmp(title,expected)) return false;
    allies=5;enemies=1;
    return true;
}
