// Verified cdecl style return sites; included inside sync's private namespace.
void* returnSites[28]{};
#define RGB_RETURN(Index,Battle) \
__declspec(naked) void StyleReturn##Index(){ \
__asm { pushfd } __asm { pushad } __asm { mov eax,esp } \
__asm { sub esp,210h } __asm { and esp,0FFFFFFF0h } \
__asm { mov [esp+200h],eax } __asm { fxsave [esp] } \
__asm { fninit } __asm { ldmxcsr dword ptr [mxcsr] } __asm { cld } \
__asm { add eax,36 } __asm { push Battle } __asm { push eax } \
__asm { call CaptureReturn } __asm { fxrstor [esp] } \
__asm { mov esp,[esp+200h] } __asm { popad } __asm { popfd } \
__asm { jmp dword ptr [returnSites+Index*4] } }
RGB_RETURN(0,0)
RGB_RETURN(1,0)
RGB_RETURN(2,0)
RGB_RETURN(3,0)
RGB_RETURN(4,0)
RGB_RETURN(5,0)
RGB_RETURN(6,0)
RGB_RETURN(7,0)
RGB_RETURN(8,0)
RGB_RETURN(9,0)
RGB_RETURN(10,0)
RGB_RETURN(11,0)
RGB_RETURN(12,0)
RGB_RETURN(13,0)
RGB_RETURN(14,0)
RGB_RETURN(15,0)
RGB_RETURN(16,0)
RGB_RETURN(17,0)
RGB_RETURN(18,0)
RGB_RETURN(19,0)
RGB_RETURN(20,0)
RGB_RETURN(21,0)
RGB_RETURN(22,1)
RGB_RETURN(23,1)
RGB_RETURN(24,1)
RGB_RETURN(25,1)
RGB_RETURN(26,1)
RGB_RETURN(27,2)
#undef RGB_RETURN
bool BindStyleReturns(){
    return api->HookSite(mod,"rgb_paint.style_return_0",reinterpret_cast<void*>(&StyleReturn0),&returnSites[0])&&
        api->HookSite(mod,"rgb_paint.style_return_1",reinterpret_cast<void*>(&StyleReturn1),&returnSites[1])&&
        api->HookSite(mod,"rgb_paint.style_return_2",reinterpret_cast<void*>(&StyleReturn2),&returnSites[2])&&
        api->HookSite(mod,"rgb_paint.style_return_3",reinterpret_cast<void*>(&StyleReturn3),&returnSites[3])&&
        api->HookSite(mod,"rgb_paint.style_return_4",reinterpret_cast<void*>(&StyleReturn4),&returnSites[4])&&
        api->HookSite(mod,"rgb_paint.style_return_5",reinterpret_cast<void*>(&StyleReturn5),&returnSites[5])&&
        api->HookSite(mod,"rgb_paint.style_return_6",reinterpret_cast<void*>(&StyleReturn6),&returnSites[6])&&
        api->HookSite(mod,"rgb_paint.style_return_7",reinterpret_cast<void*>(&StyleReturn7),&returnSites[7])&&
        api->HookSite(mod,"rgb_paint.style_return_8",reinterpret_cast<void*>(&StyleReturn8),&returnSites[8])&&
        api->HookSite(mod,"rgb_paint.style_return_9",reinterpret_cast<void*>(&StyleReturn9),&returnSites[9])&&
        api->HookSite(mod,"rgb_paint.style_return_10",reinterpret_cast<void*>(&StyleReturn10),&returnSites[10])&&
        api->HookSite(mod,"rgb_paint.style_return_11",reinterpret_cast<void*>(&StyleReturn11),&returnSites[11])&&
        api->HookSite(mod,"rgb_paint.style_return_12",reinterpret_cast<void*>(&StyleReturn12),&returnSites[12])&&
        api->HookSite(mod,"rgb_paint.style_return_13",reinterpret_cast<void*>(&StyleReturn13),&returnSites[13])&&
        api->HookSite(mod,"rgb_paint.style_return_14",reinterpret_cast<void*>(&StyleReturn14),&returnSites[14])&&
        api->HookSite(mod,"rgb_paint.style_return_15",reinterpret_cast<void*>(&StyleReturn15),&returnSites[15])&&
        api->HookSite(mod,"rgb_paint.style_return_16",reinterpret_cast<void*>(&StyleReturn16),&returnSites[16])&&
        api->HookSite(mod,"rgb_paint.style_return_17",reinterpret_cast<void*>(&StyleReturn17),&returnSites[17])&&
        api->HookSite(mod,"rgb_paint.style_return_18",reinterpret_cast<void*>(&StyleReturn18),&returnSites[18])&&
        api->HookSite(mod,"rgb_paint.style_return_19",reinterpret_cast<void*>(&StyleReturn19),&returnSites[19])&&
        api->HookSite(mod,"rgb_paint.style_return_20",reinterpret_cast<void*>(&StyleReturn20),&returnSites[20])&&
        api->HookSite(mod,"rgb_paint.style_return_21",reinterpret_cast<void*>(&StyleReturn21),&returnSites[21])&&
        api->HookSite(mod,"rgb_paint.style_return_22",reinterpret_cast<void*>(&StyleReturn22),&returnSites[22])&&
        api->HookSite(mod,"rgb_paint.style_return_23",reinterpret_cast<void*>(&StyleReturn23),&returnSites[23])&&
        api->HookSite(mod,"rgb_paint.style_return_24",reinterpret_cast<void*>(&StyleReturn24),&returnSites[24])&&
        api->HookSite(mod,"rgb_paint.style_return_25",reinterpret_cast<void*>(&StyleReturn25),&returnSites[25])&&
        api->HookSite(mod,"rgb_paint.style_return_26",reinterpret_cast<void*>(&StyleReturn26),&returnSites[26])&&
        api->HookSite(mod,"rgb_paint.style_return_27",reinterpret_cast<void*>(&StyleReturn27),&returnSites[27]);
}
