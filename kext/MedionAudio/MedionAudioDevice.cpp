#include "MedionAudioDevice.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <libkern/libkern.h>

// Kext module entry
extern "C" kern_return_t MedionAudio_start(kmod_info_t *ki, void *data);
extern "C" kern_return_t MedionAudio_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.hackintosh.MedionAudio, "1.0.0", MedionAudio_start, MedionAudio_stop)
kern_return_t MedionAudio_start(kmod_info_t *ki, void *data) { return KERN_SUCCESS; }
kern_return_t MedionAudio_stop(kmod_info_t *ki, void *data) { return KERN_SUCCESS; }

// Embedded firmware
extern "C" const unsigned char sof_fw_data[];
extern "C" const unsigned long long sof_fw_size;

// ========== HDA / DSP Register Definitions ==========

#define HDA_GCAP            0x00
#define HDA_GCTL            0x08
#define HDA_INTCTL          0x20
#define SD_BASE             0x80
#define SD_SIZE             0x20
#define SD_CTL_SRST         0x01
#define SD_CTL_RUN          0x02
#define SD_CTL_IOCE         0x04
#define SD_REG_STS          0x03
#define SD_REG_CBL          0x08
#define SD_REG_LVI          0x0C
#define SD_REG_FMT          0x12
#define SD_REG_BDLPL        0x18
#define SD_REG_BDLPU        0x1C
#define HDA_CL_STREAM_FMT   0x40

#define HDA_CAP_ML_ID       0x02
#define HDA_CAP_PP_ID       0x03
#define HDA_CAP_SPIB_ID     0x04
#define PP_PPCTL            0x04
#define PP_PPCTL_GPROCEN    (1U << 30)

#define DSP_ADSPCS          0x04
#define DSP_ADSPIC          0x08
#define ADSPCS_CRST(cm)     ((cm) << 0)
#define ADSPCS_CSTALL(cm)   ((cm) << 8)
#define ADSPCS_SPA(cm)      ((cm) << 16)
#define ADSPCS_CPA(cm)      ((cm) << 24)

#define IPC_HIPCTDR         0xC0
#define IPC_HIPCTDA         0xC4
#define IPC_HIPCIDR         0xD0
#define IPC_HIPCIDA         0xD4
#define IPC_HIPCCTL         0xE8
#define IPC_BUSY            (1U << 31)
#define IPC_DONE            (1U << 31)

#define HDA_VS_SDXDPIB_XBASE    0x1084
#define HDA_VS_SDXDPIB_XINTERVAL 0x20
#define HDA_VS_EM2          0x1030
#define PCI_TCSEL           0x44

#define SRAM_WIN(n)         (0x80000 + (n) * 0x20000)
#define MBOX_UPLINK         0x81000
#define ROM_STATUS          0x80000
#define FSR_FW_ENTERED      0x5
#define FSR_INIT_DONE       0x1
#define ROM_IPC_CONTROL     0x01000000
#define ROM_IPC_PURGE_FW    0x00004000
#define SOF_IPC_FW_READY    0x70000000
#define SOF_IPC_EXT_WINDOW  1
#define SOF_IPC_REGION_DOWNBOX  0
#define SOF_IPC_REGION_UPBOX    1

struct sof_ipc_host_buffer {
    UInt32 hdr_size;
    UInt32 phy_addr;
    UInt32 pages;
    UInt32 size;
    UInt32 reserved[3];
} __attribute__((packed));

struct sof_ipc_pcm_params {
    UInt32 hdr_size;
    UInt32 hdr_cmd;
    UInt32 comp_id;
    UInt32 flags;
    UInt32 reserved[2];
    UInt32 params_size;
    struct sof_ipc_host_buffer buffer;
    UInt32 direction;
    UInt32 frame_fmt;
    UInt32 buffer_fmt;
    UInt32 rate;
    UInt16 stream_tag;
    UInt16 channels;
    UInt16 sample_valid_bytes;
    UInt16 sample_container_bytes;
    UInt32 host_period_bytes;
    UInt16 no_stream_position;
    UInt8  cont_update_posn;
    UInt8  reserved0;
    SInt16 ext_data_length;
    UInt8  reserved1[2];
    UInt16 chmap[8];
} __attribute__((packed));

#define COMP_HOST    1
#define COMP_DAI     2
#define COMP_BUFFER  12  /* SOF_COMP_BUFFER under FILEREAD/WRITE ABI; Linux kprobe uses 0xc */
#define FRAME_S16    0
#define FRAME_S24    1
#define FRAME_S32    2
#define DAI_SSP      1
#define DAI_DMIC     2
#define DIR_PLAYBACK 0
#define DIR_CAPTURE  1
#define TIME_TIMER   1
#define XRUN_STOP    1
#define XRUN_UNDER_ZERO 2
/* sof_ipc_dai_config.flags — clocks start on HW_PARAMS (DMIC proven pattern). */
#define SOF_DAI_CONFIG_FLAGS_NONE      0
#define SOF_DAI_CONFIG_FLAGS_HW_PARAMS 0x01
#define SOF_DAI_CONFIG_FLAGS_HW_FREE   0x02
/* Linux Live topology specifies a 24 MHz SSP MCLK; the machine driver
 * separately configures a 19.2 MHz codec sysclk. Preserve both values.
 * bclk = 48000 * 2 slots * 25 bits = 2.4 MHz. */
#define SSP_GOLD_MCLK       24000000U
#define SSP_GOLD_BCLK       2400000U
#define SSP_GOLD_BITS       24U
#define SSP_GOLD_SLOTW      25U
#define SSP_CLKS_CONTROL    0U
#define SSP_PARAMS_SIZE     60U   /* sizeof(struct sof_ipc_dai_ssp_params) */
/* SOF_DAI_FMT_I2S | SOF_DAI_FMT_CBC_CFC (codec is BCLK/FS slave). */
#define SOF_DAI_FMT_I2S_CBC_CFC        0x4001

// Pipeline 1: SSP0 playback (Everest ES8326 codec)
#define PIPE1_SCHED_ID   5
#define PIPE1_HOST_ID    0
#define PIPE1_BUF0_ID    2
#define PIPE1_DAI_ID     4
#define PIPE1_ID         1

// Pipeline 2: SSP0.IN capture (ES8326 ADC, MIC1 internal or MIC2 headset)
#define PIPE2_SCHED_ID   20
#define PIPE2_HOST_ID    21
#define PIPE2_BUF0_ID    22
#define PIPE2_DAI_ID     23
#define PIPE2_ID         2

struct HdaBdlEntry {
    UInt32 addrLow, addrHigh, size, ioc;
} __attribute__((packed));

OSDefineMetaClassAndStructors(MedionAudioDevice, IOService)

static inline UInt32 rd32(volatile UInt8 *b, UInt32 o) { return *(volatile UInt32*)(b + o); }
static inline void   wr32(volatile UInt8 *b, UInt32 o, UInt32 v) { *(volatile UInt32*)(b + o) = v; }
static inline UInt16 rd16(volatile UInt8 *b, UInt32 o) { return *(volatile UInt16*)(b + o); }
static inline void   wr16(volatile UInt8 *b, UInt32 o, UInt16 v) { *(volatile UInt16*)(b + o) = v; }
static inline UInt8  rd8(volatile UInt8 *b, UInt32 o)  { return *(volatile UInt8*)(b + o); }
static inline void   wr8(volatile UInt8 *b, UInt32 o, UInt8 v)  { *(volatile UInt8*)(b + o) = v; }

static bool poll32(volatile UInt8 *b, UInt32 o, UInt32 mask, UInt32 val, UInt32 usec) {
    for (UInt32 t = 0; t < usec; t += 500) {
        if ((rd32(b, o) & mask) == val) return true;
        IODelay(500);
    }
    return false;
}

static void streamReset(volatile UInt8 *hda, UInt32 sd) {
    wr8(hda, sd, 0); wr8(hda, sd + 2, 0); IODelay(100);
    wr8(hda, sd, SD_CTL_SRST);
    for (int t = 0; t < 300; t++) { if (rd8(hda, sd) & SD_CTL_SRST) break; IODelay(10); }
    wr8(hda, sd, 0);
    for (int t = 0; t < 300; t++) { if (!(rd8(hda, sd) & SD_CTL_SRST)) break; IODelay(10); }
    wr8(hda, sd + SD_REG_STS, 0x1C);
}

/* ACK DSP→host IPC (HIPCT). Unacked STREAM_POSN stalls HIPCIDR and capture. */
static void drainDspIpc(volatile UInt8 *dsp) {
    for (int i = 0; i < 64; i++) {
        UInt32 hipct = rd32(dsp, IPC_HIPCTDR);
        if (!(hipct & IPC_BUSY)) break;
        wr32(dsp, IPC_HIPCTDA, IPC_DONE);
        wr32(dsp, IPC_HIPCTDR, hipct);
    }
}

static IOPMPowerState sPowerStates[2] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, kIOPMDeviceUsable | kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

bool MedionAudioDevice::init(OSDictionary *d) {
    if (!IOService::init(d)) return false;
    pciDevice = nullptr; hdaBarMap = nullptr; dspBarMap = nullptr;
    hdaBase = nullptr; dspBase = nullptr;
    ppCap = 0; spibCap = 0; mlCap = 0;
    sIdx = 0; sTag = 0; sd = 0;
    capIdx = 0; capTag = 0; capSd = 0;
    outboxOff = 0;
    hwReady = false; isPlaying = false; isCapturing = false;
    codecConfigured = false;
    activePlaybackHost = PIPE1_HOST_ID;
    captureSource = 0; capActiveHost = PIPE2_HOST_ID;
    for (int i = 0; i < 8; i++) tuning[i] = 0;
    tuning[2] = 3;
    fwStagingLen = 0;
    commandGate = nullptr; ownWorkLoop = nullptr; rootDomain = nullptr; powerNotifier = nullptr;
    sharedDmaBuf = nullptr; sharedBdlBuf = nullptr;
    capDmaBuf = nullptr; capBdlBuf = nullptr;
    flagsBuf = nullptr;
    fwStagingBuf = nullptr;
    return true;
}

void MedionAudioDevice::free() {
    codec.free();
    IOService::free();
}

IOService *MedionAudioDevice::probe(IOService *provider, SInt32 *score) {
    IOLog("MedionAudio: MedionAudioDevice::probe called on provider %s\n",
          provider ? provider->getName() : "null");
    if (!IOService::probe(provider, score)) return nullptr;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;

    // Validate identity via IORegistry memory properties (avoids unpowered D3hot configRead16 failures)
    OSData *devId = OSDynamicCast(OSData, pci->getProperty("device-id"));
    OSData *venId = OSDynamicCast(OSData, pci->getProperty("vendor-id"));
    if (devId && venId && devId->getLength() >= 2 && venId->getLength() >= 2) {
        UInt16 vid = *(const UInt16 *)venId->getBytesNoCopy();
        UInt16 did = *(const UInt16 *)devId->getBytesNoCopy();
        IOLog("MedionAudio: probe checking vendor=0x%04x device=0x%04x\n", vid, did);
        if (vid != 0x8086 || did != 0x02c8) {
            IOLog("MedionAudio: probe vendor/device mismatch, rejecting\n");
            return nullptr;
        }
    }
    pci->setProperty("MedionAudioProbed", "yes");
    IOLog("MedionAudio: probe accepted, proceeding to start!\n");
    return this;
}

