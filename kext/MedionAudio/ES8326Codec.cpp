#include "ES8326Codec.hpp"
#include <IOKit/IOLib.h>
#include <string.h>

/* Register helper macros */
static inline UInt32 rd32(volatile UInt8 *b, UInt32 o) { return *(volatile UInt32*)(b + o); }
static inline void   wr32(volatile UInt8 *b, UInt32 o, UInt32 v) { *(volatile UInt32*)(b + o) = v; }

ES8326Codec::ES8326Codec()
    : voodooNub(nullptr), gpioMap(nullptr), gpioBase(nullptr),
      gpioAmpMap(nullptr), gpioAmpBase(nullptr),
      version(0), calibrated(false), captureMicHeadset(false), ioError(false) {
    statusMsg[0] = '\0';
}

ES8326Codec::~ES8326Codec() {
    free();
}

void ES8326Codec::free() {
    if (voodooNub) {
        voodooNub->release();
        voodooNub = nullptr;
    }
    if (gpioMap) {
        gpioMap->release();
        gpioMap = nullptr;
    }
    gpioBase = nullptr;
    if (gpioAmpMap) {
        gpioAmpMap->release();
        gpioAmpMap = nullptr;
    }
    gpioAmpBase = nullptr;
}

bool ES8326Codec::init() {
    version = 0;
    calibrated = false;
    captureMicHeadset = false;

    /* 1. Locate the VoodooI2CDeviceNub that wraps ESSX (I2C4 @ 0x19). */
    if (initVoodooNub()) {
        snprintf(statusMsg, sizeof(statusMsg), "ES8326 @0x19 via VoodooI2CDeviceNub");
        IOLog("MedionAudio: ES8326 initialized via VoodooI2CDeviceNub\n");
    } else {
        snprintf(statusMsg, sizeof(statusMsg), "VoodooI2CDeviceNub not matched yet");
        IOLog("MedionAudio: VoodooI2CDeviceNub not yet matched, will retry\n");
    }

    /* 2. Map GPIO communities (community 1 carries speakers-enable). */
    initGPIO();
    /* 3. Leave the external speaker amp off and the DAC muted. */
    silenceAmps();

    return (voodooNub != nullptr);
}

bool ES8326Codec::initVoodooNub() {
    if (voodooNub) return true;

    OSDictionary *match = IOService::serviceMatching("VoodooI2CDeviceNub");
    if (!match) return false;

    OSIterator *iter = IOService::getMatchingServices(match);
    match->release();
    if (!iter) return false;

    VoodooI2CDeviceNub *found = nullptr;
    IOService *svc = nullptr;
    while ((svc = OSDynamicCast(IOService, iter->getNextObject()))) {
        bool isEs8326 = false;
        const char *svcName = svc->getName();
        if (svcName && (strcmp(svcName, "ESSX") == 0 ||
                        strcmp(svcName, "ESSX8326") == 0))
            isEs8326 = true;

        OSObject *nameProp = svc->getProperty("IOName");
        if (!nameProp) nameProp = svc->getProperty("name");
        if (nameProp) {
            if (OSString *str = OSDynamicCast(OSString, nameProp)) {
                if (str->isEqualTo("ESSX8326") || str->isEqualTo("ESSX"))
                    isEs8326 = true;
            } else if (OSData *data = OSDynamicCast(OSData, nameProp)) {
                const char *bytes = (const char *)data->getBytesNoCopy();
                if (bytes && data->getLength() >= 4 && strncmp(bytes, "ESSX", 4) == 0)
                    isEs8326 = true;
            }
        }

        OSObject *compProp = svc->getProperty("compatible");
        if (compProp) {
            if (OSString *str = OSDynamicCast(OSString, compProp)) {
                if (str->isEqualTo("ESSX8326") || str->isEqualTo("ESSX"))
                    isEs8326 = true;
            } else if (OSData *data = OSDynamicCast(OSData, compProp)) {
                const char *bytes = (const char *)data->getBytesNoCopy();
                if (bytes && data->getLength() >= 4 && strncmp(bytes, "ESSX", 4) == 0)
                    isEs8326 = true;
            }
        }

        OSNumber *addr = OSDynamicCast(OSNumber, svc->getProperty("i2cAddress"));
        if (addr && addr->unsigned16BitValue() == ES8326_I2C_ADDR)
            isEs8326 = true;

        if (isEs8326) {
            found = (VoodooI2CDeviceNub *)svc;
            found->retain();
            break;
        }
    }
    iter->release();

    if (found) {
        voodooNub = found;
        return true;
    }
    return false;
}

