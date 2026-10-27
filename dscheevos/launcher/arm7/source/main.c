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

// The SCFG state we were started with, then after setup(), for the ARM9's
// log (FIFO_USER_02, in this order).
#define FIFO_SCFG_REPORT FIFO_USER_02
static u32 scfg_report[8];

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

// Started straight from the DSi Menu (the hiyaCFW icon), we get the ARM7
// SCFG state the menu leaves for a DSiWare title. Started from TWiLight Menu++
// we get the state its first program ("booter") sets up. nds-bootstrap's first
// stage relies on the latter: its ARM7 reads the AES unit (to find the console
// ID) before it answers the ARM9, so without the AES clock both CPUs wait for
// ever, before NDSBTSRP.LOG is even created. Do what TWiLight's booter does
// (booter/arm7/source/main.c), while SCFG is still writable.
static void setup_scfg_like_twilight(void)
{
    scfg_report[0] = REG_SCFG_ROM;
    scfg_report[1] = REG_SCFG_CLK;
    scfg_report[2] = REG_SCFG_EXT;
    scfg_report[3] = REG_SCFG_MC;
    scfg_report[4] = ((u32)REG_SCFG_CARD_INSERT_DELAY << 16) | REG_SCFG_CARD_PWROFF_DELAY;
    if (REG_SCFG_EXT & SCFG_EXT_SCFG_MBK_REG) {
        REG_SCFG_CLK = 0x187;                   // SD/MMC, AES, NWRAM, touch clocks
        REG_SCFG_EXT = 0x93FFFB06;              // NDMA, AES, SDIO, NWRAM, ... on
        REG_SCFG_CARD_INSERT_DELAY = SCFG_CARD_INSERT_DELAY_DEFAULT;
        REG_SCFG_CARD_PWROFF_DELAY = SCFG_CARD_PWROFF_DELAY_DEFAULT;
        *(vu32*)0x0400481C = 0;                 // SD/MMC IRQ status
        *(vu32*)0x04004820 = 0;                 // SD/MMC IRQ mask
        *(vu16*)0x04004C02 = 0x4000;            // GPIO IRQ: power button
    }
    scfg_report[5] = REG_SCFG_CLK;
    scfg_report[6] = REG_SCFG_EXT;
    scfg_report[7] = ((u32)REG_SCFG_CARD_INSERT_DELAY << 16) | REG_SCFG_CARD_PWROFF_DELAY;
}

int main(void)
{
    if (isDSiMode())
        setup_scfg_like_twilight();

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
    if (isDSiMode())
        for (int i = 0; i < 8; i++)
            fifoSendValue32(FIFO_SCFG_REPORT, scfg_report[i]);

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