bool MedionAudioDevice::start(IOService *provider) {
    IOLog("MedionAudio: MedionAudioDevice::start called\n");
    if (!IOService::start(provider)) return false;
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) return false;
    pciDevice->retain();

    if (!pciDevice->open(this)) {
        IOLog("MedionAudio: FAILED to open PCI device\n");
        setProperty("Status", "FAILED: PCI open");
        goto fail;
    }

    pciDevice->setIOEnable(true);
    pciDevice->setBusMasterEnable(true);
    pciDevice->setMemoryEnable(true);

    // Power cycle D3->D0
    {
        UInt16 pmcsr = pciDevice->configRead16(0x54);
        pciDevice->configWrite16(0x54, (pmcsr & ~0x3) | 0x3);
        IOSleep(50);
        pciDevice->configWrite16(0x54, pmcsr & ~0x3);
        IOSleep(50);
        pciDevice->setBusMasterEnable(true);
        pciDevice->setMemoryEnable(true);
    }

    // TCSEL clear
    {
        UInt8 tcsel = pciDevice->configRead8(PCI_TCSEL);
        pciDevice->configWrite8(PCI_TCSEL, tcsel & ~0x07);
    }
    // Disable PCI interrupt
    {
        UInt16 cmd = pciDevice->configRead16(0x04);
        pciDevice->configWrite16(0x04, cmd | 0x0400);
    }

    hdaBarMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    dspBarMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
    if (!hdaBarMap || !dspBarMap) {
        IOLog("MedionAudio: FAILED to map BAR0 or BAR4\n");
        setProperty("Status", "FAILED: BAR map");
        goto fail;
    }
    hdaBase = (volatile UInt8 *)hdaBarMap->getVirtualAddress();
    dspBase = (volatile UInt8 *)dspBarMap->getVirtualAddress();
    if (!hdaBase || !dspBase) {
        IOLog("MedionAudio: FAILED null virtual address for BAR0/BAR4\n");
        setProperty("Status", "FAILED: BAR virt");
        goto fail;
    }
    IOLog("MedionAudio: BAR0 (HDA) at %p, BAR4 (DSP) at %p (fast start, DSP deferred)\n",
          hdaBase, dspBase);

    pciDevice->setProperty("MedionAudio_Step", "1_BAR_Mapped");
    setProperty("DSPStatus", "Deferred/UserClient");
    pciDevice->setProperty("MedionAudioDSPStatus", "Deferred/UserClient");

    // Soft-touch codec: map I2C/GPIO, optional configure, always force amps OFF
    if (codec.init() && codec.configure()) {
        setProperty("Codec", "ES8326 (ESSX8326)");
        setProperty("CodecStatus", codec.getStatus());
        pciDevice->setProperty("MedionAudioCodec", codec.getStatus());
        IOLog("MedionAudio: Codec soft-init on start: %s\n", codec.getStatus());
    } else {
        setProperty("Codec", "Init Deferred");
        pciDevice->setProperty("MedionAudioCodec", "Init Deferred");
        IOLog("MedionAudio: Codec soft-init deferred (Voodoo not ready)\n");
    }
    codec.silenceAmps();

    if (!allocateDmaBuffers()) {
        IOLog("MedionAudio: WARNING DMA buffer alloc failed at start (retry via initDSP)\n");
    }

    if (!allocateFwStaging()) {
        IOLog("MedionAudio: WARNING FW staging buffer alloc failed at start\n");
    } else {
        IOLog("MedionAudio: FW staging buffer ready (%u bytes, phys=0x%llx)\n",
              kCmlSOF_FwStagingSize,
              (unsigned long long)fwStagingBuf->getPhysicalAddress());
    }

    /* Own work loop: getWorkLoop() would return the PCI parent's, which VoodooI2C's controller
     * also uses. Gated calls doing I2C (startCapture -> setCaptureMic) then deadlocked against
     * the jack thread's I2C reads and froze the trackpad on the same bus. */
    ownWorkLoop = IOWorkLoop::workLoop();
    if (ownWorkLoop) {
        commandGate = IOCommandGate::commandGate(this);
        if (commandGate) {
            ownWorkLoop->addEventSource(commandGate);
        }
    }

    PMinit();
    registerPowerDriver(this, sPowerStates, 2);
    provider->joinPMtree(this);

    rootDomain = getPMRootDomain();
    if (rootDomain) {
        powerNotifier = rootDomain->registerInterest(
            gIOGeneralInterest, &sPowerInterestHandler, this, nullptr);
    }

    flagsBuf = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, PAGE_SIZE, PAGE_SIZE);
    if (flagsBuf) {
        flagsBuf->prepare();
        void *flagBytes = flagsBuf->getBytesNoCopy();
        if (flagBytes) bzero(flagBytes, PAGE_SIZE);
    }

    setProperty("Status", "BridgeReady");
    if (pciDevice) pciDevice->setProperty("MedionAudioStarted", "yes");
    registerService();
    IOLog("MedionAudio: Hardware bridge registered (initDSP via UserClient)\n");
    return true;

fail:
    IOLog("MedionAudio: start() failed, cleaning up\n");
    if (hdaBarMap) { hdaBarMap->release(); hdaBarMap = nullptr; }
    if (dspBarMap) { dspBarMap->release(); dspBarMap = nullptr; }
    if (pciDevice) {
        if (pciDevice->isOpen(this)) {
            pciDevice->close(this);
        }
        pciDevice->release();
        pciDevice = nullptr;
    }
    return false;
}

void MedionAudioDevice::stop(IOService *provider) {
    if (powerNotifier) {
        powerNotifier->remove();
        powerNotifier = nullptr;
    }
    PMstop();
    if (commandGate) {
        if (ownWorkLoop) ownWorkLoop->removeEventSource(commandGate);
        commandGate->release();
        commandGate = nullptr;
    }
    if (ownWorkLoop) { ownWorkLoop->release(); ownWorkLoop = nullptr; }
    if (flagsBuf) { flagsBuf->complete(); flagsBuf->release(); flagsBuf = nullptr; }
    if (fwStagingBuf) { fwStagingBuf->complete(); fwStagingBuf->release(); fwStagingBuf = nullptr; }
    fwStagingLen = 0;
    if (sharedDmaBuf) { sharedDmaBuf->complete(); sharedDmaBuf->release(); sharedDmaBuf = nullptr; }
    if (sharedBdlBuf) { sharedBdlBuf->complete(); sharedBdlBuf->release(); sharedBdlBuf = nullptr; }
    if (capDmaBuf) { capDmaBuf->complete(); capDmaBuf->release(); capDmaBuf = nullptr; }
    if (capBdlBuf) { capBdlBuf->complete(); capBdlBuf->release(); capBdlBuf = nullptr; }
    codec.free();
    if (hdaBarMap) { hdaBarMap->release(); hdaBarMap = nullptr; }
    if (dspBarMap) { dspBarMap->release(); dspBarMap = nullptr; }
    if (pciDevice) {
        pciDevice->close(this);
        pciDevice->release();
        pciDevice = nullptr;
    }
    IOService::stop(provider);
}

// ==================== DMA / MMIO helpers ====================

bool MedionAudioDevice::allocateDmaBuffers() {
    if (!sharedDmaBuf) {
        /* SOF pcm.buffer.phy_addr is uint32_t — keep DMA below 4 GiB. */
        sharedDmaBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, kCmlSOF_BufferSize, 0x00000000FFFFF000ULL);
        if (!sharedDmaBuf) return false;
        sharedDmaBuf->prepare();
    }
    if (!sharedBdlBuf) {
        sharedBdlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, 256, 0x00000000FFFFFF80ULL);
        if (!sharedBdlBuf) return false;
        sharedBdlBuf->prepare();
    }
    if (!capDmaBuf) {
        capDmaBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, kCmlSOF_CapBufferSize, 0x00000000FFFFF000ULL);
        if (!capDmaBuf) return false;
        capDmaBuf->prepare();
    }
    if (!capBdlBuf) {
        capBdlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, 256, 0x00000000FFFFFF80ULL);
        if (!capBdlBuf) return false;
        capBdlBuf->prepare();
    }
    return (sharedDmaBuf && sharedBdlBuf && capDmaBuf && capBdlBuf);
}

bool MedionAudioDevice::allocateFwStaging() {
    if (fwStagingBuf) return true;
    fwStagingBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut, kCmlSOF_FwStagingSize,
        0xFFFFFFFFFFFFF000ULL);
    if (!fwStagingBuf) return false;
    fwStagingBuf->prepare();
    void *bytes = fwStagingBuf->getBytesNoCopy();
    if (bytes) bzero(bytes, kCmlSOF_FwStagingSize);
    fwStagingLen = 0;
    return true;
}

void MedionAudioDevice::clearDspFailProps() {
    removeProperty("DSPFail");
    removeProperty("ROMStatus");
    if (pciDevice) {
        pciDevice->removeProperty("MedionAudio_DSPFail");
        pciDevice->removeProperty("MedionAudio_ROMStatus");
    }
}

void MedionAudioDevice::powerGateDSP() {
    if (!dspBase) return;
    UInt32 a = rd32(dspBase, DSP_ADSPCS);
    a |= (UInt32)(ADSPCS_CRST(0xF) | ADSPCS_CSTALL(0xF));
    a &= ~(UInt32)ADSPCS_SPA(0xF);
    wr32(dspBase, DSP_ADSPCS, a);
    poll32(dspBase, DSP_ADSPCS, ADSPCS_CPA(0xF), 0, 50000);
    IODelay(1000);
    IOLog("MedionAudio: DSP power-gated (ADSPCS=0x%08x)\n", rd32(dspBase, DSP_ADSPCS));
}