bool ES8326Codec::initGPIO() {
    if (gpioAmpBase) return true;

    /* GPI0 ACPI platform device (INT34BB on Comet Lake PCH-LP). */
    OSDictionary *match = IOService::serviceMatching("IOACPIPlatformDevice");
    if (!match) return false;

    OSIterator *iter = IOService::getMatchingServices(match);
    match->release();
    if (!iter) return false;

    IOService *svc = nullptr;
    while ((svc = OSDynamicCast(IOService, iter->getNextObject()))) {
        bool isGpi = false;
        const char *svcName = svc->getName();
        if (svcName && (strcmp(svcName, "GPI0") == 0 ||
                        strcmp(svcName, "INT34BB") == 0))
            isGpi = true;

        OSObject *nameProp = svc->getProperty("name");
        if (nameProp) {
            if (OSString *str = OSDynamicCast(OSString, nameProp)) {
                if (str->isEqualTo("GPI0") || str->isEqualTo("INT34BB"))
                    isGpi = true;
            } else if (OSData *data = OSDynamicCast(OSData, nameProp)) {
                const char *bytes = (const char *)data->getBytesNoCopy();
                unsigned int len = data->getLength();
                if (bytes && len >= 7 && strncmp(bytes, "INT34BB", 7) == 0)
                    isGpi = true;
                if (bytes && len >= 4 && strncmp(bytes, "GPI0", 4) == 0)
                    isGpi = true;
            }
        }

        if (isGpi) {
            /* Index 0 = Community 0 @ 0xFD6E0000.
             * Index 1 = Community 1 @ 0xFD6D0000 (speakers-enable, pad 8). */
            gpioMap = svc->mapDeviceMemoryWithIndex(0);
            if (gpioMap) {
                gpioBase = (volatile UInt8 *)gpioMap->getVirtualAddress();
                if (!gpioBase) {
                    gpioMap->release();
                    gpioMap = nullptr;
                } else {
                    IOLog("MedionAudio: GPIO Community 0 mapped from ACPI %s at %p\n",
                          svcName ? svcName : "GPI0", gpioBase);
                }
            }
            gpioAmpMap = svc->mapDeviceMemoryWithIndex(1);
            if (gpioAmpMap) {
                gpioAmpBase = (volatile UInt8 *)gpioAmpMap->getVirtualAddress();
                if (!gpioAmpBase) {
                    gpioAmpMap->release();
                    gpioAmpMap = nullptr;
                } else {
                    IOLog("MedionAudio: GPIO Community 1 (speakers-enable) mapped from ACPI %s at %p\n",
                          svcName ? svcName : "GPI0", gpioAmpBase);
                }
            }
            if (gpioBase || gpioAmpBase)
                break;
        }
    }
    iter->release();

    if (!gpioBase) {
        IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(0xFD6E0000ULL, 0x10000, kIODirectionInOut);
        if (desc) {
            gpioMap = desc->map();
            desc->release();
            if (gpioMap) {
                gpioBase = (volatile UInt8 *)gpioMap->getVirtualAddress();
                if (!gpioBase) { gpioMap->release(); gpioMap = nullptr; }
                else IOLog("MedionAudio: GPIO Community 0 fallback 0xFD6E0000 at %p\n", gpioBase);
            }
        }
    }
    if (!gpioAmpBase) {
        IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(0xFD6D0000ULL, 0x10000, kIODirectionInOut);
        if (desc) {
            gpioAmpMap = desc->map();
            desc->release();
            if (gpioAmpMap) {
                gpioAmpBase = (volatile UInt8 *)gpioAmpMap->getVirtualAddress();
                if (!gpioAmpBase) { gpioAmpMap->release(); gpioAmpMap = nullptr; }
                else IOLog("MedionAudio: GPIO Community 1 fallback 0xFD6D0000 at %p\n", gpioAmpBase);
            }
        }
    }

    if (!gpioAmpBase)
        IOLog("MedionAudio: FAILED to map GPIO Community 1 (speaker amp)\n");
    if (!gpioBase)
        IOLog("MedionAudio: GPIO Community 0 not mapped (only used by diagnostics)\n");

    return (gpioAmpBase != nullptr);
}

