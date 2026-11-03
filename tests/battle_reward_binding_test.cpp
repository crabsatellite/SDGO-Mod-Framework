#include <windows.h>
#include <filesystem>
#include <iostream>
#include "mods/battle_rewards/wallet_jobs.h"
using namespace sdgo::battle_rewards;
int main(){
    int failures=0;
    const auto check=[&](bool ok,const char* text){if(!ok){++failures;std::cerr<<"FAIL "<<text<<"\n";}};
    const auto root=std::filesystem::temp_directory_path()/
        (L"sdgo-wallet-binding-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    if(!std::filesystem::create_directory(root))return 2;
    const std::string scope="10000000-2000-4000-8000-000000000001";
    check(!ReadWalletBinding(root).valid,"a never-bound native ledger must fail closed");
    check(runtime_detail::AtomicWrite(root/L"wallet-binding.ini","[binding]\r\nSchemaVersion=1\r\nProtocol=sdgo-native-wallet-scope-v1\r\nScopeId="+scope+"\r\n",nullptr),"verified owner binding is written");
    check(!ReadWalletHealth(root).healthy,"a missing consumer is not reported healthy");
    check(ReadWalletBinding(root).valid&&ReadWalletBinding(root).scope==scope,"producer identity survives a missing consumer");
    check(runtime_detail::AtomicWrite(root/L"wallet-status.ini","[wallet]\r\nSchemaVersion=1\r\nHealthy=0\r\nHeartbeatUnix=0\r\nScopeId="+scope+"\r\nRevision=sdgo-native-gc-ledger-v2\r\nLastError=wallet_worker_stopped\r\n",nullptr),"stopped consumer state is written");
    check(!ReadWalletHealth(root).healthy&&ReadWalletBinding(root).valid,"stopped heartbeat cannot cancel a bound native settlement");
    WalletJob job;job.match_id="native-captured-restart-regression";job.account=42;job.mb=73;
    job.plan_hash=std::string(64,'a');job.policy_revision=std::string(64,'b');job.scope=ReadWalletBinding(root).scope;
    job.completed_unix=1793008800;job.active_seconds=180;job.contributed=true;
    std::string name,error;
    check(PublishWalletJob(root,job,&name,&error),"earned native job persists while the worker is down");
    check(std::filesystem::is_regular_file(root/L"outbox"/name),"deferred payment survives in the durable outbox");
    check(PublishWalletJob(root,job,&name,&error),"same earned job can be replayed without a second identity");
    check(runtime_detail::AtomicWrite(root/L"wallet-binding.ini","[binding]\r\nSchemaVersion=1\r\nProtocol=other\r\nScopeId="+scope+"\r\n",nullptr),"foreign binding fixture written");
    check(!ReadWalletBinding(root).valid,"a foreign protocol cannot authorize new native jobs");
    std::filesystem::remove_all(root);
    std::cout<<"Wallet binding/deferred native jobs: "<<failures<<" failures\n";return failures?1:0;
}