IOReturn MedionAudioDevice::forceResetDSP() {
    codec.mute(true);
    codec.silenceAmps();
    isPlaying = false;
    isCapturing = false;
    hwReady = false;
    powerGateDSP();
    clearDspFailProps();
    setProperty("DSPStatus", "Reset/Deferred");
    if (pciDevice)
        pciDevice->setProperty("MedionAudioDSPStatus", "Reset/Deferred");
    IOLog("MedionAudio: ForceResetDSP complete (hwReady=false)\n");
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::setFwStagingLen(UInt32 len) {
    if (len > kCmlSOF_FwStagingSize) return kIOReturnBadArgument;
    if (!allocateFwStaging()) return kIOReturnNoMemory;
    fwStagingLen = len;
    IOLog("MedionAudio: fwStagingLen set to %u\n", len);
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::getFwPhys(UInt64 *fwPhys) {
    if (!fwPhys) return kIOReturnBadArgument;
    *fwPhys = 0;
    if (!allocateFwStaging()) return kIOReturnNoMemory;
    if (fwStagingBuf) *fwPhys = (UInt64)fwStagingBuf->getPhysicalAddress();
    return kIOReturnSuccess;
}

bool MedionAudioDevice::stagingHasPayload(UInt32 *outOffset, UInt32 *outSize) const {
    if (!fwStagingBuf || !outOffset || !outSize) return false;
    const UInt8 *bytes = (const UInt8 *)fwStagingBuf->getBytesNoCopy();
    if (!bytes) return false;

    UInt32 avail = fwStagingLen;
    if (avail == 0) {
        // Treat any non-zero header as staged even if length not tracked yet
        UInt32 nonzero = 0;
        for (UInt32 i = 0; i < 64 && i < kCmlSOF_FwStagingSize; i++) {
            if (bytes[i]) { nonzero = 1; break; }
        }
        if (!nonzero) return false;
        avail = kCmlSOF_FwStagingSize;
    }
    if (avail < 8) return false;

    UInt32 payloadOffset = 0;
    UInt32 extSig = *(const UInt32 *)bytes;
    if (extSig == 0x6e614d58) // 'XMan'
        payloadOffset = *(const UInt32 *)(bytes + 4);
    if (payloadOffset >= avail) return false;
    UInt32 payloadSize = avail - payloadOffset;
    if (payloadSize == 0) return false;
    *outOffset = payloadOffset;
    *outSize = payloadSize;
    return true;
}

IOReturn MedionAudioDevice::mmioRead(volatile UInt8 *base, IOMemoryMap *map, UInt32 width,
                                       UInt32 offset, UInt64 *outVal) {
    if (!base || !map || !outVal) return kIOReturnNotReady;
    IOByteCount len = map->getLength();
    if (width == 0 || (offset + width) > len) return kIOReturnBadArgument;
    switch (width) {
        case 1: *outVal = rd8(base, offset); break;
        case 2: if (offset & 1) return kIOReturnBadArgument; *outVal = rd16(base, offset); break;
        case 4: if (offset & 3) return kIOReturnBadArgument; *outVal = rd32(base, offset); break;
        case 8: {
            if (offset & 3) return kIOReturnBadArgument;
            UInt32 lo = rd32(base, offset);
            UInt32 hi = rd32(base, offset + 4);
            *outVal = ((UInt64)hi << 32) | lo;
            break;
        }
        default: return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::mmioWrite(volatile UInt8 *base, IOMemoryMap *map, UInt32 width,
                                        UInt32 offset, UInt64 value) {
    if (!base || !map) return kIOReturnNotReady;
    IOByteCount len = map->getLength();
    if (width == 0 || (offset + width) > len) return kIOReturnBadArgument;
    switch (width) {
        case 1: wr8(base, offset, (UInt8)value); break;
        case 2: if (offset & 1) return kIOReturnBadArgument; wr16(base, offset, (UInt16)value); break;
        case 4: if (offset & 3) return kIOReturnBadArgument; wr32(base, offset, (UInt32)value); break;
        case 8: {
            if (offset & 3) return kIOReturnBadArgument;
            wr32(base, offset, (UInt32)(value & 0xFFFFFFFF));
            wr32(base, offset + 4, (UInt32)(value >> 32));
            break;
        }
        default: return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::readBAR0(UInt32 width, UInt32 offset, UInt64 *outVal) {
    return mmioRead(hdaBase, hdaBarMap, width, offset, outVal);
}
IOReturn MedionAudioDevice::writeBAR0(UInt32 width, UInt32 offset, UInt64 value) {
    return mmioWrite(hdaBase, hdaBarMap, width, offset, value);
}
IOReturn MedionAudioDevice::readBAR4(UInt32 width, UInt32 offset, UInt64 *outVal) {
    return mmioRead(dspBase, dspBarMap, width, offset, outVal);
}
IOReturn MedionAudioDevice::writeBAR4(UInt32 width, UInt32 offset, UInt64 value) {
    return mmioWrite(dspBase, dspBarMap, width, offset, value);
}

IOReturn MedionAudioDevice::readGPIO32(UInt32 offset, UInt32 *outVal) {
    if (!outVal) return kIOReturnBadArgument;
    /* High 16 bits = community index (0 or 1). Low 16 = MMIO offset. */
    UInt32 community = offset >> 16;
    UInt32 off = offset & 0xFFFF;
    volatile UInt8 *base = nullptr;
    IOByteCount len = 0;
    if (community == 0) {
        base = codec.getGpioBase();
        len = codec.getGpioLength();
    } else if (community == 1) {
        base = codec.getGpioAmpBase();
        len = codec.getGpioAmpLength();
    } else {
        return kIOReturnBadArgument;
    }
    if (!base || len == 0) return kIOReturnNotReady;
    if ((off + 4) > len || (off & 3)) return kIOReturnBadArgument;
    *outVal = rd32(base, off);
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::writeGPIO32(UInt32 offset, UInt32 value) {
    UInt32 community = offset >> 16;
    UInt32 off = offset & 0xFFFF;
    volatile UInt8 *base = nullptr;
    IOByteCount len = 0;
    if (community == 0) {
        /* Community 0 is read-only; it contains unrelated hardware pads. */
        return kIOReturnNotPrivileged;
    } else if (community == 1) {
        base = codec.getGpioAmpBase();
        len = codec.getGpioAmpLength();
        /* speakers-enable = community 1, pad 8, PADCFG0 at 0x600+8*16 = 0x680.
         * Refuse every other pad. */
        if (off != 0x680)
            return kIOReturnNotPrivileged;
    } else {
        return kIOReturnBadArgument;
    }
    if (!base || len == 0) return kIOReturnNotReady;
    if ((off + 4) > len || (off & 3)) return kIOReturnBadArgument;
    wr32(base, off, value);
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::i2cWriteReg(UInt8 reg, UInt8 val) {
    if (!codec.isReady()) {
        if (!codec.init()) return kIOReturnNotReady;
    }
    return codec.writeReg(reg, val) ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn MedionAudioDevice::i2cReadReg(UInt8 reg, UInt8 *outVal) {
    if (!outVal) return kIOReturnBadArgument;
    if (!codec.isReady()) {
        if (!codec.init()) return kIOReturnNotReady;
    }
    return codec.readRegChecked(reg, outVal) ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn MedionAudioDevice::getPhysicalAddrs(UInt64 *playPhys, UInt64 *capPhys, UInt64 *flagsPhys) {
    if (!playPhys || !capPhys || !flagsPhys) return kIOReturnBadArgument;
    *playPhys = 0; *capPhys = 0; *flagsPhys = 0;
    if (sharedDmaBuf) *playPhys = (UInt64)sharedDmaBuf->getPhysicalAddress();
    if (capDmaBuf) *capPhys = (UInt64)capDmaBuf->getPhysicalAddress();
    if (flagsBuf) *flagsPhys = (UInt64)flagsBuf->getPhysicalAddress();
    return kIOReturnSuccess;
}

// ==================== DSP Initialization ====================

bool MedionAudioDevice::initDSP(bool force, bool useStaging) {
    volatile UInt8 *hda = hdaBase;
    volatile UInt8 *dsp = dspBase;
    if (!hda || !dsp || !hdaBarMap || !dspBarMap) return false;
    if (hwReady && !force) return true;
    if (force && hwReady) {
        IOLog("MedionAudio: initDSP force re-run (clearing hwReady)\n");
        hwReady = false;
        isPlaying = false;
        isCapturing = false;
        powerGateDSP();
        clearDspFailProps();
    }

    IOLog("MedionAudio: initDSP starting (force=%d useStaging=%d)...\n",
          force ? 1 : 0, useStaging ? 1 : 0);

    UInt16 gcap = rd16(hda, HDA_GCAP);
    int numISS = (gcap >> 8) & 0xF;
    int numOSS = (gcap >> 12) & 0xF;
    (void)numOSS;
    IOLog("MedionAudio: HDA GCAP=0x%04x (ISS=%d, OSS=%d)\n", gcap, numISS, numOSS);

    sIdx = numISS;
    /* ROM code-loader IPC uses (sTag-1)<<9 and historically boots only with tag 1
     * on the playback SD. Runtime StartPlayback overrides to a unique playTag. */
    sTag = 1;
    sd = SD_BASE + (UInt32)sIdx * SD_SIZE;

    capIdx = 0;
    /* SOF host_params(): capture DMA channel = stream_tag - 1 → ISS[0]/SD0 */
    capTag = 1;
    capSd = SD_BASE + (UInt32)capIdx * SD_SIZE;

    UInt32 fwSize = (UInt32)sof_fw_size;
    UInt32 payloadOffset = 0, payloadSize = 0;
    IOBufferMemoryDescriptor *bdlBuf = nullptr;
    UInt32 numBdl = 0;
    bool fwLoaded = false;
    IOByteCount dmaBaseOff = 0;

    wr32(hda, HDA_VS_EM2, rd32(hda, HDA_VS_EM2) | 0x4000);
    IODelay(100);

    { UInt8 cgctl = pciDevice->configRead8(0x48);
      pciDevice->configWrite8(0x48, cgctl & ~(1U << 6)); }

    if (rd32(hda, HDA_GCTL) & 1)
        wr32(hda, 0x0E, 0xFFFF);

    wr32(hda, HDA_GCTL, rd32(hda, HDA_GCTL) & ~1U);
    for (int t = 0; t < 100; t++) { if (!(rd32(hda, HDA_GCTL) & 1)) break; IODelay(1000); }
    IODelay(1000);

    wr32(hda, HDA_GCTL, rd32(hda, HDA_GCTL) | 1U);
    for (int t = 0; t < 100; t++) { if (rd32(hda, HDA_GCTL) & 1) break; IODelay(1000); }
    IODelay(1500);
    wr32(hda, HDA_GCTL, rd32(hda, HDA_GCTL) | (1U << 8));

    for (int i = 0; i < numISS + 4; i++) {
        UInt32 sdOff = SD_BASE + (UInt32)i * SD_SIZE;
        wr32(hda, sdOff + SD_REG_STS, 0x1C);
    }
    wr32(hda, 0x0E, 0xFFFF);
    wr32(hda, 0x24, 0xC0000000 | ((1U << (numISS + 4)) - 1));
    wr32(hda, HDA_INTCTL, (1U << 31) | (1U << 30));

    { UInt8 cgctl = pciDevice->configRead8(0x48);
      pciDevice->configWrite8(0x48, cgctl | (1U << 6)); }

    // Resolve FW source: staging (useStaging or XMan already present) else embedded.
    const UInt8 *fwSrc = sof_fw_data;
    fwSize = (UInt32)sof_fw_size;
    bool fromStaging = false;
    {
        UInt32 stOff = 0, stSize = 0;
        bool hasStaged = stagingHasPayload(&stOff, &stSize);
        if ((useStaging || hasStaged) && hasStaged && fwStagingBuf) {
            const UInt8 *staged = (const UInt8 *)fwStagingBuf->getBytesNoCopy();
            UInt32 total = (fwStagingLen > 0) ? fwStagingLen : (stOff + stSize);
            if (staged && total >= 8 && total <= kCmlSOF_FwStagingSize) {
                fwSrc = staged;
                fwSize = total;
                fromStaging = true;
            }
        }
        if (useStaging && !fromStaging) {
            IOLog("MedionAudio: staging empty/zero — falling back to embedded sof_fw_data\n");
        }
    }
    if (pciDevice)
        pciDevice->setProperty("MedionAudio_FWSource", fromStaging ? "staging" : "embedded");

    payloadOffset = 0;
    if (*(const UInt32 *)fwSrc == 0x6e614d58) // 'XMan'
        payloadOffset = *(const UInt32 *)(fwSrc + 4);
    payloadSize = fwSize - payloadOffset;

    if (payloadOffset >= fwSize || payloadSize == 0 || payloadSize > kCmlSOF_FwStagingSize) {
        IOLog("MedionAudio: FAILED bad FW payload (offset=%u, size=%u, total=%u)\n",
              payloadOffset, payloadSize, fwSize);
        return false;
    }

    IOLog("MedionAudio: FW payload at offset %u, size %u bytes (src=%s)\n",
          payloadOffset, payloadSize, fromStaging ? "staging" : "embedded");

    UInt32 hdaLen = hdaBarMap ? (UInt32)hdaBarMap->getLength() : 0;
    for (UInt32 o = 0x500; o < hdaLen && o < 0x2000; o += 0x10) {
        UInt16 capId = (rd32(hda, o) >> 16) & 0xFFF;
        if (capId == HDA_CAP_ML_ID && !mlCap) mlCap = o;
        if (capId == HDA_CAP_PP_ID && !ppCap) ppCap = o;
        if (capId == HDA_CAP_SPIB_ID && !spibCap) spibCap = o;
    }

    // PPCAP before DSP power (Linux hda_dsp_ctrl_ppcap_enable)
    if (ppCap)
        wr32(hda, ppCap + PP_PPCTL, (1U << 31) | PP_PPCTL_GPROCEN | (1U << sIdx) | (1U << capIdx));

    // Full power-cycle clears prior ROM/CSE halted state (FSR 0x80000012 / err 0x2c)
    {
        UInt32 a = rd32(dsp, DSP_ADSPCS);
        a |= (UInt32)(ADSPCS_CRST(0xF) | ADSPCS_CSTALL(0xF));
        a &= ~(UInt32)ADSPCS_SPA(0xF);
        wr32(dsp, DSP_ADSPCS, a);
        poll32(dsp, DSP_ADSPCS, ADSPCS_CPA(0xF), 0, 50000);
        IODelay(1000);
    }

    wr32(dsp, DSP_ADSPCS, rd32(dsp, DSP_ADSPCS) | ADSPCS_CRST(0xF) | ADSPCS_CSTALL(0xF));
    IODelay(1000);
    wr32(dsp, DSP_ADSPCS, rd32(dsp, DSP_ADSPCS) | ADSPCS_SPA(0xF));
    if (!poll32(dsp, DSP_ADSPCS, ADSPCS_CPA(0xF), ADSPCS_CPA(0xF), 50000)) {
        if (!poll32(dsp, DSP_ADSPCS, ADSPCS_CPA(1), ADSPCS_CPA(1), 20000)) {
            IOLog("MedionAudio: FAILED to power up DSP cores (ADSPCS=0x%08x)\n", rd32(dsp, DSP_ADSPCS));
            if (pciDevice) {
                char s[64]; snprintf(s, sizeof(s), "FAIL_PWR: ADSPCS=0x%08x", rd32(dsp, DSP_ADSPCS));
                pciDevice->setProperty("MedionAudio_DSPFail", s);
            }
            return false;
        }
    }
    IOLog("MedionAudio: DSP cores powered up successfully (ADSPCS=0x%08x)\n", rd32(dsp, DSP_ADSPCS));
    if (pciDevice) pciDevice->setProperty("MedionAudio_Step", "2_CoresPowered");

    // Code-loader DMA from persistent fwStagingBuf (userspace MemFirmware / embedded copy).
    if (!allocateFwStaging()) {
        IOLog("MedionAudio: FAILED allocateFwStaging for code-loader\n");
        return false;
    }
    {
        void *fwBytes = fwStagingBuf->getBytesNoCopy();
        if (!fwBytes) {
            IOLog("MedionAudio: FAILED fwStagingBuf getBytesNoCopy\n");
            return false;
        }
        if (fromStaging) {
            // Keep XMan header intact; DMA payload in-place from payloadOffset.
            dmaBaseOff = payloadOffset;
            if (fwStagingLen < payloadOffset + payloadSize)
                fwStagingLen = payloadOffset + payloadSize;
        } else {
            // Embedded path: stage full RI then DMA stripped payload.
            if (fwSize > kCmlSOF_FwStagingSize) {
                IOLog("MedionAudio: FAILED embedded FW larger than staging (%u)\n", fwSize);
                return false;
            }
            memcpy(fwBytes, sof_fw_data, fwSize);
            fwStagingLen = fwSize;
            dmaBaseOff = payloadOffset;
        }
    }

    numBdl = (payloadSize + PAGE_SIZE - 1) / PAGE_SIZE;
    if (numBdl > (PAGE_SIZE / 16)) numBdl = PAGE_SIZE / 16;
    {
        bdlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, PAGE_SIZE, 0xFFFFFFFFFFFFF000ULL);
        if (!bdlBuf) {
            IOLog("MedionAudio: FAILED to allocate temp BDL\n");
            return false;
        }
        bdlBuf->prepare();
        HdaBdlEntry *bdl = (HdaBdlEntry *)bdlBuf->getBytesNoCopy();
        if (!bdl) {
            IOLog("MedionAudio: FAILED BDL getBytesNoCopy\n");
            bdlBuf->complete(); bdlBuf->release();
            return false;
        }
        memset(bdl, 0, PAGE_SIZE);
        UInt32 rem = payloadSize; IOByteCount boff = dmaBaseOff;
        for (UInt32 i = 0; i < numBdl && rem > 0; i++) {
            IOByteCount segLen = 0;
            IOPhysicalAddress segP = fwStagingBuf->getPhysicalSegment(boff, &segLen, 0);
            UInt32 chunk = (rem > PAGE_SIZE) ? PAGE_SIZE : rem;
            if (segLen < chunk) chunk = (UInt32)segLen;
            bdl[i].addrLow = (UInt32)(segP & 0xFFFFFFFF);
            bdl[i].addrHigh = (UInt32)(segP >> 32);
            bdl[i].size = chunk;
            bdl[i].ioc = (rem <= chunk) ? 1 : 0;
            rem -= chunk; boff += chunk;
        }
        if (rem != 0) {
            IOLog("MedionAudio: FAILED BDL incomplete rem=%u\n", rem);
            bdlBuf->complete(); bdlBuf->release();
            return false;
        }
    }
    IOPhysicalAddress bdlPhys = bdlBuf->getPhysicalAddress();

    streamReset(hda, sd);
    wr32(hda, sd + SD_REG_BDLPL, (UInt32)(bdlPhys & 0xFFFFFFFF));
    wr32(hda, sd + SD_REG_BDLPU, (UInt32)(bdlPhys >> 32));
    wr32(hda, sd + SD_REG_CBL, payloadSize);
    wr16(hda, sd + SD_REG_LVI, (UInt16)(numBdl - 1));
    wr16(hda, sd + SD_REG_FMT, HDA_CL_STREAM_FMT);
    wr8(hda, sd + 2, (UInt8)((sTag & 0xF) << 4));

    if (spibCap) {
        wr32(hda, spibCap + 0x04, rd32(hda, spibCap + 0x04) | (1U << sIdx));
        wr32(hda, spibCap + 0x08 + (UInt32)sIdx * 0x08, payloadSize);
    }

    // Configure SSP0 to CBP_CFP mode
    for (UInt32 s = 0; s < 3; s++) {
        UInt32 ssc1Off = 0x10000 + s * 0x1000 + 0x4;
        wr32(dsp, ssc1Off, rd32(dsp, ssc1Off) | 0x03000000);
    }

    wr32(dsp, IPC_HIPCIDR, IPC_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((sTag - 1) << 9));
    {
        UInt32 a = rd32(dsp, DSP_ADSPCS);
        a &= ~(UInt32)ADSPCS_CRST(1); wr32(dsp, DSP_ADSPCS, a);
        poll32(dsp, DSP_ADSPCS, (UInt32)ADSPCS_CRST(1), 0, 50000);
        a = rd32(dsp, DSP_ADSPCS);
        a &= ~(UInt32)ADSPCS_CSTALL(1); wr32(dsp, DSP_ADSPCS, a);
    }

    if (!poll32(dsp, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
        IOLog("MedionAudio: FAILED waiting for ROM IPC purge/control\n");
        bdlBuf->complete(); bdlBuf->release();
        return false;
    }
    wr32(dsp, IPC_HIPCIDA, rd32(dsp, IPC_HIPCIDA) | IPC_DONE);

    { UInt32 a = rd32(dsp, DSP_ADSPCS);
      a |= (UInt32)(ADSPCS_CRST(0xE) | ADSPCS_CSTALL(0xE)); a &= ~(UInt32)ADSPCS_SPA(0xE);
      wr32(dsp, DSP_ADSPCS, a); }
    wr32(dsp, IPC_HIPCCTL, 0x03);
    wr32(dsp, DSP_ADSPIC, rd32(dsp, DSP_ADSPIC) | 0x01);

    {
        bool initDone = false;
        for (int t = 0; t < 300; t++) {
            if ((rd32(dsp, ROM_STATUS) & 0xFFFFFF) == FSR_INIT_DONE) { initDone = true; break; }
            IOSleep(1);
        }
        IOLog("MedionAudio: DSP ROM Status after un-reset: 0x%08x\n", rd32(dsp, ROM_STATUS));
        if (pciDevice) {
            char s[64]; snprintf(s, sizeof(s), "ROM=0x%08x", rd32(dsp, ROM_STATUS));
            pciDevice->setProperty("MedionAudio_ROMStatus", s);
        }
        if (!initDone) {
            IOLog("MedionAudio: FAILED waiting for FSR_INIT_DONE (ROM_STATUS=0x%08x err=0x%x)\n",
                  rd32(dsp, ROM_STATUS), rd32(dsp, ROM_STATUS + 4));
            bdlBuf->complete(); bdlBuf->release();
            return false;
        }
    }

    wr32(hda, HDA_INTCTL, rd32(hda, HDA_INTCTL) | (1U << 31) | (1U << 30) | (1U << sIdx));
    wr8(hda, sd + SD_REG_STS, 0x1C);
    wr8(hda, sd, rd8(hda, sd) | SD_CTL_RUN | SD_CTL_IOCE);
    IODelay(500);

    for (int t = 0; t < 3000; t++) {
        UInt32 romSt = rd32(dsp, ROM_STATUS);
        if ((romSt & 0xFFFFFF) == FSR_FW_ENTERED) { fwLoaded = true; break; }
        // Bail early on CSE validation halt (0x80000012 + ext 0x2c)
        if ((romSt & 0x80000000U) && ((romSt & 0xFFFFFF) == 0x12)) break;
        IOSleep(1);
    }

    wr8(hda, sd, rd8(hda, sd) & ~(UInt8)(SD_CTL_RUN | SD_CTL_IOCE));
    wr32(hda, HDA_INTCTL, rd32(hda, HDA_INTCTL) & ~(1U << sIdx));

    bdlBuf->complete(); bdlBuf->release(); bdlBuf = nullptr;

    if (!fwLoaded) {
        UInt32 romSt = rd32(dsp, ROM_STATUS);
        UInt32 romErr = rd32(dsp, ROM_STATUS + 4);
        IOLog("MedionAudio: FAILED to boot SOF firmware (ROM_STATUS=0x%08x err=0x%x)\n",
              romSt, romErr);
        if (pciDevice) {
            char s[80];
            snprintf(s, sizeof(s), "FW_FAIL: ROM=0x%08x err=0x%x", romSt, romErr);
            pciDevice->setProperty("MedionAudio_DSPFail", s);
        }
        return false;
    }
    IOLog("MedionAudio: SOF firmware loaded and entered successfully!\n");
    if (pciDevice) pciDevice->setProperty("MedionAudio_Step", "3_FW_Booted");

    for (int t = 0; t < 1000; t++) {
        if (rd32(dsp, IPC_HIPCTDR) & IPC_BUSY) break;
        IOSleep(1);
    }

    outboxOff = 0x82000;
    UInt32 dspLen = dspBarMap ? (UInt32)dspBarMap->getLength() : 0;
    if (dspLen >= MBOX_UPLINK + 256) {
        volatile UInt8 *mb = dsp + MBOX_UPLINK;
        UInt32 hSz = rd32(mb, 0x00);
        UInt32 hCmd = rd32(mb, 0x04);
        if (hCmd == SOF_IPC_FW_READY && hSz >= 60 && hSz <= 200) {
            char ver[80];
            snprintf(ver, sizeof(ver), "%d.%d.%d-%d ABI:%d.%d.%d",
                     rd16(mb, 0x1C), rd16(mb, 0x1E), rd16(mb, 0x20), rd16(mb, 0x22),
                     (rd32(mb, 0x40) >> 24) & 0xFF, (rd32(mb, 0x40) >> 16) & 0xFF,
                     rd32(mb, 0x40) & 0xFF);
            setProperty("FW-Version", ver);
            IOLog("MedionAudio: SOF FW Version: %s\n", ver);

            UInt32 extOff = MBOX_UPLINK + hSz;
            for (int i = 0; i < 20; i++) {
                if (extOff + 12 > dspLen) break;
                UInt32 eSize = rd32(dsp, extOff), eCmd = rd32(dsp, extOff + 4);
                UInt32 eType = rd32(dsp, extOff + 8);
                if (eCmd != SOF_IPC_FW_READY || eSize < 12 || eSize > 4096) break;
                if (eType == SOF_IPC_EXT_WINDOW) {
                    UInt32 nw = rd32(dsp, extOff + 12);
                    for (UInt32 w = 0; w < nw && w < 8; w++) {
                        UInt32 wb = extOff + 16 + w * 24;
                        if (wb + 24 > dspLen) break;
                        UInt32 wType = rd32(dsp, wb + 4), wId = rd32(dsp, wb + 8);
                        UInt32 wOff = rd32(dsp, wb + 20);
                        if (wType == SOF_IPC_REGION_DOWNBOX) outboxOff = SRAM_WIN(wId) + wOff;
                    }
                }
                extOff += eSize;
            }
        }
    }
    if (!outboxOff) outboxOff = 0x82000;
    IOLog("MedionAudio: DSP Outbox mailbox offset: 0x%x\n", outboxOff);

    wr32(dsp, IPC_HIPCTDR, rd32(dsp, IPC_HIPCTDR) | IPC_BUSY);
    wr32(dsp, IPC_HIPCTDA, IPC_DONE);

    auto sendIpc = [&](const void *msg, UInt32 msgSize) -> UInt32 {
        drainDspIpc(dsp);
        volatile UInt8 *ob = dsp + outboxOff;
        UInt8 *src = (UInt8 *)msg;
        for (UInt32 i = 0; i < msgSize; i += 4)
            wr32(ob, i, *(UInt32 *)(src + i));
        // Clear any previous completion flag before asserting busy
        wr32(dsp, IPC_HIPCIDA, rd32(dsp, IPC_HIPCIDA) | IPC_DONE);
        wr32(dsp, IPC_HIPCCTL, rd32(dsp, IPC_HIPCCTL) | 0x03);
        wr32(dsp, IPC_HIPCIDR, IPC_BUSY);
        if (!poll32(dsp, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
            drainDspIpc(dsp);
            return 0xFFFFFFFF;
        }
        UInt32 replyErr = rd32(ob, 8);
        wr32(dsp, IPC_HIPCIDA, rd32(dsp, IPC_HIPCIDA) | IPC_DONE);
        wr32(dsp, IPC_HIPCCTL, rd32(dsp, IPC_HIPCCTL) | 0x03);
        drainDspIpc(dsp);
        return (replyErr != 0) ? replyErr : 0;
    };

    int ipcOk = 0, ipcFail = 0;
    auto sendIpcTracked = [&](const void *msg, UInt32 msgSize, const char *name) -> UInt32 {
        UInt32 err = sendIpc(msg, msgSize);
        if (err == 0) {
            ipcOk++;
        } else {
            ipcFail++;
            IOLog("MedionAudio: IPC error %u (0x%x) on %s\n", err, err, name ? name : "unknown");
            if (name) setProperty("TplgLastFail", name);
        }
        return err;
    };

    if (spibCap) wr32(hda, spibCap + 0x04, 0);
    streamReset(hda, sd);

    /*
     * Topology for the MEDION E15415 (NP1xCM), matching the stock Linux
     * sof-cml-es8336-ssp0.tplg used by sof_es8336 with quirk 0x0:
     *   PIPELINE.1 SSP0.OUT playback (ES8326 codec)
     *   PIPELINE.2 SSP0.IN  capture  (internal MIC1 or headset MIC2)
     * HOST FRAME_S32 (s32le), DAI FRAME_S24 (s24le), SSP MCLK 19.2 MHz /
     * BCLK 2.4 MHz / 24 valid / 25-slot, codec LEN_24.
     */
    setProperty("TplgTarget", "sof-cml-es8336-ssp0 (quirk 0x0)");

    // ==================== PIPELINE 1: SSP0 playback (ES8326) ====================
    // HOST S32 + DAI S24 — matches tplg s32le host / s24le SSP0.OUT (CmlSOF same DAI).
    { struct { UInt32 size, cmd, comp_id, pipeline_id, sched_id, core,
               period, priority, period_mips, frames_per_sched,
               xrun_limit_usecs, time_domain; } __attribute__((packed)) m = {
          48, 0x30100000, PIPE1_SCHED_ID, PIPE1_ID, PIPE1_HOST_ID, 0,
          1000, 0, 5000, 0, 0, TIME_TIMER };
      sendIpcTracked(&m, sizeof(m), "PIPE1_SCHED"); }
    { struct { UInt32 size, cmd, id, type, pipeline_id, core, ext,
               buf_size, caps, flags, reserved; } __attribute__((packed)) m = {
          44, 0x30200000, PIPE1_BUF0_ID, COMP_BUFFER, PIPE1_ID, 0, 0,
          19200, 0x71, 0, 0 };
      sendIpcTracked(&m, sizeof(m), "PIPE1_BUF0"); }
    /* sof_ipc_comp_config: periods_sink@9, periods_source@10, reserved1@11, frame_fmt@12, xrun@13 */
    { UInt32 m[76/4] = {}; m[0]=76; m[1]=0x30010000; m[2]=PIPE1_HOST_ID;
      m[3]=COMP_HOST; m[4]=PIPE1_ID; m[7]=36;
      /* Match CmlSOF / Linux: periods_sink=2 AND periods_source=2.
       * periods_source=0 accepted IPC but DSP may not pull host→buf (silence). */
      m[9]=2; m[10]=2; m[11]=0; m[12]=FRAME_S32; m[13]=0;
      m[16]=DIR_PLAYBACK;
      sendIpcTracked(m, 76, "PIPE1_HOST"); }
    { UInt32 m[80/4] = {}; m[0]=80; m[1]=0x30010000; m[2]=PIPE1_DAI_ID;
      m[3]=COMP_DAI; m[4]=PIPE1_ID; m[7]=36;
      m[9]=0; m[10]=2; m[11]=0; m[12]=FRAME_S24; m[13]=XRUN_STOP;
      m[16]=DIR_PLAYBACK; m[17]=0; m[18]=DAI_SSP;
      sendIpcTracked(m, 80, "PIPE1_DAI"); }
    /* DAI_CONFIG SSP0 — flags=NONE at topology load; HW_PARAMS
     * at startPlayback actually starts BCLK/FS/MCLK. Pack format as u16
     * (0x01004001 as u32 wrongly set HW_PARAMS early with ssp hdr.size=0).
     * FW ABI 3.1.1 accepts mclk_rate at ssp+8 (not Linux dai-intel.h +12). */
    { UInt8 m[216] = {};
      *(UInt32*)(m+0)=216; *(UInt32*)(m+4)=0x80010000;
      *(UInt32*)(m+8)=DAI_SSP; *(UInt32*)(m+12)=0;
      *(UInt16*)(m+16)=SOF_DAI_FMT_I2S_CBC_CFC;
      m[18]=0; /* group_id */
      m[19]=SOF_DAI_CONFIG_FLAGS_NONE;
      UInt8 *ssp=m+52;
      *(UInt32*)(ssp+0)=SSP_PARAMS_SIZE;
      *(UInt32*)(ssp+4)=0;         /* reserved1 | mclk_id */
      *(UInt32*)(ssp+8)=SSP_GOLD_MCLK;
      *(UInt32*)(ssp+12)=48000;    /* fsync_rate */
      *(UInt32*)(ssp+16)=SSP_GOLD_BCLK;
      *(UInt32*)(ssp+20)=2;        /* tdm_slots */
      *(UInt32*)(ssp+24)=3;        /* rx_slots L+R */
      *(UInt32*)(ssp+28)=3;        /* tx_slots L+R */
      *(UInt32*)(ssp+32)=SSP_GOLD_BITS;
      *(UInt16*)(ssp+36)=SSP_GOLD_SLOTW;
      *(UInt32*)(ssp+40)=1;        /* MCLK is an input to the codec */
      *(UInt32*)(ssp+48)=SSP_CLKS_CONTROL;
      sendIpcTracked(m, 216, "PIPE1_DAI_CONFIG"); }
    { struct { UInt32 s,c,src,dst; } __attribute__((packed)) conn[] = {
          {16, 0x30030000, PIPE1_HOST_ID, PIPE1_BUF0_ID},
          {16, 0x30030000, PIPE1_BUF0_ID, PIPE1_DAI_ID} };
      for (int c = 0; c < 2; c++) sendIpcTracked(&conn[c], 16, "PIPE1_CONN"); }
    { struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, 0x30130000, PIPE1_SCHED_ID};
      sendIpcTracked(&m, 12, "PIPE1_COMPLETE"); }

    // ==================== PIPELINE 2: SSP0.IN capture (headset mic) ====================
    // tplg PIPELINE.2.SSP0.IN : DAI(SSP0, s24le) -> BUF -> HOST(s32le). DAI_CONFIG SSP0
    // already sent for PIPE1 (rx_slots=3): same port, same I2S 24-in-25 format.
    { struct { UInt32 size, cmd, comp_id, pipeline_id, sched_id, core,
               period, priority, period_mips, frames_per_sched,
               xrun_limit_usecs, time_domain; } __attribute__((packed)) m = {
          48, 0x30100000, PIPE2_SCHED_ID, PIPE2_ID, PIPE2_HOST_ID, 0,
          1000, 0, 5000, 0, 0, TIME_TIMER };
      sendIpcTracked(&m, sizeof(m), "PIPE2_SCHED"); }
    { struct { UInt32 size, cmd, id, type, pipeline_id, core, ext,
               buf_size, caps, flags, reserved; } __attribute__((packed)) m = {
          44, 0x30200000, PIPE2_BUF0_ID, COMP_BUFFER, PIPE2_ID, 0, 0,
          19200, 0x71, 0, 0 };
      sendIpcTracked(&m, sizeof(m), "PIPE2_BUF0"); }
    { UInt32 m[76/4] = {}; m[0]=76; m[1]=0x30010000; m[2]=PIPE2_HOST_ID;
      m[3]=COMP_HOST; m[4]=PIPE2_ID; m[7]=36;
      m[9]=2; m[10]=2; m[12]=tuning[1] ? FRAME_S24 : FRAME_S32;
      m[16]=DIR_CAPTURE;
      sendIpcTracked(m, 76, "PIPE2_HOST"); }
    { UInt32 m[80/4] = {}; m[0]=80; m[1]=0x30010000; m[2]=PIPE2_DAI_ID;
      m[3]=COMP_DAI; m[4]=PIPE2_ID; m[7]=36;
      m[10]=2; m[12]=tuning[0] ? FRAME_S32 : FRAME_S24; m[13]=XRUN_STOP;
      m[16]=DIR_CAPTURE; m[17]=0; m[18]=DAI_SSP;
      sendIpcTracked(m, 80, "PIPE2_DAI"); }
    { struct { UInt32 s,c,src,dst; } __attribute__((packed)) conn[] = {
          {16, 0x30030000, PIPE2_DAI_ID, PIPE2_BUF0_ID},
          {16, 0x30030000, PIPE2_BUF0_ID, PIPE2_HOST_ID} };
      for (int c = 0; c < 2; c++) sendIpcTracked(&conn[c], 16, "PIPE2_CONN"); }
    { struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, 0x30130000, PIPE2_SCHED_ID};
      sendIpcTracked(&m, 12, "PIPE2_COMPLETE"); }

    IOLog("MedionAudio: IPC Topology (quirk 0x0 SSP0.OUT+SSP0.IN): %d OK, %d FAIL\n",
          ipcOk, ipcFail);
    char topStr[48];
    snprintf(topStr, sizeof(topStr), "0x0 SSP0.OUT+SSP0.IN: %d OK, %d FAIL", ipcOk, ipcFail);
    setProperty("Topology", topStr);

    // PM_CTX_RESTORE (clears pm_prepare_D3 in firmware)
    { UInt32 pm[3] = {12, 0x40020000, 0};
      UInt32 pmr = sendIpc(pm, 12);
      IOLog("MedionAudio: PM_CTX_RESTORE returned %u\n", pmr);
      char pmstr[32]; snprintf(pmstr, sizeof(pmstr), "PM_RESTORE=%u", pmr);
      setProperty("PM", pmstr);
    }

    if (!allocateDmaBuffers()) {
        IOLog("MedionAudio: FAILED to allocate shared/capture DMA buffers\n");
        return false;
    }

    IOLog("MedionAudio: DMA buffers allocated: sharedDmaBuf=0x%llx, capDmaBuf=0x%llx\n",
          (UInt64)sharedDmaBuf->getPhysicalAddress(), (UInt64)capDmaBuf->getPhysicalAddress());

    codec.silenceAmps();

    hwReady = true;
    setProperty("Status", "Ready");
    if (pciDevice) {
        pciDevice->setProperty("MedionAudio_Step", "4_DSP_Ready");
        pciDevice->setProperty("MedionAudioDSPStatus", "Active");
    }
    return true;
}

// ==================== Playback & Capture Control ====================

IOReturn MedionAudioDevice::startPlaybackGated() {
    if (!hwReady || !hdaBase || !dspBase || !sharedDmaBuf || !sharedBdlBuf) return kIOReturnNotReady;
    if (isPlaying) stopPlaybackGated();

    if (!codecConfigured) {
        if (codec.init() && codec.configure()) {
            codecConfigured = true;
            setProperty("Codec", "ES8326 (ESSX8326)");
            setProperty("CodecStatus", codec.getStatus());
        }
    }
    if (sTag == 0) sTag = 1;
    if (sd == 0) {
        if (sIdx == 0) sIdx = 7;
        sd = SD_BASE + (UInt32)sIdx * SD_SIZE;
    }

    UInt32 hostPeriodBytes = 16384;
    UInt32 numBdl = kCmlSOF_BufferSize / hostPeriodBytes;

    wr32(hdaBase, HDA_INTCTL, rd32(hdaBase, HDA_INTCTL) | (1U << 31) | (1U << 30) | (1U << sIdx));
    if (ppCap) {
        wr32(hdaBase, ppCap + PP_PPCTL, rd32(hdaBase, ppCap + PP_PPCTL) | (1U << sIdx));
    }

    streamReset(hdaBase, sd);
    streamReset(hdaBase, sd);

    { HdaBdlEntry *bdl = (HdaBdlEntry *)sharedBdlBuf->getBytesNoCopy();
      if (!bdl) return kIOReturnNoMemory;
      memset(bdl, 0, sharedBdlBuf->getLength());
      UInt64 phys = sharedDmaBuf->getPhysicalAddress();
      for (UInt32 i = 0; i < numBdl; i++) {
          bdl[i].addrLow  = (UInt32)((phys + i * hostPeriodBytes) & 0xFFFFFFFF);
          bdl[i].addrHigh = (UInt32)((phys + i * hostPeriodBytes) >> 32);
          bdl[i].size = hostPeriodBytes;
          bdl[i].ioc  = 1;
      }
    }
    UInt64 bdlPhys = sharedBdlBuf->getPhysicalAddress();

    wr8(hdaBase, sd + 2, (UInt8)((sTag & 0xF) << 4));
    wr32(hdaBase, sd + SD_REG_CBL, kCmlSOF_BufferSize);

    if (ppCap) {
        wr32(hdaBase, ppCap + PP_PPCTL, rd32(hdaBase, ppCap + PP_PPCTL) & ~(1U << sIdx));
    }
    wr16(hdaBase, sd + SD_REG_FMT, 0x0041); // 48kHz, 32-bit, 2ch (Linux)
    if (ppCap) {
        wr32(hdaBase, ppCap + PP_PPCTL, rd32(hdaBase, ppCap + PP_PPCTL) | (1U << sIdx));
    }

    wr16(hdaBase, sd + SD_REG_LVI, (UInt16)(numBdl - 1));
    wr32(hdaBase, sd + SD_REG_BDLPL, (UInt32)(bdlPhys & 0xFFFFFFFF));
    wr32(hdaBase, sd + SD_REG_BDLPU, (UInt32)(bdlPhys >> 32));

    /* DPLBASE enable with no allocated page meant the controller DMA-wrote the
     * position buffer to physical address 0. Positions come from LPIB, so keep
     * it disabled rather than pointing it at address 0. */
    { UInt32 dplbase = rd32(hdaBase, 0x70);
      if ((dplbase & ~1U) == 0 && (dplbase & 1U)) {
          wr32(hdaBase, 0x70, 0);
          wr32(hdaBase, 0x74, 0);
          IOLog("MedionAudio: cleared DPLBASE (was enabled at phys 0)\n");
      }
    }

    wr8(hdaBase, sd, rd8(hdaBase, sd) | SD_CTL_IOCE | 0x08 | 0x10);

    if (!(isCapturing && capActiveHost == PIPE2_HOST_ID))   /* SSP0 already running for headset capture */
    /* SSP0 DAI_CONFIG HW_PARAMS — starts physical BCLK/FS/MCLK on SSP0 pins.
     * Topology used flags=NONE. Non-fatal on
     * failure so host DMA still runs for diagnosis; acoustic out needs success. */
    {
      UInt8 m[216] = {};
      *(UInt32*)(m+0)=216; *(UInt32*)(m+4)=0x80010000;
      *(UInt32*)(m+8)=DAI_SSP; *(UInt32*)(m+12)=0;
      *(UInt16*)(m+16)=SOF_DAI_FMT_I2S_CBC_CFC;
      m[18]=0;
      m[19]=SOF_DAI_CONFIG_FLAGS_HW_PARAMS;
      UInt8 *ssp=m+52;
      *(UInt32*)(ssp+0)=SSP_PARAMS_SIZE;
      *(UInt32*)(ssp+4)=0;             /* reserved1 | mclk_id = 0 (NHLT mask 0x1) */
      *(UInt32*)(ssp+8)=SSP_GOLD_MCLK;
      *(UInt32*)(ssp+12)=48000;
      *(UInt32*)(ssp+16)=SSP_GOLD_BCLK;
      *(UInt32*)(ssp+20)=2;
      *(UInt32*)(ssp+24)=3;
      *(UInt32*)(ssp+28)=3;
      *(UInt32*)(ssp+32)=SSP_GOLD_BITS;
      *(UInt16*)(ssp+36)=SSP_GOLD_SLOTW;
      *(UInt32*)(ssp+40)=1;            /* MCLK is an input to the codec */
      *(UInt32*)(ssp+48)=SSP_CLKS_CONTROL;
      drainDspIpc(dspBase);
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < sizeof(m); i += 4) wr32(ob, i, *(UInt32*)(m + i));
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
          IOLog("MedionAudio: Playback SSP0 DAI_CONFIG HW_PARAMS timeout (clocks may be dead)\n");
          setProperty("SSP0Clocks", "HW_PARAMS timeout");
      } else {
          UInt32 replyErr = rd32(ob, 8);
          wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
          wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
          drainDspIpc(dspBase);
          if (replyErr != 0) {
              IOLog("MedionAudio: Playback SSP0 DAI_CONFIG HW_PARAMS replyErr=0x%x\n", replyErr);
              char errStr[48];
              snprintf(errStr, sizeof(errStr), "HW_PARAMS err=0x%x", replyErr);
              setProperty("SSP0Clocks", errStr);
          } else {
              IOLog("MedionAudio: Playback SSP0 DAI_CONFIG HW_PARAMS ok (MCLK_AON)\n");
              setProperty("SSP0Clocks", "HW_PARAMS ok");
          }
      }
    }

    // Send PCM_PARAMS to Pipeline 1
    { struct sof_ipc_pcm_params pcm = {};
      pcm.hdr_size = sizeof(pcm);
      pcm.hdr_cmd = 0x60010000;
      pcm.comp_id = PIPE1_HOST_ID;
      pcm.params_size = 84;
      pcm.buffer.hdr_size = 0;
      pcm.buffer.phy_addr = (UInt32)(sharedDmaBuf->getPhysicalAddress() & 0xFFFFFFFF);
      pcm.buffer.pages = numBdl;
      pcm.buffer.size = kCmlSOF_BufferSize;
      pcm.direction = DIR_PLAYBACK;
      pcm.frame_fmt = FRAME_S32;
      pcm.rate = 48000;
      pcm.stream_tag = (UInt16)sTag;
      pcm.channels = 2;
      pcm.sample_valid_bytes = 4;
      pcm.sample_container_bytes = 4;
      pcm.host_period_bytes = hostPeriodBytes;
      pcm.no_stream_position = 1;

      drainDspIpc(dspBase);
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < sizeof(pcm); i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&pcm + i));
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000))
          return kIOReturnTimeout;
      UInt32 replyErr = rd32(ob, 8);
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      drainDspIpc(dspBase);
      if (replyErr != 0) {
          IOLog("MedionAudio: Playback PCM_PARAMS failed with replyErr=0x%x\n", replyErr);
          return kIOReturnIOError;
      }
    }

    /* Disable SPIB for playback (Linux / CmlSOF). Enabling SPIB and letting HAL
     * UpdateSPIB write the ring write-cursor stalls DMA when LPIB catches SPIB
     * (observed: SPBFCCTL bit7, SPIB7≈LPIB≈0x4DF0, DPIB frozen). */
    if (spibCap) {
        wr32(hdaBase, spibCap + 0x04, rd32(hdaBase, spibCap + 0x04) & ~(1U << sIdx));
    }

    wr8(hdaBase, sd + SD_REG_STS, 0x1C);
    wr8(hdaBase, sd, rd8(hdaBase, sd) | SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10);
    IODelay(500);

    /* TRIG_START host only. TRIG on the DAI (PIPE1_DAI_ID) or scheduler
     * (PIPE1_SCHED_ID) is rejected by this FW and wedges the IPC mailbox
     * (replyErr 0xffffffff / poll timeout 0xe00002d6) — seen in alltone and
     * every StartPlayback after the S32/GPIO fix reboot. Linux lets the
     * pipeline scheduler start the DAI via the host trigger. */
    drainDspIpc(dspBase);
    wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
    wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
    IODelay(100);
    {
      struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, 0x60040000, PIPE1_HOST_ID};
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000))
          return kIOReturnTimeout;
      UInt32 replyErr = rd32(ob, 8);
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      if (replyErr != 0) {
          IOLog("MedionAudio: Playback TRIG_START host failed replyErr=0x%x\n", replyErr);
          return kIOReturnIOError;
      }
    }
    /* Re-assert the analog output + speaker amp. The topology-load path and
     * CoreAudio/I2C races can leave the DAC muted, so unmute first and enable
     * the external amp 70 ms later (anti-pop, as in sof_es8336). */
    if (codecConfigured) {
        codec.mute(false);
        IOSleep(70);
        codec.setSpeakerEnable(!codec.isHeadphoneInserted());
    }

    isPlaying = true;
    setProperty("Status", "Playing");
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::stopPlaybackGated() {
    if (!isPlaying) return kIOReturnSuccess;
    if (!hdaBase || !dspBase) {
        isPlaying = false;
        return kIOReturnNotReady;
    }

    /* Mute before stopping clocks to reduce clicks. startPlaybackGated() runs
     * the ES8326 unmute sequence again before enabling the speaker amplifier. */
    if (codecConfigured) {
        codec.mute(true);
        IOSleep(20);
        codec.setSpeakerEnable(false);
    }

    wr8(hdaBase, sd, rd8(hdaBase, sd) & ~(UInt8)(SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10));

    auto sendSimpleIpc = [&](UInt32 cmd) {
        drainDspIpc(dspBase);
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
        IODelay(100);
        struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, cmd, PIPE1_HOST_ID};
        volatile UInt8 *ob = dspBase + outboxOff;
        for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
        wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
        poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000);
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
        drainDspIpc(dspBase);
    };

    sendSimpleIpc(0x60050000); // TRIG_STOP
    sendSimpleIpc(0x60030000); // PCM_FREE

    /* Release SSP0 clocks (pair of startPlayback HW_PARAMS) — unless headset capture is running. */
    if (!(isCapturing && capActiveHost == PIPE2_HOST_ID))
    {
      UInt8 m[216] = {};
      *(UInt32*)(m+0)=216; *(UInt32*)(m+4)=0x80010000;
      *(UInt32*)(m+8)=DAI_SSP; *(UInt32*)(m+12)=0;
      *(UInt16*)(m+16)=SOF_DAI_FMT_I2S_CBC_CFC;
      m[18]=0; m[19]=SOF_DAI_CONFIG_FLAGS_HW_FREE;
      drainDspIpc(dspBase);
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < sizeof(m); i += 4) wr32(ob, i, *(UInt32*)(m + i));
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000);
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      drainDspIpc(dspBase);
      setProperty("SSP0Clocks", "HW_FREE");
    }

    if (sharedDmaBuf) {
        void *pb = sharedDmaBuf->getBytesNoCopy();
        if (pb) bzero(pb, kCmlSOF_BufferSize);
    }
    isPlaying = false;
    setProperty("Status", "Ready");
    return kIOReturnSuccess;
}