IOByteCount ES8326Codec::getGpioLength() const {
    if (!gpioMap) return 0;
    return gpioMap->getLength();
}

IOByteCount ES8326Codec::getGpioAmpLength() const {
    if (!gpioAmpMap) return 0;
    return gpioAmpMap->getLength();
}

UInt32 ES8326Codec::ampPadcfgOffset() {
    /* PADBAR (community + 0x0C) is the offset of pad 0 inside the community.
     * GPCL[3] says community 1 (0xFD6D0000), 24 pins, PADBAR 0x0600.
     * speakers-enable = group 3 pin 8 -> pad 8 -> 0x600 + 8*16 = 0x680. */
    UInt32 padbar = MEDION_SPK_PADBAR_FALLBACK;
    if (gpioAmpBase) {
        UInt32 v = rd32(gpioAmpBase, MEDION_PADBAR_REG) & 0xFFFFU;
        if (v >= 0x100U && v < 0x8000U) padbar = v;
    }
    return padbar + MEDION_SPK_PAD * MEDION_PAD_STRIDE;
}

void ES8326Codec::setAmpPadcfg(bool on) {
    if (!gpioAmpBase) return;

    UInt32 padcfg0 = ampPadcfgOffset();
    IOByteCount len = getGpioAmpLength();
    if (len == 0 || (padcfg0 + 4) > len) return;

    /* TXSTATE only: Linux live playback reports raw high, idle raw low.
     * The active-low GPIO descriptor is inverted again by the machine driver. */
    UInt32 oldVal = rd32(gpioAmpBase, padcfg0);
    UInt32 val = (oldVal & ~1U) | (on ? 1U : 0U);
    wr32(gpioAmpBase, padcfg0, val);
    UInt32 verify = rd32(gpioAmpBase, padcfg0);
    IOLog("MedionAudio: speaker amp pad C1/8 @0x%x on=%d: 0x%08x -> wr 0x%08x rd 0x%08x\n",
          padcfg0, on ? 1 : 0, oldVal, val, verify);
}

bool ES8326Codec::writeReg(UInt8 reg, UInt8 val) {
    if (voodooNub) {
        UInt8 buf[2] = { reg, val };
        IOReturn ret = voodooNub->writeI2C(buf, 2);
        if (ret == kIOReturnSuccess) return true;
    }
    ioError = true;
    return false;
}

bool ES8326Codec::readRegChecked(UInt8 reg, UInt8 *val) {
    if (voodooNub && val &&
        voodooNub->writeReadI2C(&reg, 1, val, 1) == kIOReturnSuccess) return true;
    ioError = true;
    return false;
}

UInt8 ES8326Codec::readReg(UInt8 reg) {
    UInt8 val = 0;
    readRegChecked(reg, &val);
    return val;
}

bool ES8326Codec::updateBits(UInt8 reg, UInt8 mask, UInt8 val) {
    UInt8 old = 0;
    if (!readRegChecked(reg, &old)) return false;
    return writeReg(reg, (UInt8)((old & ~mask) | (val & mask)));
}

