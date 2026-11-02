#pragma once
#include <windows.h>
#include <string>

inline bool ReadSealedRoomTitle(const std::wstring& job, char (&title)[21]) {
    wchar_t declared[64] = {};
    const DWORD length=GetPrivateProfileStringW(L"run",L"room_title",L"",declared,64,job.c_str());
    if (!length) return true; // Older jobs retain their generated default.
    return length < 63 && WideCharToMultiByte(CP_UTF8,0,declared,-1,title,21,nullptr,nullptr)!=0;
}