UInt32 MedionAudioDevice::getSamplePosition() {
    if (!hwReady || !hdaBase) return 0;
    UInt32 dpib = rd32(hdaBase, HDA_VS_SDXDPIB_XBASE + HDA_VS_SDXDPIB_XINTERVAL * (UInt32)sIdx);
    return dpib / kCmlSOF_BytesPerFrame;
}

IOReturn MedionAudioDevice::updateSPIB(UInt32 byteOffset) {
    /* SPIB disabled on playback start. Ignore HAL write-cursor updates —
     * shrinking SPIB to the mix offset stalls HDA DMA (LPIB stuck at SPIB). */
    (void)byteOffset;
    return (hwReady && isPlaying) ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn MedionAudioDevice::startCaptureGated() {
    if (!hwReady || !hdaBase || !dspBase || !capDmaBuf || !capBdlBuf) return kIOReturnNotReady;
    if (isCapturing) return kIOReturnSuccess;
    /* Both capture sources (internal MIC1 / headset MIC2) use the same
     * SSP0.IN pipeline; only the codec analog front-end changes. */
    const UInt32 capHost = PIPE2_HOST_ID;
    codec.setCaptureMic(captureSource == 1);

    /* Match BDL periods (4 × hostPeriod) — not PAGE_SIZE count (was pages=32 vs LVI=3). */
    UInt32 numPeriods = 4;
    UInt32 hostPeriodBytes = kCmlSOF_CapBufferSize / numPeriods;

    drainDspIpc(dspBase);
    wr32(hdaBase, HDA_INTCTL, rd32(hdaBase, HDA_INTCTL) | (1U << 31) | (1U << 30) | (1U << capIdx));

    streamReset(hdaBase, capSd);

    { HdaBdlEntry *bdl = (HdaBdlEntry *)capBdlBuf->getBytesNoCopy();
      if (!bdl) return kIOReturnNoMemory;
      memset(bdl, 0, capBdlBuf->getLength());
      UInt64 phys = capDmaBuf->getPhysicalAddress();
      for (UInt32 i = 0; i < numPeriods; i++) {
          bdl[i].addrLow  = (UInt32)((phys + i * hostPeriodBytes) & 0xFFFFFFFF);
          bdl[i].addrHigh = (UInt32)((phys + i * hostPeriodBytes) >> 32);
          bdl[i].size = hostPeriodBytes;
          bdl[i].ioc  = 1;
      }
    }
    UInt64 bdlPhys = capBdlBuf->getPhysicalAddress();

    /* Decouple while programming FMT (same dance as playback). */
    if (ppCap)
        wr32(hdaBase, ppCap + PP_PPCTL, rd32(hdaBase, ppCap + PP_PPCTL) & ~(1U << capIdx));

    wr8(hdaBase, capSd + 2, (UInt8)((capTag & 0xF) << 4));
    wr32(hdaBase, capSd + SD_REG_CBL, kCmlSOF_CapBufferSize);
    /* HDA SD_FMT: base 48 kHz, bits=32 (4<<4), chans-1=1 → 2ch S32. */
    wr16(hdaBase, capSd + SD_REG_FMT, 0x0041);
    wr16(hdaBase, capSd + SD_REG_LVI, (UInt16)(numPeriods - 1));
    wr32(hdaBase, capSd + SD_REG_BDLPL, (UInt32)(bdlPhys & 0xFFFFFFFF));
    wr32(hdaBase, capSd + SD_REG_BDLPU, (UInt32)(bdlPhys >> 32));

    if (ppCap)
        wr32(hdaBase, ppCap + PP_PPCTL, rd32(hdaBase, ppCap + PP_PPCTL) | (1U << capIdx));

    /* DPLBASE enable with no allocated page meant the controller DMA-wrote the
     * position buffer to physical address 0. Positions come from LPIB, so keep
     * it disabled rather than pointing it at address 0. */
    { UInt32 dplbase = rd32(hdaBase, 0x70);
      if ((dplbase & ~1U) == 0 && (dplbase & 1U)) {
          wr32(hdaBase, 0x70, 0);
          wr32(hdaBase, 0x74, 0);
          IOLog("MedionAudio: cleared DPLBASE (was enabled at phys 0)\n");
      }
    }

    /* SPIB not used for capture (Linux); ensure disabled so SPIB=0 cannot stall. */
    if (spibCap)
        wr32(hdaBase, spibCap + 0x04, rd32(hdaBase, spibCap + 0x04) & ~(1U << capIdx));

    wr8(hdaBase, capSd, rd8(hdaBase, capSd) | SD_CTL_IOCE | 0x08 | 0x10);

    /* SSP0 is shared with playback: only start its clocks if playback has
     * not already started them. */
    if (!isPlaying) {
        IOReturn r = sspDaiConfig(SOF_DAI_CONFIG_FLAGS_HW_PARAMS);
        if (r != kIOReturnSuccess) return r;
    }

    { struct sof_ipc_pcm_params pcm = {};
      pcm.hdr_size = sizeof(pcm);
      pcm.hdr_cmd = 0x60010000;
      pcm.comp_id = capHost;
      pcm.params_size = 84;
      pcm.buffer.hdr_size = 0;
      pcm.buffer.phy_addr = (UInt32)(capDmaBuf->getPhysicalAddress() & 0xFFFFFFFF);
      pcm.buffer.pages = numPeriods;
      pcm.buffer.size = kCmlSOF_CapBufferSize;
      pcm.direction = DIR_CAPTURE;
      pcm.frame_fmt = FRAME_S32;
      pcm.rate = 48000;
      pcm.stream_tag = (UInt16)capTag;
      pcm.channels = kCmlSOF_CapChannels;
      pcm.sample_valid_bytes = 4;
      pcm.sample_container_bytes = 4;
      pcm.host_period_bytes = hostPeriodBytes;
      pcm.no_stream_position = 1;

      drainDspIpc(dspBase);
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < sizeof(pcm); i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&pcm + i));
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000))
          return kIOReturnTimeout;
      UInt32 replyErr = rd32(ob, 8);
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      drainDspIpc(dspBase);
      if (replyErr != 0) {
          IOLog("MedionAudio: Capture PCM_PARAMS failed with replyErr=0x%x\n", replyErr);
          return kIOReturnIOError;
      }
    }

    wr8(hdaBase, capSd + SD_REG_STS, 0x1C);
    wr8(hdaBase, capSd, rd8(hdaBase, capSd) | SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10);
    IODelay(500);

    // TRIG_START
    drainDspIpc(dspBase);
    wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
    wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
    IODelay(100);
    { struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, 0x60040000, capHost};
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000))
          return kIOReturnTimeout;
      UInt32 replyErr = rd32(ob, 8);
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
      drainDspIpc(dspBase);
      if (replyErr != 0) {
          IOLog("MedionAudio: Capture TRIG_START failed with replyErr=0x%x\n", replyErr);
          return kIOReturnIOError;
      }
    }

    capActiveHost = capHost;
    setProperty("CaptureSource", captureSource == 1 ? "Headset MIC2/SSP0.IN"
                                                   : "Internal MIC1/SSP0.IN");
    isCapturing = true;
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::stopCaptureGated() {
    if (!isCapturing) return kIOReturnSuccess;
    if (!hdaBase || !dspBase) {
        isCapturing = false;
        return kIOReturnNotReady;
    }

    wr8(hdaBase, capSd, rd8(hdaBase, capSd) & ~(UInt8)(SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10));

    auto sendCapIpc = [&](UInt32 cmd) {
        drainDspIpc(dspBase);
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
        IODelay(100);
        struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, cmd, capActiveHost};
        volatile UInt8 *ob = dspBase + outboxOff;
        for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
        wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
        poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000);
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
        drainDspIpc(dspBase);
    };
    sendCapIpc(0x60050000); // TRIG_STOP
    sendCapIpc(0x60030000); // PCM_FREE

    isCapturing = false;                 /* before HW_FREE: playback may keep SSP0 */
    if (!isPlaying) sspDaiConfig(SOF_DAI_CONFIG_FLAGS_HW_FREE);

    if (capDmaBuf) {
        void *cb = capDmaBuf->getBytesNoCopy();
        if (cb) bzero(cb, kCmlSOF_CapBufferSize);
    }
    isCapturing = false;
    return kIOReturnSuccess;
}

