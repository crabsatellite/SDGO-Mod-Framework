#pragma once

#include "operator_flow_actions.h"
#include "shop_catalog_actions.h"

// One original ordinary-shop confirmation, never retried. Inventory and SQL
// persistence are deliberately not inferred from its ACK or wallet result.
class ShopPurchaseActions {
public:
    bool Bind(const SdgoApi* api, SdgoMod* mod, NativeActions* native,
              ShopCatalogActions* shop) {
        if (!api || !api->HookCode || !api->Symbol || !native || !shop || active_)
            return false;
        api_=api;mod_=mod;native_=native;shop_=shop;active_=this;
        extensionVtable_=api->Symbol(mod,"test.shop_extension_vtable");
        extensionCallback_=reinterpret_cast<OperatorFlowActions::UiCallbackFn>(api->Symbol(mod,"test.shop_extension_callback"));
        childLookup_=reinterpret_cast<ShopCatalogActions::ServiceFindByNameFn>(api->Symbol(mod,"test.shop_service_child_lookup"));
        if(!extensionVtable_ || !extensionCallback_ || !childLookup_)return false;
        const char* names[]={"flow.operator_shop_dispatch_1","flow.operator_shop_dispatch_2","flow.operator_shop_dispatch_3"};
        void* hooks[]={reinterpret_cast<void*>(&Hook0),reinterpret_cast<void*>(&Hook1),reinterpret_cast<void*>(&Hook2)};
        for(size_t i=0;i<3;++i)
            if(!api->HookCode(mod,names[i],hooks[i],reinterpret_cast<void**>(&original_[i])))return false;
        return true;
    }

    int Poll(const SdgoGameTestContextV1* context) {
        if(!armed_) {
            const auto result=shop_->Poll(context);
            const auto current=shop_->State();
            if(lastCatalog_!=result || lastCatalogStage_!=current.stage) {
                char text[320]{};sprintf_s(text,"native purchase catalog=%s stage=%u product=%u option=%u name=%u ui=%u inventoryFailure=%u ready=%u",ShopCatalogActions::ResultName(result),current.stage,current.product_id,current.option_id,static_cast<unsigned>(current.native_name.size()),static_cast<unsigned>(current.visible_name.size()),current.inventory_failure,current.inventory_ready);
                context->Observe(text);lastCatalog_=result;lastCatalogStage_=current.stage;
            }
            if(result>=ShopCatalogActions::kInvalidContext || result==ShopCatalogActions::kUnbound || result==ShopCatalogActions::kUnconfigured)
                return SDGO_GAME_TEST_FAIL;
            const auto state=shop_->State();
            void* popup=nullptr;
            if(!shop_->HandoffBuyOptions(state.product_id,state.category,&popup))return SDGO_GAME_TEST_WAIT;
            OperatorFlowActions::Config config{};
            config.transport_wallet_only=true;
            config.product_id=state.product_id;config.option_id=state.option_id;
            config.category_id=state.category;config.price=state.price;
            config.validity=state.validity;config.payment=static_cast<uint8_t>(state.payment);
            const auto& n=shop_->NativeBindings();
            OperatorFlowActions::Bindings bindings{};
            bindings.profile_pointer=n.profile_pointer;bindings.service_managed_pointer=n.service_managed_pointer;
            bindings.image_base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            bindings.options_vtable=n.buy_vtable;bindings.options_callback=n.buy_callback;
            bindings.confirmation_vtable=api_->Symbol(mod_,"flow.shop_confirmation_vtable");
            bindings.confirmation_callback=reinterpret_cast<OperatorFlowActions::UiCallbackFn>(api_->Symbol(mod_,"flow.shop_confirmation_callback"));
            const auto& baseline=shop_->Baseline();
            if(!flow_.Configure(config) || !flow_.Bind(bindings,native_) ||
               !flow_.PrimeWalletBaseline({baseline.gp,baseline.cash}) || !flow_.AdoptOptionsPopup(popup))
                return SDGO_GAME_TEST_FAIL;
            armed_=true;
        }
        const auto result=flow_.Poll(context);
        if(result==OperatorFlowActions::kWaitingForAutomaticCommit)
            TryExtension(context);
        if(extensionFault_)return SDGO_GAME_TEST_FAIL;
        const auto state=flow_.State();
        if(lastResult_!=result || lastStage_!=state.stage) {
            char message[512]{};
            sprintf_s(message,"native one-shot shop purchase result=%s stage=%u product=%u option=%u mutation=%u req0931=%u ack074D=%u req0758=%u ack0574=%u original=%u wallet=%u GP=%u/%u cash=%u/%u inventoryProof=external-required",
                OperatorFlowActions::ResultName(result),state.stage,state.product_id,state.option_id,state.mutation_attempted,
                state.request_0931_sequence,state.reply_074d_sequence,state.request_0758_sequence,state.reply_0574_sequence,
                state.original_0574_completed,state.wallet_verified,state.baseline_gp,state.current_gp,state.baseline_cash,state.current_cash);
            context->Observe(message);lastResult_=result;lastStage_=state.stage;
        }
        if(result>=OperatorFlowActions::kInvalidContext)return SDGO_GAME_TEST_FAIL;
        if(result!=OperatorFlowActions::kComplete)return SDGO_GAME_TEST_WAIT;
        const bool accepted=state.options_actions==1 && state.confirmation_actions==1 && state.mutation_attempted &&
            state.request_0931_sequence && state.reply_074d_sequence && state.request_0758_sequence && state.reply_0574_sequence &&
            state.original_0574_completed && state.wallet_verified && !state.operator_verified;
        context->Assert(accepted,"one original purchase completed its real transport and exact native wallet debit; inventory/persistence needs independent readback");
        return accepted?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
    }

