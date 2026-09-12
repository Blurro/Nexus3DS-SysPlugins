#include <3ds.h>
#include "memory.h"
#include "draw.h"
#include "menu.h"

#define PLUGIN_CODE(id)   __attribute__((section(".plugin_" #id), used))
#define PLUGIN_MAIN(id)   __attribute__((section(".plugin_" #id "_entry"), used))
#define PLUGIN_RODATA(id) __attribute__((section(".pluginrodata_" #id), used))
#define PLUGIN_DATA(id)   __attribute__((section(".plugindata_" #id), used))
#define PLUGIN_BSS(id)    __attribute__((section(".pluginbss_" #id), used))

typedef struct PluginMenuRegistration
{
    u32 pluginId;
    const char *title;
    void (*callback)(void);
    u32 color;
    struct PluginMenuRegistration *next;
} PluginMenuRegistration;

extern void *pluginTable_blur[];

#define BLUR_HOST__svcFlushEntireDataCache          ((void(*)(void))pluginTable_blur[6])
#define BLUR_HOST__svcSleepThread                   ((void(*)(s64))pluginTable_blur[1])
#define BLUR_MENU__MapPage                         ((bool(*)(Handle,u32,u32*,u32*))pluginTable_blur[7])
#define BLUR_MENU__UnmapPage                       ((void(*)(u32))pluginTable_blur[8])
#define BLUR_HOST__blur_marker_menudraw_start       ((u32)pluginTable_blur[9])
#define BLUR_HOST__blur_marker_menudraw_end         ((u32)pluginTable_blur[10])
#define BLUR_HOST__blur_marker_menu_entered         ((u32)pluginTable_blur[12])
#define BLUR_HOST__blur_marker_menu_leaving         ((u32)pluginTable_blur[13])
#define BLUR_HOST__Draw_SetupFramebuffer            ((void(*)(void))pluginTable_blur[14])
#define BLUR_HOST__Draw_RestoreFramebuffer          ((void(*)(void))pluginTable_blur[15])
#define BLUR_HOST__Draw_FreeFramebufferCache        ((void(*)(void))pluginTable_blur[16])
#define BLUR_HOST__svcInvalidateEntireInstructionCache ((void(*)(void))pluginTable_blur[17])
#define BLUR_MENU__AddItem                          ((bool(*)(PluginMenuRegistration*,u32,const char*,void(*)(void),u32))pluginTable_blur[23])
#define BLUR_MENU__AddOnlineEntry                   ((bool(*)(const char*,const char*))pluginTable_blur[33])
#define BLUR_HOST__menuShouldExit                   (*(bool*)pluginTable_blur[30])
#define BLUR_PLUGIN_ID                              0x72756C62u

extern const char g_blurFeatureTitle[];
extern const char g_blurOnlineV1Title[];
extern const char g_blurOnlineV1Url[];
extern PluginMenuRegistration g_blurMenuRegistration;
extern void PLUGIN_blur_RunMenuDrawHookBody(Menu *currentMenu);
extern void PLUGIN_blur_SetMenuFreezeInternal(bool freeze);
extern bool PLUGIN_blur_EnsureCoolThread(void);
extern void PLUGIN_blur_SetHostIsLuma(bool isLuma);
extern void PLUGIN_blur_OpenFeatureMenu(void);
extern void PLUGIN_blur_LoadMenuSettings(void);
extern bool PLUGIN_blur_IsMenuTextEnabled(void);

PLUGIN_DATA(blur) u32 blur_menudraw_return_addr = 0u;
PLUGIN_DATA(blur) u32 blur_menu_enter_return_addr = 0u;
PLUGIN_DATA(blur) u32 blur_menu_leave_return_addr = 0u;
PLUGIN_BSS(blur) static u32 g_blurMenuDrawOriginal0;
PLUGIN_BSS(blur) static u32 g_blurMenuDrawOriginal1;
PLUGIN_BSS(blur) static bool g_blurMenuDrawInstalled;
PLUGIN_DATA(blur) static bool g_blurSleepIoAllowed = true;
PLUGIN_DATA(blur) static u32 g_blurSleepReplyTarget;
PLUGIN_DATA(blur) static u32 g_blurSleepReplyReturnAddr;
PLUGIN_BSS(blur) static volatile s32 g_blurSleepIoLock;
PLUGIN_BSS(blur) static u32 g_blurSleepIoUsers;
PLUGIN_BSS(blur) static u32 g_blurSleepCycleCounter;