UInt32 MedionAudioDevice::getCapturePosition() {
    if (!hwReady || !hdaBase) return 0;
    UInt32 dpib = rd32(hdaBase, HDA_VS_SDXDPIB_XBASE + HDA_VS_SDXDPIB_XINTERVAL * (UInt32)capIdx);
    return dpib / kCmlSOF_CapBytesPerFrame;
}

// ==================== Power Management & Command Gate ====================

void MedionAudioDevice::fastMute() {
    codec.mute(true);
    codec.silenceAmps();
    if (sharedDmaBuf) {
        void *pb = sharedDmaBuf->getBytesNoCopy();
        if (pb) bzero(pb, kCmlSOF_BufferSize);
    }
}

IOReturn MedionAudioDevice::sPowerInterestHandler(void *target, void *refCon,
                                                    UInt32 messageType, IOService *provider,
                                                    void *messageArg, vm_size_t argSize) {
    auto *dev = static_cast<MedionAudioDevice *>(target);
    if (!dev || !dev->commandGate) return kIOReturnSuccess;

    switch (messageType) {
        case kIOMessageSystemWillSleep:
            dev->commandGate->runAction(&MedionAudioDevice::s_handleWillSleep);
            break;
        case kIOPMMessageClamshellStateChange: {
            uintptr_t closed = (uintptr_t)messageArg & 0x01;
            dev->commandGate->runAction(&MedionAudioDevice::s_handleClamshellChange, (void *)closed);
            break;
        }
        default:
            break;
    }
    return kIOReturnSuccess;
}