bool ES8326Codec::probe() {
    if (!voodooNub && !initVoodooNub()) return false;
    UInt8 id1 = readReg(ES8326_CHIP_ID1);
    UInt8 id2 = readReg(ES8326_CHIP_ID2);
    UInt8 ver = readReg(ES8326_CHIP_VERSION);
    IOLog("MedionAudio: ES8326 probe: ID1=0x%02x ID2=0x%02x VERSION=0x%02x\n", id1, id2, ver);
    return (voodooNub != nullptr);
}

void ES8326Codec::enableMicbias() {
    /* MICBIAS1 (bit2) + MICBIAS2 (bit3), active high. */
    updateBits(ES8326_ANA_MICBIAS, (1 << 2) | (1 << 3), (1 << 2) | (1 << 3));
}

void ES8326Codec::disableMicbias() {
    updateBits(ES8326_ANA_MICBIAS, (1 << 2) | (1 << 3), 0);
}

void ES8326Codec::calibrate() {
    UInt8 reg = readReg(ES8326_CHIP_VERSION);
    version = reg;
    IOLog("MedionAudio: ES8326 chip version 0x%02x\n", version);

    if (version >= ES8326_VERSION_B && !calibrated) {
        writeReg(ES8326_CLK_INV, 0xc0);
        writeReg(ES8326_CLK_DIV1, 0x03);
        writeReg(ES8326_CLK_DLL, 0x30);
        writeReg(ES8326_CLK_MUX, 0xed);
        writeReg(ES8326_CLK_DAC_SEL, 0x08);
        writeReg(ES8326_CLK_TRI, 0xc1);
        writeReg(ES8326_DAC_MUTE, 0x03);
        writeReg(ES8326_ANA_VSEL, 0x7f);
        writeReg(ES8326_VMIDLOW, 0x23);
        writeReg(ES8326_DAC2HPMIX, 0x88);
        IOSleep(18);
        writeReg(ES8326_HP_OFFSET_CAL, 0x8c);
        IOSleep(18);
        writeReg(ES8326_RESET, 0xc0);
        IOSleep(18);

        writeReg(ES8326_HP_OFFSET_CAL, ES8326_HP_OFF);
        UInt8 muteSta = readReg(ES8326_CSM_MUTE_STA);
        if ((muteSta & 0xf0) != 0x40) IOSleep(50);

        writeReg(ES8326_HP_CAL, 0xd4);
        IOSleep(200);
        writeReg(ES8326_HP_CAL, 0x4d);
        IOSleep(200);
        writeReg(ES8326_HP_CAL, ES8326_HP_OFF);
        UInt8 offL = readReg(ES8326_HPL_OFFSET_INI);
        UInt8 offR = readReg(ES8326_HPR_OFFSET_INI);
        writeReg(ES8326_HP_OFFSET_CAL, 0x8c);
        writeReg(ES8326_HPL_OFFSET_INI, offL);
        writeReg(ES8326_HPR_OFFSET_INI, offR);
        writeReg(ES8326_CLK_INV, 0x00);
        calibrated = true;
        IOLog("MedionAudio: ES8326 headphone calibration done (offL=0x%02x offR=0x%02x)\n",
              offL, offR);
    }
}