PLUGIN_CODE(blur) __attribute__((naked)) void PLUGIN_blur_MenuDrawHook(void)
{
    __asm__ volatile(
        // space for the return address
        "sub sp, sp, #4\n"
        "push {r0-r12, lr}\n"
        "mrs r12, cpsr\n"
        "push {r12}\n"

        // r6 moved through the items, the saved count is 0x6C above this frame
        "ldr r0, [sp, #0x6C]\n"
        "sub r0, r6, r0, lsl #4\n"
        "bl PLUGIN_blur_RunMenuDrawHookBody\n"

        // keep the return address in plugin data instead of patching this code
        "ldr r12, 1f\n"
        "ldr r12, [r12]\n"
        "cmp r12, #0\n"
        "bne 2f\n"
        "ldr r12, 3f\n"
        "ldr r12, [r12, #44]\n"
        "2:\n"
        "str r12, [sp, #60]\n"

        "pop {r12}\n"
        "msr cpsr_f, r12\n"
        "pop {r0-r12, lr}\n"
        "pop {pc}\n"
        "1:\n"
        ".word blur_menudraw_return_addr\n"
        "3:\n"
        ".word pluginTable_blur\n"
    );
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_InstallMenuDrawHook(u32 *saved0, u32 *saved1)
{
    u32 start = BLUR_HOST__blur_marker_menudraw_start;
    u32 end = BLUR_HOST__blur_marker_menudraw_end;
    u32 hostMapBase = 0;
    u32 hostAddress = 0;
    u32 instr0;
    u32 instr1;

    if (!BLUR_MENU__MapPage(CUR_PROCESS_HANDLE, start, &hostMapBase, &hostAddress))
        return false;

    instr0 = *(volatile u32*)hostAddress;
    instr1 = *(volatile u32*)(hostAddress + 4);

    if (end <= start + 8u || end - start > 0x400u)
    {
        BLUR_MENU__UnmapPage(hostMapBase);
        return false;
    }

    if (saved0)
        *saved0 = instr0;
    if (saved1)
        *saved1 = instr1;

    PLUGIN_blur_SetHostIsLuma(end - start < 0x80u);
    blur_menudraw_return_addr = end;
    *(volatile u32*)(hostAddress + 4) = (u32)PLUGIN_blur_MenuDrawHook;
    *(volatile u32*)hostAddress = 0xE51FF004;

    BLUR_MENU__UnmapPage(hostMapBase);
    return true;
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_RestoreHostWords(
    u32 address,
    u32 expectedHook,
    u32 word0,
    u32 word1
);
PLUGIN_CODE(blur) static void PLUGIN_blur_SyncExecutableChanges(void);

PLUGIN_CODE(blur) bool PLUGIN_blur_SetMenuTextHookEnabled(bool enabled)
{
    if (enabled == g_blurMenuDrawInstalled)
        return true;

    if (enabled)
    {
        u32 original0;
        u32 original1;
        if (!PLUGIN_blur_InstallMenuDrawHook(&original0, &original1))
            return false;

        g_blurMenuDrawOriginal0 = original0;
        g_blurMenuDrawOriginal1 = original1;
        g_blurMenuDrawInstalled = true;
        PLUGIN_blur_SyncExecutableChanges();
        return true;
    }

    if (!PLUGIN_blur_RestoreHostWords(
            BLUR_HOST__blur_marker_menudraw_start,
            (u32)PLUGIN_blur_MenuDrawHook,
            g_blurMenuDrawOriginal0,
            g_blurMenuDrawOriginal1))
    {
        return false;
    }

    g_blurMenuDrawInstalled = false;
    PLUGIN_blur_SyncExecutableChanges();
    return true;
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_RestoreHostWords(
    u32 address,
    u32 expectedHook,
    u32 word0,
    u32 word1
)
{
    u32 mapBase;
    u32 mappedAddress;

    if (!BLUR_MENU__MapPage(CUR_PROCESS_HANDLE, address, &mapBase, &mappedAddress))
        return false;

    u32 current0 = *(volatile u32*)mappedAddress;
    u32 current1 = *(volatile u32*)(mappedAddress + 4);

    if (current0 == word0 && current1 == word1)
    {
        BLUR_MENU__UnmapPage(mapBase);
        return true;
    }

    if (current0 != 0xE51FF004u || current1 != expectedHook)
    {
        BLUR_MENU__UnmapPage(mapBase);
        return false;
    }

    *(volatile u32*)mappedAddress = word0;
    *(volatile u32*)(mappedAddress + 4) = word1;
    BLUR_MENU__UnmapPage(mapBase);
    return true;
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_DecodeArmBranch(u32 instr, u32 address, u32 *target)
{
    if (!target || (instr & 0x0E000000u) != 0x0A000000u)
        return false;

    s32 imm24 = (s32)(instr << 8) >> 8;
    *target = (u32)((s32)(address + 8) + imm24 * 4);
    return true;
}

PLUGIN_CODE(blur) static void PLUGIN_blur_LockSleepIo(void)
{
    s32 *lock = (s32*)&g_blurSleepIoLock;

    for (;;)
    {
        if (__ldrex(lock) == 0)
        {
            if (!__strex(lock, 1))
            {
                __dmb();
                return;
            }
        }
        else
        {
            __clrex();
        }

        BLUR_HOST__svcSleepThread(1000);
    }
}

PLUGIN_CODE(blur) static void PLUGIN_blur_UnlockSleepIo(void)
{
    __dmb();
    g_blurSleepIoLock = 0;
}

PLUGIN_CODE(blur) bool PLUGIN_blur_SleepTryEnterIo(void)
{
    PLUGIN_blur_LockSleepIo();

    // main.c clears this only after a denied sleep or FULLY_AWAKE
    if (!g_blurSleepIoAllowed && !BLUR_HOST__menuShouldExit)
        g_blurSleepIoAllowed = true;

    bool entered = g_blurSleepIoAllowed && !BLUR_HOST__menuShouldExit;
    if (entered)
        g_blurSleepIoUsers++;

    PLUGIN_blur_UnlockSleepIo();
    return entered;
}

PLUGIN_CODE(blur) void PLUGIN_blur_SleepLeaveIo(void)
{
    PLUGIN_blur_LockSleepIo();
    if (g_blurSleepIoUsers)
        g_blurSleepIoUsers--;
    PLUGIN_blur_UnlockSleepIo();
}

PLUGIN_CODE(blur) u32 PLUGIN_blur_SleepCycleCounter(void)
{
    __dmb();
    return g_blurSleepCycleCounter;
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_CloseSleepIo(void)
{
    PLUGIN_blur_LockSleepIo();
    g_blurSleepIoAllowed = false;
    g_blurSleepCycleCounter++;
    PLUGIN_blur_UnlockSleepIo();

    // normal saves finish here; a stuck task makes PTM deny this sleep instead
    for (u32 waits = 0; waits < 250u; waits++)
    {
        PLUGIN_blur_LockSleepIo();
        bool busy = g_blurSleepIoUsers != 0;
        PLUGIN_blur_UnlockSleepIo();
        if (!busy)
            return true;
        BLUR_HOST__svcSleepThread(1000000LL);
    }

    return false;
}

PLUGIN_CODE(blur) static Result PLUGIN_blur_SleepReplyHookBody(bool deny)
{
    bool drained = PLUGIN_blur_CloseSleepIo();
    Result (*reply)(bool) = (Result(*)(bool))g_blurSleepReplyTarget;
    return reply ? reply(deny || !drained) : (Result)-1;
}

PLUGIN_CODE(blur) __attribute__((naked)) static void PLUGIN_blur_SleepReplyHook(void)
{
    __asm__ volatile(
        "bl PLUGIN_blur_SleepReplyHookBody\n"
        "pop {r4, lr}\n"
        "ldr r12, 1f\n"
        "ldr pc, [r12]\n"
        "1:\n"
        ".word g_blurSleepReplyReturnAddr\n"
    );
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_MatchSleepReplySite(
    u32 address,
    u32 textStart,
    u32 textEnd
)
{
    volatile u32 *call = (volatile u32*)address;
    if (call[-5] != 0xE3A02001u ||
        (call[-4] & 0xFFFFF000u) != 0xE59F3000u ||
        call[-3] != 0xE5C32000u ||
        (call[-2] & 0xFFFFF000u) != 0xE59F3000u ||
        call[-1] != 0xE5D30000u ||
        (call[0] & 0xFF000000u) != 0xEB000000u ||
        call[1] != 0xE8BD4010u ||
        (call[2] & 0xFF000000u) != 0xEA000000u)
    {
        return false;
    }

    u32 literal = address - 8u + (call[-4] & 0xFFFu);
    return literal >= textStart && literal + 4u <= textEnd &&
        *(volatile u32*)literal == (u32)pluginTable_blur[30];
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_InstallSleepReplyHook(
    u32 *addressOut,
    u32 *original0Out,
    u32 *original1Out
)
{
    u32 marker = BLUR_HOST__blur_marker_menudraw_start;
    u32 textStart = marker & 0xFFF00000u;
    u32 textEnd = marker;
    u32 found = 0;
    u32 matches = 0;

    if (textEnd <= textStart + 0x100u || textEnd - textStart > 0x100000u)
        return false;

    for (u32 address = textStart + 20u; address + 12u <= textEnd; address += 4u)
    {
        if (PLUGIN_blur_MatchSleepReplySite(address, textStart, textEnd))
        {
            found = address;
            matches++;
        }
    }

    if (matches != 1u)
        return false;

    u32 original0 = *(volatile u32*)found;
    u32 original1 = *(volatile u32*)(found + 4u);
    u32 target = 0;
    if (!PLUGIN_blur_DecodeArmBranch(original0, found, &target) ||
        original1 != 0xE8BD4010u)
    {
        return false;
    }

    u32 mapBase = 0;
    u32 mappedAddress = 0;
    if (!BLUR_MENU__MapPage(CUR_PROCESS_HANDLE, found, &mapBase, &mappedAddress))
        return false;
    if (*(volatile u32*)mappedAddress != original0 ||
        *(volatile u32*)(mappedAddress + 4u) != original1)
    {
        BLUR_MENU__UnmapPage(mapBase);
        return false;
    }

    g_blurSleepReplyTarget = target;
    g_blurSleepReplyReturnAddr = found + 8u;
    *(volatile u32*)(mappedAddress + 4u) = (u32)PLUGIN_blur_SleepReplyHook;
    *(volatile u32*)mappedAddress = 0xE51FF004u;
    BLUR_MENU__UnmapPage(mapBase);
    *addressOut = found;
    *original0Out = original0;
    *original1Out = original1;
    return true;
}

PLUGIN_CODE(blur) static void PLUGIN_blur_MenuEnterFreezeBody(void)
{
    PLUGIN_blur_SetMenuFreezeInternal(true);
    BLUR_HOST__Draw_SetupFramebuffer();
}

PLUGIN_CODE(blur) __attribute__((naked)) void PLUGIN_blur_MenuEnterFreezeHook(void)
{
    __asm__ volatile(
        "bl PLUGIN_blur_MenuEnterFreezeBody\n"
        "ldr r12, 1f\n"
        "ldr pc, [r12]\n"
        "1:\n"
        ".word blur_menu_enter_return_addr\n"
    );
}

PLUGIN_CODE(blur) static void PLUGIN_blur_MenuLeaveFreezeBody(void)
{
    PLUGIN_blur_SetMenuFreezeInternal(false);
    BLUR_HOST__Draw_RestoreFramebuffer();
    BLUR_HOST__Draw_FreeFramebufferCache();
}

PLUGIN_CODE(blur) __attribute__((naked)) void PLUGIN_blur_MenuLeaveFreezeHook(void)
{
    __asm__ volatile(
        "bl PLUGIN_blur_MenuLeaveFreezeBody\n"
        "ldr r12, 1f\n"
        "ldr pc, [r12]\n"
        "1:\n"
        ".word blur_menu_leave_return_addr\n"
    );
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_InstallMenuFreezeEnterHook(u32 *saved0, u32 *saved1)
{
    u32 marker = BLUR_HOST__blur_marker_menu_entered;
    u32 hostMapBase = 0;
    u32 hostAddress = 0;
    u32 instr0;
    u32 instr1;
    u32 setupTarget;
    u32 returnTarget;

    if (!BLUR_MENU__MapPage(CUR_PROCESS_HANDLE, marker, &hostMapBase, &hostAddress))
        return false;

    instr0 = *(volatile u32*)hostAddress;
    instr1 = *(volatile u32*)(hostAddress + 4);

    if (!PLUGIN_blur_DecodeArmBranch(instr0, marker, &setupTarget) ||
        !PLUGIN_blur_DecodeArmBranch(instr1, marker + 4, &returnTarget) ||
        (instr0 & 0x01000000u) == 0 ||
        (instr1 & 0x01000000u) != 0 ||
        setupTarget != (u32)BLUR_HOST__Draw_SetupFramebuffer)
    {
        BLUR_MENU__UnmapPage(hostMapBase);
        return false;
    }

    if (saved0) *saved0 = instr0;
    if (saved1) *saved1 = instr1;

    blur_menu_enter_return_addr = returnTarget;
    *(volatile u32*)(hostAddress + 4) = (u32)PLUGIN_blur_MenuEnterFreezeHook;
    *(volatile u32*)hostAddress = 0xE51FF004u;
    BLUR_MENU__UnmapPage(hostMapBase);
    return true;
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_InstallMenuFreezeLeaveHook(u32 *saved0, u32 *saved1)
{
    u32 marker = BLUR_HOST__blur_marker_menu_leaving;
    u32 hostMapBase = 0;
    u32 hostAddress = 0;
    u32 instr0;
    u32 instr1;
    u32 restoreTarget;
    u32 freeTarget;

    if (!BLUR_MENU__MapPage(CUR_PROCESS_HANDLE, marker, &hostMapBase, &hostAddress))
        return false;

    instr0 = *(volatile u32*)hostAddress;
    instr1 = *(volatile u32*)(hostAddress + 4);

    if (!PLUGIN_blur_DecodeArmBranch(instr0, marker, &restoreTarget) ||
        !PLUGIN_blur_DecodeArmBranch(instr1, marker + 4, &freeTarget) ||
        (instr0 & 0x01000000u) == 0 ||
        (instr1 & 0x01000000u) == 0 ||
        restoreTarget != (u32)BLUR_HOST__Draw_RestoreFramebuffer ||
        freeTarget != (u32)BLUR_HOST__Draw_FreeFramebufferCache)
    {
        BLUR_MENU__UnmapPage(hostMapBase);
        return false;
    }

    if (saved0) *saved0 = instr0;
    if (saved1) *saved1 = instr1;

    blur_menu_leave_return_addr = marker + 8u;
    *(volatile u32*)(hostAddress + 4) = (u32)PLUGIN_blur_MenuLeaveFreezeHook;
    *(volatile u32*)hostAddress = 0xE51FF004u;
    BLUR_MENU__UnmapPage(hostMapBase);
    return true;
}

typedef struct
{
    u32 installed;
    u32 draw0;
    u32 draw1;
    u32 enter0;
    u32 enter1;
    u32 leave0;
    u32 leave1;
    u32 sleepAddress;
    u32 sleepOriginal0;
    u32 sleepOriginal1;
} BlurHookState;

#define BLUR_HOOK_DRAW  (1u << 0)
#define BLUR_HOOK_ENTER (1u << 1)
#define BLUR_HOOK_LEAVE (1u << 2)
#define BLUR_HOOK_SLEEP (1u << 3)

PLUGIN_CODE(blur) static void PLUGIN_blur_SyncExecutableChanges(void)
{
    BLUR_HOST__svcFlushEntireDataCache();
    BLUR_HOST__svcInvalidateEntireInstructionCache();
}

PLUGIN_CODE(blur) static bool PLUGIN_blur_RollBackHooks(const BlurHookState *state)
{
    bool restored = true;

    if ((state->installed & BLUR_HOOK_SLEEP) &&
        !PLUGIN_blur_RestoreHostWords(
            state->sleepAddress,
            (u32)PLUGIN_blur_SleepReplyHook,
            state->sleepOriginal0,
            state->sleepOriginal1))
    {
        restored = false;
    }

    if ((state->installed & BLUR_HOOK_LEAVE) &&
        !PLUGIN_blur_RestoreHostWords(
            BLUR_HOST__blur_marker_menu_leaving,
            (u32)PLUGIN_blur_MenuLeaveFreezeHook,
            state->leave0,
            state->leave1))
    {
        restored = false;
    }

    if ((state->installed & BLUR_HOOK_ENTER) &&
        !PLUGIN_blur_RestoreHostWords(
            BLUR_HOST__blur_marker_menu_entered,
            (u32)PLUGIN_blur_MenuEnterFreezeHook,
            state->enter0,
            state->enter1))
    {
        restored = false;
    }

    if ((state->installed & BLUR_HOOK_DRAW) &&
        !PLUGIN_blur_RestoreHostWords(
            BLUR_HOST__blur_marker_menudraw_start,
            (u32)PLUGIN_blur_MenuDrawHook,
            state->draw0,
            state->draw1))
    {
        restored = false;
    }

    if ((state->installed & BLUR_HOOK_DRAW) && restored)
        g_blurMenuDrawInstalled = false;

    PLUGIN_blur_SyncExecutableChanges();
    return restored;
}

PLUGIN_MAIN(blur) bool PLUGIN_blur_Main(void)
{
    BlurHookState state;

    state.installed = 0;

    if (!BLUR_MENU__AddItem)
        return false;

    PLUGIN_blur_LoadMenuSettings();
    PLUGIN_blur_SetMenuFreezeInternal(false);

    (void)BLUR_MENU__AddOnlineEntry(g_blurOnlineV1Title, g_blurOnlineV1Url);

    if (PLUGIN_blur_IsMenuTextEnabled())
    {
        if (!PLUGIN_blur_InstallMenuDrawHook(&state.draw0, &state.draw1))
            goto fail;
        g_blurMenuDrawOriginal0 = state.draw0;
        g_blurMenuDrawOriginal1 = state.draw1;
        g_blurMenuDrawInstalled = true;
        state.installed |= BLUR_HOOK_DRAW;
    }
    else
    {
        u32 span = BLUR_HOST__blur_marker_menudraw_end - BLUR_HOST__blur_marker_menudraw_start;
        PLUGIN_blur_SetHostIsLuma(span < 0x80u);
    }

    if (!PLUGIN_blur_InstallMenuFreezeEnterHook(&state.enter0, &state.enter1))
        goto fail;
    state.installed |= BLUR_HOOK_ENTER;

    if (!PLUGIN_blur_InstallMenuFreezeLeaveHook(&state.leave0, &state.leave1))
        goto fail;
    state.installed |= BLUR_HOOK_LEAVE;

    if (!PLUGIN_blur_InstallSleepReplyHook(
            &state.sleepAddress,
            &state.sleepOriginal0,
            &state.sleepOriginal1))
    {
        goto fail;
    }
    state.installed |= BLUR_HOOK_SLEEP;

    PLUGIN_blur_SyncExecutableChanges();

    if (!PLUGIN_blur_EnsureCoolThread())
        goto fail;

    (void)BLUR_MENU__AddItem(
        &g_blurMenuRegistration,
        BLUR_PLUGIN_ID,
        g_blurFeatureTitle,
        PLUGIN_blur_OpenFeatureMenu,
        RGB565(6, 25, 31)
    );

    return true;

fail:
    // only unload Blur if every hook was removed safely
    if (PLUGIN_blur_RollBackHooks(&state))
        return false;

    (void)PLUGIN_blur_EnsureCoolThread();
    return true;
}