void MedionAudioDevice::handleWillSleepGated() {
    fastMute();
}

void MedionAudioDevice::handleClamshellChangeGated(bool closed) {
    if (flagsBuf) {
        UInt8 *flags = (UInt8 *)flagsBuf->getBytesNoCopy();
        if (flags) flags[kCmlSOF_FlagOff_ClamshellMuted] = closed ? 1 : 0;
    }
    if (closed) {
        fastMute();
    }
}

IOReturn MedionAudioDevice::setPowerState(unsigned long ordinal, IOService *whatDevice) {
    if (commandGate) {
        return commandGate->runAction(&MedionAudioDevice::s_setPowerState, (void *)ordinal);
    }
    return setPowerStateGated(ordinal);
}

IOReturn MedionAudioDevice::setPowerStateGated(unsigned long ordinal) {
    if (ordinal == 0) {
        // Going to sleep
        fastMute();
        shutdownForSleepGated();
    } else {
        // Wake: BAR already mapped; DSP stays deferred until UserClient TriggerInitDSP
        codec.init();
        codec.configure(); /* configures route + DAC unmuted */
        codec.setCaptureMic(captureSource == 1);
    }
    return IOPMAckImplied;
}

void MedionAudioDevice::shutdownForSleepGated() {
    codec.mute(true);
    codec.silenceAmps();
    hwReady = false;
    isPlaying = false;
    isCapturing = false;
}