bool ES8326Codec::configure() {
    if (!voodooNub && !initVoodooNub()) return false;
    ioError = false;

    IOLog("MedionAudio: Starting ES8326 initialization sequence\n");

    /* Linux sound/soc/codecs/es8326.c:es8326_init(). */
    writeReg(ES8326_RESET, 0x1f);
    writeReg(ES8326_VMIDSEL, 0x3e);
    writeReg(ES8326_ANA_LP, 0xf0);
    IOSleep(12);
    writeReg(ES8326_HPJACK_TIMER, 0xd9);
    writeReg(ES8326_ANA_MICBIAS, 0xd8);
    writeReg(ES8326_HPDET_TYPE, 0x83);
    writeReg(ES8326_CLK_RESAMPLE, 0x05);
    writeReg(ES8326_CLK_DIV_CPC, 0x89);
    writeReg(ES8326_CLK_CTL, ES8326_CLK_ON);
    writeReg(ES8326_RESET, 0x17);
    writeReg(ES8326_HP_MISC, 0x3d);
    writeReg(ES8326_PULLUP_CTL, 0x00);
    writeReg(ES8326_HP_VOL, 0xc4);
    writeReg(ES8326_HP_DRIVER, 0xa7);
    IOSleep(3);
    writeReg(ES8326_HP_DRIVER_REF, 0x23);
    writeReg(ES8326_HP_DRIVER_REF, 0x33);
    writeReg(ES8326_HP_DRIVER, 0xa1);
    writeReg(ES8326_CLK_INV, 0x00);
    writeReg(ES8326_CLK_VMIDS1, 0xc4);
    writeReg(ES8326_CLK_VMIDS2, 0x81);
    writeReg(ES8326_CLK_CAL_TIME, 0x00);

    /* Read CHIP_VERSION and run the version-B calibration. */
    calibrate();

    writeReg(ES8326_DAC_CROSSTALK, 0xaa);
    writeReg(ES8326_DAC_RAMPRATE, 0x00);
    writeReg(ES8326_HP_CAL, 0x00);
    writeReg(ES8326_ANA_LP, 0xf0);
    writeReg(ES8326_ANA_VSEL, 0x7f);
    writeReg(ES8326_VMIDLOW, 0x03);
    writeReg(ES8326_DAC_DSM, 0x08);
    writeReg(ES8326_DAC_VPPSCALE, 0x15);

    /* Jack detection: pin 9, auto OMTP/CTIA. */
    UInt8 hptype = (UInt8)(0x80 | ES8326_HP_DET_SRC_PIN9 | ES8326_HP_TYPE_AUTO);
    if (version < ES8326_VERSION_B) hptype |= 0x04;
    writeReg(ES8326_HPDET_TYPE, hptype);
    IOSleep(7);
    enableMicbias();
    IOSleep(60);
    updateBits(ES8326_HPDET_TYPE, 0x03, 0x00);
    writeReg(ES8326_INTOUT_IO, 0x00);
    writeReg(ES8326_SDINOUT1_IO, ES8326_IO_INPUT);
    writeReg(ES8326_SDINOUT23_IO, ES8326_IO_INPUT);
    writeReg(ES8326_ANA_PDN, 0x00);
    writeReg(ES8326_RESET, ES8326_CSM_ON);
    updateBits(ES8326_PGAGAIN, ES8326_MIC_SEL_MASK, ES8326_MIC1_SEL);
    updateBits(ES8326_DAC_MUTE, ES8326_MUTE_MASK, ES8326_MUTE);
    writeReg(ES8326_ADC_MUTE, 0x0f);
    writeReg(ES8326_CLK_DIV_LRCK, 0xff);
    writeReg(ES8326_ADC1_SRC, 0x44);
    writeReg(ES8326_ADC2_SRC, 0x66);
    disableMicbias();
    if (version > ES8326_VERSION_B) {
        updateBits(ES8326_ANA_MICBIAS, 0x73, 0x10);
        updateBits(ES8326_VMIDSEL, 0x40, 0x40);
    }
    IOSleep(200);
    writeReg(ES8326_INT_SOURCE, ES8326_INT_SRC_PIN9);

    /* Linux selects coeff_div_v0 for version 0, v3 for later revisions.
     * These are the coefficients for the Linux codec sysclk of 19.2 MHz;
     * the physical SSP MCLK and its topology still need verification. */
    writeReg(ES8326_CLK_DIV1,     version == 0 ? 0xE9 : 0xE4);
    writeReg(ES8326_CLK_DIV2,     0x04);
    writeReg(ES8326_CLK_DLL,      version == 0 ? 0x0F : 0x35);
    writeReg(ES8326_CLK_MUX,      0x6d);
    writeReg(ES8326_CLK_ADC_SEL,  version == 0 ? 0x4A : 0xCA);
    writeReg(ES8326_CLK_DAC_SEL,  0x0A);
    writeReg(ES8326_CLK_ADC_OSR,  0x1F);
    writeReg(ES8326_CLK_DAC_OSR,  0x1F);

    /* I2S, BCLK/FS slave (SSP0 is master), 24-bit samples. */
    writeReg(ES8326_FMT, ES8326_S24_LE);

    /* Analog capture sources: MIC1/MIC2 (not DMIC), keep ADC muted for now. */
    writeReg(ES8326_ADC1_SRC, 0x00);
    writeReg(ES8326_ADC2_SRC, 0x00);
    updateBits(ES8326_PGAGAIN, ES8326_MIC_SEL_MASK, ES8326_MIC1_SEL);
    updateBits(ES8326_PGAGAIN, 0x0f, 0x04);       /* +12 dB analog PGA */
    writeReg(ES8326_ADC1_VOL, 0xbf);              /* 0 dB digital */
    writeReg(ES8326_ADC2_VOL, 0xbf);

    /* Enable both mixers and raise DAC gain 12 dB over the observed Linux
     * playback setting (0x97), while retaining 8 dB of codec headroom. */
    writeReg(ES8326_DAC2HPMIX, 0x99);
    writeReg(ES8326_DACL_VOL, 0xaf);

    /* Do not enable an output if the initialization failed. */
    if (ioError) {
        setAmpPadcfg(false);
        IOLog("MedionAudio: ES8326 initialization failed; speaker amp left off\n");
        return false;
    }
    /* Bring the DAC/HP driver up and route to the internal speakers. */
    playbackUnmute();
    if (ioError) {
        setAmpPadcfg(false);
        return false;
    }
    setSpeakerEnable(!isHeadphoneInserted());

    IOLog("MedionAudio: ES8326 configure done (19.2 MHz/48k, I2S slave, ver=0x%02x)\n", version);
    return true;
}