    bool ExtensionAwaitingCapture() const {return extensionReady_&&!extensionAttempted_;}

private:
    void TryExtension(const SdgoGameTestContextV1* context) {
        if(extensionAttempted_)return;
        void* popup=nullptr;
        if(!flow_.AuthorizedExtensionPopup(extensionVtable_,&popup))return;
        const auto state=flow_.State();const auto& n=shop_->NativeBindings();
        alignas(4) uint8_t name[0x1C]{};bool constructed=false;
        void* root=nullptr;void* yes=nullptr;void* no=nullptr;
        __try {
            void* callback=*reinterpret_cast<void**>(static_cast<uint8_t*>(extensionVtable_)+0x38);
            void* lookup=*reinterpret_cast<void**>(static_cast<uint8_t*>(extensionVtable_)+0x5C);
            uint16_t opcode=0;uint32_t product=0,option=0;
            memcpy(&opcode,static_cast<uint8_t*>(popup)+0x4C,2);
            memcpy(&product,static_cast<uint8_t*>(popup)+0x52,4);
            memcpy(&option,static_cast<uint8_t*>(popup)+0x56,4);
            if(callback!=reinterpret_cast<void*>(extensionCallback_) || lookup!=reinterpret_cast<void*>(childLookup_) ||
               opcode!=0x074D || product!=state.product_id || option!=state.option_id) {extensionFault_=true;return;}
            constructed=n.native_wstring_ctor(name,L"dlg")!=nullptr;
            if(constructed)root=childLookup_(popup,name);
            if(constructed)n.native_wstring_dtor(name);constructed=false;
            if(!root)return;
            constructed=n.native_wstring_ctor(name,L"YesBtn")!=nullptr;
            if(constructed)yes=n.native_ui_find_control(root,name,4);
            if(constructed)n.native_wstring_dtor(name);constructed=false;
            constructed=n.native_wstring_ctor(name,L"NoBtn")!=nullptr;
            if(constructed)no=n.native_ui_find_control(root,name,4);
            if(constructed)n.native_wstring_dtor(name);constructed=false;
            if(!OperatorFlowActions::NativeActionControl(yes,root,L"YesBtn") ||
               !OperatorFlowActions::NativeActionControl(no,root,L"NoBtn"))return;
            if(!extensionReady_) {
                extensionReady_=true;
                context->Observe("native ETC extension confirmation is live and matches the exact original preflight product/option; capture before confirming");
                return;
            }
            // Original CServiceShoppingExtension::OnMessage constructs 0758.
            // Never send that packet ourselves or repeat this final click.
            extensionAttempted_=true;
            extensionCallback_(popup,0x101,static_cast<uint8_t*>(yes)+0x118,yes);
            context->Observe("invoked the original ETC YesBtn callback once for the sealed purchase");
        } __except(EXCEPTION_EXECUTE_HANDLER) {extensionFault_=true;}
        if(constructed)n.native_wstring_dtor(name);
    }
    using Dispatch=int(__thiscall*)(void*,int,int,int,int,void*);
    struct Copy {std::array<uint8_t,0xD4> bytes{};uint32_t size=0;};
    static Copy CopyReply(uint16_t opcode,void* packet) {
        Copy copy;
        copy.size=opcode==0x074D?0x33:opcode==0x0574?0xD4:
            (opcode==0x05D8 || opcode==0x05DA || opcode==0x09BD)?8:0;
        if(copy.size) {
            __try {memcpy(copy.bytes.data(),packet,copy.size);}
            __except(EXCEPTION_EXECUTE_HANDLER) {copy.size=0;}
        }
        return copy;
    }
    static int Observe(size_t index,void* self,int a,int b,int c,int opcode,void* packet) {
        auto* instance=active_;
        if(!instance || !instance->original_[index])return 0;
        const auto copy=CopyReply(static_cast<uint16_t>(opcode),packet);
        const int result=instance->original_[index](self,a,b,c,opcode,packet);
        const auto error=GetLastError();
        if(copy.size)instance->flow_.ObserveShopReply(static_cast<uint16_t>(opcode),copy.bytes.data(),copy.size,true);
        SetLastError(error);return result;
    }
    static int __fastcall Hook0(void* self,void*,int a,int b,int c,int opcode,void* packet) {return Observe(0,self,a,b,c,opcode,packet);}
    static int __fastcall Hook1(void* self,void*,int a,int b,int c,int opcode,void* packet) {return Observe(1,self,a,b,c,opcode,packet);}
    static int __fastcall Hook2(void* self,void*,int a,int b,int c,int opcode,void* packet) {return Observe(2,self,a,b,c,opcode,packet);}
    inline static ShopPurchaseActions* active_=nullptr;
    const SdgoApi* api_=nullptr;SdgoMod* mod_=nullptr;NativeActions* native_=nullptr;ShopCatalogActions* shop_=nullptr;
    Dispatch original_[3]{};OperatorFlowActions flow_;
    bool armed_=false;uint32_t lastResult_=UINT32_MAX,lastStage_=UINT32_MAX;
    uint32_t lastCatalog_=UINT32_MAX,lastCatalogStage_=UINT32_MAX;
    void* extensionVtable_=nullptr;
    OperatorFlowActions::UiCallbackFn extensionCallback_=nullptr;
    ShopCatalogActions::ServiceFindByNameFn childLookup_=nullptr;
    bool extensionReady_=false,extensionAttempted_=false,extensionFault_=false;
};