void MedionAudioDevice::shutdownDSPGated() {
    codec.mute(true);
    codec.silenceAmps();
    hwReady = false;
    isPlaying = false;
    isCapturing = false;
}

// IOCommandGate Static Wrappers
IOReturn MedionAudioDevice::s_setPowerState(OSObject *o, void *a0, void *, void *, void *) {
    return static_cast<MedionAudioDevice *>(o)->setPowerStateGated((unsigned long)a0);
}
IOReturn MedionAudioDevice::s_startPlayback(OSObject *o, void *, void *, void *, void *) {
    return static_cast<MedionAudioDevice *>(o)->startPlaybackGated();
}
IOReturn MedionAudioDevice::s_stopPlayback(OSObject *o, void *, void *, void *, void *) {
    return static_cast<MedionAudioDevice *>(o)->stopPlaybackGated();
}
IOReturn MedionAudioDevice::s_startCapture(OSObject *o, void *, void *, void *, void *) {
    return static_cast<MedionAudioDevice *>(o)->startCaptureGated();
}
IOReturn MedionAudioDevice::s_stopCapture(OSObject *o, void *, void *, void *, void *) {
    return static_cast<MedionAudioDevice *>(o)->stopCaptureGated();
}
IOReturn MedionAudioDevice::s_handleWillSleep(OSObject *o, void *, void *, void *, void *) {
    static_cast<MedionAudioDevice *>(o)->handleWillSleepGated();
    return kIOReturnSuccess;
}
IOReturn MedionAudioDevice::s_handleClamshellChange(OSObject *o, void *closed, void *, void *, void *) {
    static_cast<MedionAudioDevice *>(o)->handleClamshellChangeGated((bool)(uintptr_t)closed);
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::startPlayback() {
    if (commandGate) return commandGate->runAction(&MedionAudioDevice::s_startPlayback);
    return startPlaybackGated();
}
IOReturn MedionAudioDevice::stopPlayback() {
    if (commandGate) return commandGate->runAction(&MedionAudioDevice::s_stopPlayback);
    return stopPlaybackGated();
}
IOReturn MedionAudioDevice::startCapture() {
    if (commandGate) return commandGate->runAction(&MedionAudioDevice::s_startCapture);
    return startCaptureGated();
}
IOReturn MedionAudioDevice::stopCapture() {
    if (commandGate) return commandGate->runAction(&MedionAudioDevice::s_stopCapture);
    return stopCaptureGated();
}

// ==================== UserClient Implementation ====================

OSDefineMetaClassAndStructors(MedionAudioUserClient, IOUserClient)

const IOExternalMethodDispatch MedionAudioUserClient::sMethods[kCmlSOF_MethodCount] = {
    { (IOExternalMethodAction)&MedionAudioUserClient::sStart,            0, 0, 0, 0 }, // 0
    { (IOExternalMethodAction)&MedionAudioUserClient::sStop,             0, 0, 0, 0 }, // 1
    { (IOExternalMethodAction)&MedionAudioUserClient::sGetPos,           0, 0, 1, 0 }, // 2
    { (IOExternalMethodAction)&MedionAudioUserClient::sUpdateSPIB,       1, 0, 0, 0 }, // 3
    { (IOExternalMethodAction)&MedionAudioUserClient::sStartCap,         0, 0, 0, 0 }, // 4
    { (IOExternalMethodAction)&MedionAudioUserClient::sStopCap,          0, 0, 0, 0 }, // 5
    { (IOExternalMethodAction)&MedionAudioUserClient::sGetCapPos,        0, 0, 1, 0 }, // 6
    { (IOExternalMethodAction)&MedionAudioUserClient::sTriggerInitDSP,   0, 0, 0, 0 }, // 7
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadBAR0_8,       1, 0, 1, 0 }, // 8
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadBAR0_16,      1, 0, 1, 0 }, // 9
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadBAR0_32,      1, 0, 1, 0 }, // 10
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteBAR0_8,      2, 0, 0, 0 }, // 11
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteBAR0_16,     2, 0, 0, 0 }, // 12
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteBAR0_32,     2, 0, 0, 0 }, // 13
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadBAR4_8,       1, 0, 1, 0 }, // 14
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadBAR4_16,      1, 0, 1, 0 }, // 15
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadBAR4_32,      1, 0, 1, 0 }, // 16
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteBAR4_8,      2, 0, 0, 0 }, // 17
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteBAR4_16,     2, 0, 0, 0 }, // 18
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteBAR4_32,     2, 0, 0, 0 }, // 19
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadBAR4_64,      1, 0, 2, 0 }, // 20 lo,hi
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteBAR4_64,     3, 0, 0, 0 }, // 21 off,lo,hi
    { (IOExternalMethodAction)&MedionAudioUserClient::sReadGPIO32,       1, 0, 1, 0 }, // 22
    { (IOExternalMethodAction)&MedionAudioUserClient::sWriteGPIO32,      2, 0, 0, 0 }, // 23
    { (IOExternalMethodAction)&MedionAudioUserClient::sI2CWriteReg,      2, 0, 0, 0 }, // 24
    { (IOExternalMethodAction)&MedionAudioUserClient::sI2CReadReg,       1, 0, 1, 0 }, // 25
    { (IOExternalMethodAction)&MedionAudioUserClient::sGetPhysicalAddrs, 0, 0, 5, 0 }, // 26 play,cap,flags,fw,fwBdl
    { (IOExternalMethodAction)&MedionAudioUserClient::sForceResetDSP,    0, 0, 0, 0 }, // 27
    { (IOExternalMethodAction)&MedionAudioUserClient::sInitDSPFromStaging, 1, 0, 0, 0 }, // 28 stagedLen
    { (IOExternalMethodAction)&MedionAudioUserClient::sGetFwPhys,        0, 0, 1, 0 }, // 29
    { (IOExternalMethodAction)&MedionAudioUserClient::sSetFwStagingLen,  1, 0, 0, 0 }, // 30
    { (IOExternalMethodAction)&MedionAudioUserClient::sSetCaptureSource, 1, 0, 0, 0 }, // 31 0=internal MIC1 1=headset MIC2
    { (IOExternalMethodAction)&MedionAudioUserClient::sSetTuning,        2, 0, 0, 0 }, // 32 id,value
};

bool MedionAudioUserClient::initWithTask(task_t owningTask, void *sec, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, sec, type)) return false;
    clientTask = owningTask;
    device = nullptr;
    return true;
}

bool MedionAudioUserClient::start(IOService *provider) {
    if (!IOUserClient::start(provider)) return false;
    device = OSDynamicCast(MedionAudioDevice, provider);
    return (device != nullptr);
}

void MedionAudioUserClient::stop(IOService *provider) {
    device = nullptr;
    IOUserClient::stop(provider);
}

IOReturn MedionAudioUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn MedionAudioUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                      IOMemoryDescriptor **memory) {
    if (!device) return kIOReturnNotAttached;
    switch (type) {
        case kCmlSOF_MemPlayback: {
            IOBufferMemoryDescriptor *buf = device->getSharedBuffer();
            if (!buf) return kIOReturnNoMemory;
            buf->retain();
            *memory = buf;
            return kIOReturnSuccess;
        }
        case kCmlSOF_MemCapture: {
            IOBufferMemoryDescriptor *buf = device->getCaptureBuffer();
            if (!buf) return kIOReturnNoMemory;
            buf->retain();
            *memory = buf;
            return kIOReturnSuccess;
        }
        case kCmlSOF_MemFlags: {
            IOBufferMemoryDescriptor *buf = device->getFlagsBuffer();
            if (!buf) return kIOReturnNoMemory;
            buf->retain();
            *memory = buf;
            return kIOReturnSuccess;
        }
        case kCmlSOF_MemFirmware: {
            if (!device->allocateFwStaging()) return kIOReturnNoMemory;
            IOBufferMemoryDescriptor *buf = device->getFwStagingBuffer();
            if (!buf) return kIOReturnNoMemory;
            buf->retain();
            *memory = buf;
            return kIOReturnSuccess;
        }
        default:
            return kIOReturnBadArgument;
    }
}