void ES8326Codec::playbackUnmute() {
    if (!calibrated) {
        writeReg(ES8326_HP_CAL, ES8326_HP_FORCE_CAL);
        IOSleep(30);
        writeReg(ES8326_HP_CAL, ES8326_HP_OFF);
        UInt8 offL = readReg(ES8326_HPL_OFFSET_INI);
        UInt8 offR = readReg(ES8326_HPR_OFFSET_INI);
        writeReg(ES8326_HP_OFFSET_CAL, 0x8c);
        writeReg(ES8326_HPL_OFFSET_INI, offL);
        writeReg(ES8326_HPR_OFFSET_INI, offR);
        calibrated = true;
    }

    updateBits(ES8326_CLK_INV, 0xc0, 0x00);
    updateBits(ES8326_CLK_MUX, 0x80, 0x00);

    updateBits(ES8326_DAC_DSM, 0x01, 0x01);
    IOSleep(2);
    updateBits(ES8326_DAC_DSM, 0x01, 0x00);
    IOSleep(2);

    updateBits(ES8326_HP_DRIVER_REF, 0x30, 0x20);
    updateBits(ES8326_HP_DRIVER_REF, 0x30, 0x30);
    writeReg(ES8326_HP_DRIVER, 0xa1);
    writeReg(ES8326_HP_CAL, ES8326_HP_ON);
    updateBits(ES8326_DAC_MUTE, ES8326_MUTE_MASK, 0x00);
}

void ES8326Codec::mute(bool muted) {
    if (muted) {
        writeReg(ES8326_HP_CAL, ES8326_HP_OFF);
        updateBits(ES8326_DAC_MUTE, ES8326_MUTE_MASK, ES8326_MUTE);
        updateBits(ES8326_HP_DRIVER_REF, 0x30, 0x00);
    } else {
        playbackUnmute();
    }
}

