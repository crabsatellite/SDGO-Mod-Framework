#pragma once
#define DBINITCONSTANTS
#include <windows.h>
#include <oledb.h>
#include <oledberr.h>
#include <transact.h>
#include <bcrypt.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>
#include "sql_policy.h"

namespace sdgo::friend_fix {
template<class T> class Com {
    T* p_=nullptr;
public:
    Com()=default;
    Com(const Com&)=delete;
    Com& operator=(const Com&)=delete;
    ~Com(){if(p_)p_->Release();}
    T* get()const{return p_;} T* operator->()const{return p_;}
    T** put(){if(p_){p_->Release();p_=nullptr;}return &p_;}
};
inline std::wstring Hash(const std::wstring& value) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    std::array<UCHAR,32> bytes={};DWORD objectSize=0,got=0;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash provider unavailable");
    const auto close=[&](){if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);};
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&got,0)<0){close();throw std::runtime_error("hash object unavailable");}
    std::vector<UCHAR> object(objectSize);
    if(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)<0||
       BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(value.data())),static_cast<ULONG>(value.size()*sizeof(wchar_t)),0)<0||
       BCryptFinishHash(hash,bytes.data(),static_cast<ULONG>(bytes.size()),0)<0){close();throw std::runtime_error("hash failed");}
    close();std::wstring out;for(UCHAR b:bytes){out+=L"0123456789abcdef"[b>>4];out+=L"0123456789abcdef"[b&15];}return out;
}
inline bool Borrow(void* connection,ICommandText** out) {
    __try {
        if(!connection||*reinterpret_cast<int*>(static_cast<BYTE*>(connection)+0x240))return false;
        *out=*reinterpret_cast<ICommandText**>(static_cast<BYTE*>(connection)+8);
        if(!*out)return false;(*out)->AddRef();return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
class Session {
    Com<IDBCreateCommand> factory_;
    Com<ITransactionLocal> transaction_;
    bool active_=false;
    bool Create(const std::wstring& sql,Com<ICommandText>& command) {
        if (!factory_.get() || FAILED(factory_->CreateCommand(nullptr,__uuidof(ICommandText),
                reinterpret_cast<IUnknown**>(command.put()))) || !command.get()) return false;
        Com<ICommandProperties> properties;
        if (FAILED(command->QueryInterface(__uuidof(ICommandProperties), reinterpret_cast<void**>(properties.put())))) return false;
        DBPROP property{};
        property.dwPropertyID=DBPROP_COMMANDTIMEOUT; property.dwOptions=DBPROPOPTIONS_REQUIRED;
        property.colid=DB_NULLID; property.vValue.vt=VT_I4; property.vValue.lVal=5;
        DBPROPSET set{&property,1,DBPROPSET_ROWSET};
        return SUCCEEDED(properties->SetProperties(1,&set)) && property.dwStatus==DBPROPSTATUS_OK &&
            SUCCEEDED(command->SetCommandText(DBGUID_DBSQL,sql.c_str()));
    }
public:
    ~Session(){Abort();}
    void Abort(){if(active_){transaction_->Abort(nullptr,FALSE,FALSE);active_=false;}}
    bool Open(void* connection) {
        Com<ICommandText> native;
        return Borrow(connection,native.put())&&SUCCEEDED(native->GetDBSession(__uuidof(IDBCreateCommand),reinterpret_cast<IUnknown**>(factory_.put())))&&
            SUCCEEDED(factory_->QueryInterface(__uuidof(ITransactionLocal),reinterpret_cast<void**>(transaction_.put())));
    }
    bool Begin() {
        XACTTRANSINFO current={};if(SUCCEEDED(transaction_->GetTransactionInfo(&current)))return false;
        ULONG level=0;if(FAILED(transaction_->StartTransaction(ISOLATIONLEVEL_READCOMMITTED,0,nullptr,&level)))return false;
        active_=true;return level==1;
    }
    bool Commit(){if(!active_||FAILED(transaction_->Commit(FALSE,0,0)))return false;active_=false;return true;}
    bool Exec(const std::wstring& sql) {
        Com<ICommandText> command;if(!Create(sql,command))return false;
        Com<IMultipleResults> results; Com<IRowset> row; DBROWCOUNT affected=0;
        if (FAILED(command->Execute(nullptr,__uuidof(IMultipleResults),nullptr,&affected,
                reinterpret_cast<IUnknown**>(results.put()))) || !results.get()) return false;
        for (;;) {
            const auto hr=results->GetResult(nullptr,DBRESULTFLAG_DEFAULT,__uuidof(IRowset),
                &affected,reinterpret_cast<IUnknown**>(row.put()));
            if (hr==DB_S_NORESULT) return true;
            if (FAILED(hr)) return false;
        }
    }
    bool String(const std::wstring& sql,std::wstring& value) {
        Com<ICommandText> command;if(!Create(sql,command))return false;
        Com<IRowset> rows;Com<IMultipleResults> results;DBROWCOUNT affected=0;
        if(FAILED(command->Execute(nullptr,__uuidof(IMultipleResults),nullptr,&affected,reinterpret_cast<IUnknown**>(results.put())))||!results.get())return false;
        for(;;){
            const HRESULT next=results->GetResult(nullptr,DBRESULTFLAG_DEFAULT,__uuidof(IRowset),&affected,reinterpret_cast<IUnknown**>(rows.put()));
            if(FAILED(next)||next==DB_S_NORESULT)return false;
            if(rows.get())break;
        }
        Com<IAccessor> access;if(FAILED(rows->QueryInterface(__uuidof(IAccessor),reinterpret_cast<void**>(access.put()))))return false;
        struct Cell { DBSTATUS status;DBLENGTH length;wchar_t value[262145]; };
        auto cell=std::make_unique<Cell>();memset(cell.get(),0,sizeof(Cell));
        DBBINDING binding={};binding.iOrdinal=1;binding.obStatus=offsetof(Cell,status);binding.obLength=offsetof(Cell,length);
        binding.obValue=offsetof(Cell,value);binding.dwPart=DBPART_VALUE|DBPART_STATUS|DBPART_LENGTH;
        binding.dwMemOwner=DBMEMOWNER_CLIENTOWNED;binding.wType=DBTYPE_WSTR;binding.cbMaxLen=sizeof(cell->value);
        HACCESSOR handle=DB_NULL_HACCESSOR;DBBINDSTATUS status=DBBINDSTATUS_BADBINDINFO;
        if(FAILED(access->CreateAccessor(DBACCESSOR_ROWDATA,1,&binding,sizeof(Cell),&handle,&status))||status!=DBBINDSTATUS_OK)return false;
        DBCOUNTITEM count=0;HROW* data=nullptr;HRESULT hr=rows->GetNextRows(DB_NULL_HCHAPTER,0,1,&count,&data);
        if(SUCCEEDED(hr)&&count==1)hr=rows->GetData(data[0],handle,cell.get());
        const bool ok=SUCCEEDED(hr)&&count==1&&cell->status==DBSTATUS_S_OK&&cell->length<sizeof(cell->value)&&cell->length%2==0;
        if(ok)value.assign(cell->value,cell->length/sizeof(wchar_t));
        if(count&&data)rows->ReleaseRows(count,data,nullptr,nullptr,nullptr);
        CoTaskMemFree(data);access->ReleaseAccessor(handle,nullptr);return ok;
    }
};
inline bool Need(bool value,const char* operation="friend policy SQL operation failed"){if(!value)throw std::runtime_error(operation);return value;}
}