IOReturn MedionAudioUserClient::externalMethod(uint32_t sel, IOExternalMethodArguments *args,
                                                IOExternalMethodDispatch *disp, OSObject *tgt,
                                                void *ref) {
    if (sel >= kCmlSOF_MethodCount) return kIOReturnBadArgument;
    disp = const_cast<IOExternalMethodDispatch *>(&sMethods[sel]);
    tgt = this;
    return IOUserClient::externalMethod(sel, args, disp, tgt, ref);
}

IOReturn MedionAudioUserClient::sStart(MedionAudioUserClient *t, void *, IOExternalMethodArguments *) {
    return t->device ? t->device->startPlayback() : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sStop(MedionAudioUserClient *t, void *, IOExternalMethodArguments *) {
    return t->device ? t->device->stopPlayback() : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sGetPos(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t->device) return kIOReturnNotAttached;
    a->scalarOutput[0] = t->device->getSamplePosition();
    a->scalarOutputCount = 1;
    return kIOReturnSuccess;
}
IOReturn MedionAudioUserClient::sUpdateSPIB(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t->device ? t->device->updateSPIB((UInt32)a->scalarInput[0]) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sStartCap(MedionAudioUserClient *t, void *, IOExternalMethodArguments *) {
    return t->device ? t->device->startCapture() : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sStopCap(MedionAudioUserClient *t, void *, IOExternalMethodArguments *) {
    return t->device ? t->device->stopCapture() : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sGetCapPos(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t->device) return kIOReturnNotAttached;
    a->scalarOutput[0] = t->device->getCapturePosition();
    a->scalarOutputCount = 1;
    return kIOReturnSuccess;
}
IOReturn MedionAudioUserClient::sTriggerInitDSP(MedionAudioUserClient *t, void *,
                                                  IOExternalMethodArguments *) {
    if (!t->device) return kIOReturnNotAttached;
    /* Force re-run: call ForceResetDSP first (clears hwReady). */
    bool ok = t->device->initDSP(false, false);
    if (ok) {
        t->device->setProperty("DSPStatus", "Active");
        if (t->device->pciDevice)
            t->device->pciDevice->setProperty("MedionAudioDSPStatus", "Active");
    }
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

static IOReturn ucReadBar(MedionAudioDevice *dev, UInt32 which, UInt32 width,
                          IOExternalMethodArguments *a) {
    if (!dev || !a || a->scalarInputCount < 1) return kIOReturnBadArgument;
    UInt64 val = 0;
    IOReturn r = (which == 0)
        ? dev->readBAR0(width, (UInt32)a->scalarInput[0], &val)
        : dev->readBAR4(width, (UInt32)a->scalarInput[0], &val);
    if (r != kIOReturnSuccess) return r;
    if (width == 8) {
        a->scalarOutput[0] = (UInt32)(val & 0xFFFFFFFF);
        a->scalarOutput[1] = (UInt32)(val >> 32);
        a->scalarOutputCount = 2;
    } else {
        a->scalarOutput[0] = val;
        a->scalarOutputCount = 1;
    }
    return kIOReturnSuccess;
}

static IOReturn ucWriteBar(MedionAudioDevice *dev, UInt32 which, UInt32 width,
                           IOExternalMethodArguments *a) {
    if (!dev || !a) return kIOReturnBadArgument;
    if (width == 8) {
        if (a->scalarInputCount < 3) return kIOReturnBadArgument;
        UInt64 v = ((UInt64)(UInt32)a->scalarInput[2] << 32) | (UInt32)a->scalarInput[1];
        return (which == 0)
            ? dev->writeBAR0(8, (UInt32)a->scalarInput[0], v)
            : dev->writeBAR4(8, (UInt32)a->scalarInput[0], v);
    }
    if (a->scalarInputCount < 2) return kIOReturnBadArgument;
    return (which == 0)
        ? dev->writeBAR0(width, (UInt32)a->scalarInput[0], a->scalarInput[1])
        : dev->writeBAR4(width, (UInt32)a->scalarInput[0], a->scalarInput[1]);
}

IOReturn MedionAudioUserClient::sReadBAR0_8(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucReadBar(t->device, 0, 1, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sReadBAR0_16(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucReadBar(t->device, 0, 2, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sReadBAR0_32(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucReadBar(t->device, 0, 4, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sWriteBAR0_8(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucWriteBar(t->device, 0, 1, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sWriteBAR0_16(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucWriteBar(t->device, 0, 2, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sWriteBAR0_32(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucWriteBar(t->device, 0, 4, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sReadBAR4_8(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucReadBar(t->device, 4, 1, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sReadBAR4_16(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucReadBar(t->device, 4, 2, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sReadBAR4_32(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucReadBar(t->device, 4, 4, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sWriteBAR4_8(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucWriteBar(t->device, 4, 1, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sWriteBAR4_16(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucWriteBar(t->device, 4, 2, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sWriteBAR4_32(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucWriteBar(t->device, 4, 4, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sReadBAR4_64(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucReadBar(t->device, 4, 8, a) : kIOReturnNotAttached;
}
IOReturn MedionAudioUserClient::sWriteBAR4_64(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    return t ? ucWriteBar(t->device, 4, 8, a) : kIOReturnNotAttached;
}

IOReturn MedionAudioUserClient::sReadGPIO32(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device || !a || a->scalarInputCount < 1) return kIOReturnBadArgument;
    UInt32 val = 0;
    IOReturn r = t->device->readGPIO32((UInt32)a->scalarInput[0], &val);
    if (r != kIOReturnSuccess) return r;
    a->scalarOutput[0] = val;
    a->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn MedionAudioUserClient::sWriteGPIO32(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device || !a || a->scalarInputCount < 2) return kIOReturnBadArgument;
    return t->device->writeGPIO32((UInt32)a->scalarInput[0], (UInt32)a->scalarInput[1]);
}

IOReturn MedionAudioUserClient::sI2CWriteReg(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device || !a || a->scalarInputCount < 2) return kIOReturnBadArgument;
    return t->device->i2cWriteReg((UInt8)a->scalarInput[0], (UInt8)a->scalarInput[1]);
}

IOReturn MedionAudioUserClient::sI2CReadReg(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device || !a || a->scalarInputCount < 1) return kIOReturnBadArgument;
    UInt8 val = 0;
    IOReturn r = t->device->i2cReadReg((UInt8)a->scalarInput[0], &val);
    if (r != kIOReturnSuccess) return r;
    a->scalarOutput[0] = val;
    a->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn MedionAudioUserClient::sGetPhysicalAddrs(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device || !a) return kIOReturnBadArgument;
    UInt64 play = 0, cap = 0, flags = 0, fw = 0;
    IOReturn r = t->device->getPhysicalAddrs(&play, &cap, &flags);
    if (r != kIOReturnSuccess) return r;
    (void)t->device->getFwPhys(&fw);
    a->scalarOutput[0] = play;
    a->scalarOutput[1] = cap;
    a->scalarOutput[2] = flags;
    a->scalarOutput[3] = fw;
    a->scalarOutput[4] = 0; /* fw BDL not retained after boot */
    a->scalarOutputCount = 5;
    return kIOReturnSuccess;
}

IOReturn MedionAudioUserClient::sForceResetDSP(MedionAudioUserClient *t, void *, IOExternalMethodArguments *) {
    return t && t->device ? t->device->forceResetDSP() : kIOReturnNotAttached;
}

IOReturn MedionAudioUserClient::sInitDSPFromStaging(MedionAudioUserClient *t, void *,
                                                      IOExternalMethodArguments *a) {
    if (!t || !t->device) return kIOReturnNotAttached;
    if (a && a->scalarInputCount >= 1) {
        UInt32 len = (UInt32)a->scalarInput[0];
        if (len > 0 && len <= kCmlSOF_FwStagingSize)
            t->device->fwStagingLen = len;
    }
    bool ok = t->device->initDSP(/*force=*/true, /*useStaging=*/true);
    if (ok) {
        t->device->setProperty("DSPStatus", "Active");
        if (t->device->pciDevice)
            t->device->pciDevice->setProperty("MedionAudioDSPStatus", "Active");
    }
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn MedionAudioUserClient::sGetFwPhys(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device || !a) return kIOReturnBadArgument;
    UInt64 fw = 0;
    IOReturn r = t->device->getFwPhys(&fw);
    if (r != kIOReturnSuccess) return r;
    a->scalarOutput[0] = fw;
    a->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn MedionAudioUserClient::sSetFwStagingLen(MedionAudioUserClient *t, void *,
                                                   IOExternalMethodArguments *a) {
    if (!t || !t->device || !a || a->scalarInputCount < 1) return kIOReturnBadArgument;
    return t->device->setFwStagingLen((UInt32)a->scalarInput[0]);
}

// ==================== Headset mic (SSP0.IN) ====================

/* SSP0 DAI_CONFIG (same payload as playback) with HW_PARAMS / HW_FREE flags. */
IOReturn MedionAudioDevice::sspDaiConfig(UInt8 flags) {
    if (!dspBase) return kIOReturnNotReady;
    UInt8 m[216] = {};
    *(UInt32*)(m+0)=216; *(UInt32*)(m+4)=0x80010000;
    *(UInt32*)(m+8)=DAI_SSP; *(UInt32*)(m+12)=0;
    *(UInt16*)(m+16)=SOF_DAI_FMT_I2S_CBC_CFC;
    m[18]=0; m[19]=flags;
    if (flags == SOF_DAI_CONFIG_FLAGS_HW_PARAMS) {
        UInt8 *ssp=m+52;
        *(UInt32*)(ssp+0)=SSP_PARAMS_SIZE;
        *(UInt32*)(ssp+4)=0;
        *(UInt32*)(ssp+8)=SSP_GOLD_MCLK;
        *(UInt32*)(ssp+12)=48000;
        *(UInt32*)(ssp+16)=SSP_GOLD_BCLK;
        *(UInt32*)(ssp+20)=2;
        *(UInt32*)(ssp+24)=3;
        *(UInt32*)(ssp+28)=3;
        *(UInt32*)(ssp+32)=SSP_GOLD_BITS;
        *(UInt16*)(ssp+36)=SSP_GOLD_SLOTW;
        *(UInt32*)(ssp+40)=1;
        *(UInt32*)(ssp+48)=SSP_CLKS_CONTROL;
    }
    drainDspIpc(dspBase);
    volatile UInt8 *ob = dspBase + outboxOff;
    for (UInt32 i = 0; i < sizeof(m); i += 4) wr32(ob, i, *(UInt32*)(m + i));
    wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
    wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
    wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
    if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) { drainDspIpc(dspBase); return kIOReturnTimeout; }
    UInt32 replyErr = rd32(ob, 8);
    wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
    wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x03);
    drainDspIpc(dspBase);
    if (replyErr) IOLog("MedionAudio: SSP0 DAI_CONFIG flags=%u (capture) replyErr=0x%x\n", flags, replyErr);
    return replyErr ? kIOReturnIOError : kIOReturnSuccess;
}

/* Switch internal MIC1 <-> headset MIC2; the SSP0.IN pipeline is shared, so a
 * running capture is restarted only to let the codec settle the analog path. */
IOReturn MedionAudioDevice::setCaptureSourceGated(UInt32 source) {
    if (source > 1) return kIOReturnBadArgument;
    bool wasCapturing = isCapturing;
    if (source == captureSource) {
        codec.setCaptureMic(source == 1);
        return kIOReturnSuccess;
    }
    if (wasCapturing) stopCaptureGated();
    captureSource = source;
    codec.setCaptureMic(source == 1);
    if (wasCapturing) return startCaptureGated();
    setProperty("CaptureSource", source ? "Headset MIC2/SSP0.IN"
                                       : "Internal MIC1/SSP0.IN");
    return kIOReturnSuccess;
}

IOReturn MedionAudioDevice::s_setCaptureSource(OSObject *o, void *src, void *, void *, void *) {
    return static_cast<MedionAudioDevice *>(o)->setCaptureSourceGated((UInt32)(uintptr_t)src);
}

IOReturn MedionAudioDevice::setCaptureSource(UInt32 source) {
    if (!commandGate) return kIOReturnNotReady;
    return commandGate->runAction(&MedionAudioDevice::s_setCaptureSource, (void *)(uintptr_t)source);
}

IOReturn MedionAudioUserClient::sSetCaptureSource(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device) return kIOReturnNotAttached;
    return t->device->setCaptureSource((UInt32)a->scalarInput[0]);
}

IOReturn MedionAudioUserClient::sSetTuning(MedionAudioUserClient *t, void *, IOExternalMethodArguments *a) {
    if (!t || !t->device) return kIOReturnNotAttached;
    UInt64 id = a->scalarInput[0];
    if (id >= 8) return kIOReturnBadArgument;
    t->device->tuning[id] = (UInt32)a->scalarInput[1];
    IOLog("MedionAudio: tuning[%llu] = %u (applies at next DSP init)\n", id, t->device->tuning[id]);
    return kIOReturnSuccess;
}