bool ES8326Codec::isHeadphoneInserted() {
    UInt8 sta = 0;
    if (!readRegChecked(ES8326_HPDET_STA, &sta)) return true; /* amp off on error */
    return ((sta & ES8326_HPINSERT_FLAG) != 0);
}

bool ES8326Codec::isHeadsetMicPresent() {
    UInt8 sta = 0;
    if (!readRegChecked(ES8326_HPDET_STA, &sta)) return false;
    if ((sta & ES8326_HPINSERT_FLAG) == 0) return false;
    /* Bit 0: 0 = 4-pole headset (mic), 1 = plain headphones. */
    return ((sta & ES8326_HPBUTTON_FLAG) == 0);
}

void ES8326Codec::setSpeakerEnable(bool enable) {
    setAmpPadcfg(enable);
    IOLog("MedionAudio: Audio route -> %s (C1 pad8 TXSTATE raw=%d)\n",
          enable ? "Internal Speakers" : "Headphones", enable ? 1 : 0);
}

void ES8326Codec::silenceAmps() {
    setAmpPadcfg(false);
    mute(true);
    IOLog("MedionAudio: Amps silenced (speaker amp OFF, DAC muted)\n");
}

void ES8326Codec::setCaptureMic(bool headset) {
    captureMicHeadset = headset;

    /* Linux keeps MIC1_SEL for both sources. Selecting MIC2_SEL here makes
     * headset capture almost silent on this version-0 codec. */
    updateBits(ES8326_PGAGAIN, ES8326_MIC_SEL_MASK, ES8326_MIC1_SEL);
    updateBits(ES8326_PGAGAIN, 0x0f, 0x04);   /* +12 dB analog PGA */
    writeReg(ES8326_ADC1_SRC, 0x00);          /* analog mic, no inversion */
    writeReg(ES8326_ADC2_SRC, 0x00);

    /* Linux jack handler: on a 4-pole insert it toggles PGA_PDN bit 3 and
     * restores SYS_BIAS / HP_DRIVER_REF for the headset mic path. */
    if (headset) {
        updateBits(ES8326_PGA_PDN, 0x08, 0x08);
        IOSleep(10);
        updateBits(ES8326_PGA_PDN, 0x08, 0x00);
        writeReg(ES8326_SYS_BIAS, 0x1f);
        updateBits(ES8326_HP_DRIVER_REF, 0x0f, 0x0d);
    }

    enableMicbias();
    setCaptureMute(false);

    IOLog("MedionAudio: capture mic -> %s (PGAGAIN MIC1_SEL)\n",
          headset ? "Headset MIC2" : "Internal MIC1");
}

void ES8326Codec::setCaptureMute(bool muted) {
    if (muted) {
        updateBits(ES8326_ADC_MUTE, 0x0f, 0x0f);
        if (version > ES8326_VERSION_B) {
            updateBits(ES8326_VMIDSEL, 0x40, 0x40);
            updateBits(ES8326_ANA_MICBIAS, 0x70, 0x30);
        }
        updateBits(ES8326_ADC_HPFS1, 0x0f, 0x04);
        updateBits(ES8326_ADC_HPFS2, 0x0f, 0x04);
    } else {
        /* Linux es8326_mute() waits 300 ms, then applies the HPF coefficients
         * (default 8 -> 0x08) and unmutes the ADC. */
        IOSleep(300);
        if (version > ES8326_VERSION_B) {
            updateBits(ES8326_ANA_MICBIAS, 0x70, 0x70);
            updateBits(ES8326_VMIDSEL, 0x40, 0x00);
        }
        IOSleep(40);
        updateBits(ES8326_ADC_HPFS1, 0x0f, 0x08);
        updateBits(ES8326_ADC_HPFS2, 0x0f, 0x08);
        writeReg(ES8326_ADC_MUTE, 0x00);
    }
}
