// SPDX-License-Identifier: Zlib
// SPDX-FileNotice: Modified from the BlocksDS default ARM7 core.
//
// Copyright (C) 2005 Michael Noland (joat)
// Copyright (C) 2005 Jason Rogers (Dovoto)
// Copyright (C) 2005-2015 Dave Murphy (WinterMute)
// Copyright (C) 2023 Antonio Niño Díaz
//
// DScheevos launcher ARM7: the BlocksDS default core with DSWiFi, plus a
// handover command. Before the launcher boots nds-bootstrap, the ARM9 asks
// (FIFO_USER_01) to freeze DSWiFi without telling the Wi-Fi chip anything:
// no disconnect, no reset. The chip stays associated, and nds-bootstrap's
// card engine takes it over during the game.

#include <nds.h>
#include <dswifi7.h>

#define FIFO_HANDOVER FIFO_USER_01
#define HANDOVER_FREEZE 0x46524545u   // "FREE"
#define HANDOVER_DONE   0x444F4E45u   // "DONE"

static volatile bool exit_loop = false;
static volatile bool freeze_requested = false;
static volatile bool frozen = false;

static void power_button_callback(void)
{
    exit_loop = true;
}

static void vblank_handler(void)
{
    inputGetAndSend();
    if (!frozen)
        Wifi_Update();
}

static void handover_handler(u32 value, void *userdata)
{
    (void)userdata;
    if (value == HANDOVER_FREEZE)
        freeze_requested = true;
}

// Runs in the main loop, so no DSWiFi IRQ or timer handler can be in the
// middle of an SDIO transfer: they run to completion before we get here.
static void freeze_wifi(void)
{
    int old = enterCriticalSection();
    REG_AUXIE &= ~IRQ_SDIO_DATA;                  // DSWiFi's card IRQ
    timerStop(LIBNDS_DEFAULT_TIMER_WIFI);         // DSWiFi's 1 ms tick
    fifoSetValue32Handler(FIFO_DSWIFI, NULL, NULL);
    fifoSetAddressHandler(FIFO_DSWIFI, NULL, NULL);
    frozen = true;
    leaveCriticalSection(old);
    fifoSendValue32(FIFO_HANDOVER, HANDOVER_DONE);
}

int main(void)
{
    // Started straight from the DSi Menu (the hiyaCFW icon), the ARM7 gets the
    // SCFG_EXT7 of our header (0x80040407: no NDMA, no NWRAM, ...). Started from
    // TWiLight it already has everything on. nds-bootstrap's first stage relies
    // on the starter for this and only sets it itself much later, so its SD
    // access hangs before the first log line. Turn everything on like TWiLight
    // (the value nds-bootstrap's bootloader uses too), while we still may.
    if (isDSiMode() && (REG_SCFG_EXT & BIT(31)))
        REG_SCFG_EXT = 0x93FFFB06;

    enableSound();
    readUserSettings();
    ledBlink(0);
    touchInit();

    irqInit();
    fifoInit();
    installWifiFIFO();
    installSoundFIFO();
    installSystemFIFO();
    if (isDSiMode())
        installCameraFIFO();
    fifoSetValue32Handler(FIFO_HANDOVER, handover_handler, NULL);

    setPowerButtonCB(power_button_callback);
    initClockIRQTimer(LIBNDS_DEFAULT_TIMER_RTC);

    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);

    while (!exit_loop)
    {
        if (freeze_requested && !frozen)
            freeze_wifi();
        swiWaitForVBlank();
    }
    return 0;
}
