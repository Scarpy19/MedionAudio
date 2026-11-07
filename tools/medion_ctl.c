#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <IOKit/IOKitLib.h>

/* Must match MedionAudioDevice.hpp selectors */
enum {
    kSel_StartPlayback  = 0,
    kSel_StopPlayback   = 1,
    kSel_GetPosition    = 2,
    kSel_UpdateSPIB     = 3,
    kSel_StartCapture   = 4,
    kSel_StopCapture    = 5,
    kSel_GetCapPosition = 6,
    kSel_TriggerInitDSP = 7,
    kSel_ReadBAR0_8     = 8,
    kSel_ReadBAR0_16    = 9,
    kSel_ReadBAR0_32    = 10,
    kSel_WriteBAR0_8    = 11,
    kSel_WriteBAR0_16   = 12,
    kSel_WriteBAR0_32   = 13,
    kSel_ReadBAR4_8     = 14,
    kSel_ReadBAR4_16    = 15,
    kSel_ReadBAR4_32    = 16,
    kSel_WriteBAR4_8    = 17,
    kSel_WriteBAR4_16   = 18,
    kSel_WriteBAR4_32   = 19,
    kSel_ReadBAR4_64    = 20,
    kSel_WriteBAR4_64   = 21,
    kSel_ReadGPIO32     = 22,
    kSel_WriteGPIO32    = 23,
    kSel_I2CWriteReg    = 24,
    kSel_I2CReadReg     = 25,
    kSel_GetPhysicalAddrs = 26,
    kSel_ForceResetDSP    = 27,
    kSel_InitDSPFromStaging = 28,
    kSel_GetFwPhys        = 29,
    kSel_SetFwStagingLen  = 30
};

enum {
    kMemPlayback = 0,
    kMemCapture  = 1,
    kMemFlags    = 2,
    kMemFirmware = 3
};

#define FW_STAGING_SIZE  (600u * 1024u)

/* SOF IPC mailbox / SSP (ABI 3.x) — mirrors dma_capture helpers */
#define OUTBOX              0x82000u
#define IPC_HIPCTDR         0xC0u
#define IPC_HIPCTDA         0xC4u
#define IPC_HIPCIDR         0xD0u
#define IPC_HIPCIDA         0xD4u
#define IPC_HIPCCTL         0xE8u
#define IPC_BUSY_DONE       (1u << 31)
#define DAI_SSP             1u
/* sof_ipc_dai_ssp_params.clks_control. The Linux topology for this board
 * carries no SSP vendor tokens, so every one of clks_control / quirks /
 * mclk_id / frame_pulse_width / bclk_delay is 0. The old (1u << 8) here was a
 * reserved bit, not MCLK_AON (which is BIT(4)). */
#define SOF_SSP_CLKS_CONTROL  0u

/* Speaker amplifier enable, from the MEDION E15415 DSDT:
 *   ESSX._CRS -> GpioIo "speakers-enable", GNUM(0x04030008) = group 3 pin 8.
 *   GPCL[3] = { PCR 0x006D0000, 24 pins, PADBAR 0x0600 } -> GPIO community 1
 *   (0xFD6D0000), pad index 8, PADCFG0 = 0x600 + 8*16 = 0x680.
 * Linux reports gpio-616 active-low but the machine driver inverts its DAPM
 * state: observed raw TXSTATE is 1 during playback and 0 at idle.
 * High 16 bits = community for the kext. */
#define GPIO_COMM0       0x00000000u
#define GPIO_COMM1       0x00010000u
#define GPIO_SPK_PADCFG  (GPIO_COMM1 | 0x0680u)   /* community 1, pad 8 */
#define CAP_BUFFER_SIZE  131072u  /* 2ch S32 × 16384 frames */
#define CAP_CHANNELS     2
#define ROM_STATUS       0x80000u
#define ROM_STATUS_EX    0x80004u
#define FSR_FW_ENTERED   0x5u

/* ES8326 register map — must match ES8326Codec.hpp (Linux es8326.h). */
#define ES8326_RESET            0x00
#define ES8326_CLK_CTL          0x01
#define ES8326_CLK_INV          0x02
#define ES8326_CLK_RESAMPLE     0x03
#define ES8326_CLK_DIV1         0x04
#define ES8326_CLK_DIV2         0x05
#define ES8326_CLK_DLL          0x06
#define ES8326_CLK_MUX          0x07
#define ES8326_CLK_ADC_SEL      0x08
#define ES8326_CLK_DAC_SEL      0x09
#define ES8326_CLK_ADC_OSR      0x0A
#define ES8326_CLK_DAC_OSR      0x0B
#define ES8326_CLK_DIV_CPC      0x0C
#define ES8326_CLK_DIV_BCLK     0x0D
#define ES8326_CLK_TRI          0x0E
#define ES8326_CLK_DIV_LRCK     0x0F
#define ES8326_CLK_VMIDS1       0x10
#define ES8326_CLK_VMIDS2       0x11
#define ES8326_CLK_CAL_TIME     0x12
#define ES8326_FMT              0x13
#define ES8326_DAC_MUTE         0x14
#define ES8326_ADC_MUTE         0x15
#define ES8326_ANA_PDN          0x16
#define ES8326_PGA_PDN          0x17
#define ES8326_VMIDSEL          0x18
#define ES8326_ANA_LP           0x19
#define ES8326_ANA_DMS          0x1A
#define ES8326_ANA_MICBIAS      0x1B
#define ES8326_ANA_VSEL         0x1C
#define ES8326_SYS_BIAS         0x1D
#define ES8326_BIAS_SW1         0x1E
#define ES8326_BIAS_SW2         0x1F
#define ES8326_BIAS_SW3         0x20
#define ES8326_BIAS_SW4         0x21
#define ES8326_VMIDLOW          0x22
#define ES8326_PGAGAIN          0x23
#define ES8326_HP_DRIVER        0x24
#define ES8326_DAC2HPMIX        0x25
#define ES8326_HP_VOL           0x26
#define ES8326_HP_CAL           0x27
#define ES8326_HP_DRIVER_REF    0x28
#define ES8326_ADC_SCALE        0x29
#define ES8326_ADC1_SRC         0x2A
#define ES8326_ADC2_SRC         0x2B
#define ES8326_ADC1_VOL         0x2C
#define ES8326_ADC2_VOL         0x2D
#define ES8326_ADC_RAMPRATE     0x2E
#define ES8326_ADC_DRE          0x2F
#define ES8326_ADC_DRE_GAIN     0x30
#define ES8326_ADC_DRE_GATE     0x31
#define ES8326_ALC_RECOVERY     0x32
#define ES8326_ALC_LEVEL        0x33
#define ES8326_ADC_HPFS1        0x34
#define ES8326_ADC_HPFS2        0x35
#define ES8326_ADC_EQ           0x36
#define ES8326_HP_OFFSET_CAL    0x4A
#define ES8326_HPL_OFFSET_INI   0x4B
#define ES8326_HPR_OFFSET_INI   0x4C
#define ES8326_DAC_DSM          0x4D
#define ES8326_DAC_RAMPRATE     0x4E
#define ES8326_DAC_VPPSCALE     0x4F
#define ES8326_DACL_VOL         0x50
#define ES8326_DRC_RECOVERY     0x53
#define ES8326_DRC_WINSIZE      0x54
#define ES8326_DAC_CROSSTALK    0x55
#define ES8326_HPJACK_TIMER     0x56
#define ES8326_HPDET_TYPE       0x57
#define ES8326_INT_SOURCE       0x58
#define ES8326_INTOUT_IO        0x59
#define ES8326_SDINOUT1_IO      0x5A
#define ES8326_SDINOUT23_IO     0x5B
#define ES8326_JACK_PULSE       0x5C
#define ES8326_DACR_VOL         0xF4
#define ES8326_SPKL_VOL         0xF5
#define ES8326_SPKR_VOL         0xF6
#define ES8326_HP_MISC          0xF7
#define ES8326_CTIA_OMTP_STA    0xF8
#define ES8326_PULLUP_CTL       0xF9
#define ES8326_CSM_I2C_STA      0xFA
#define ES8326_HPDET_STA        0xFB
#define ES8326_CSM_MUTE_STA     0xFC
#define ES8326_CHIP_ID1         0xFD
#define ES8326_CHIP_ID2         0xFE
#define ES8326_CHIP_VERSION     0xFF

#define ES8326_CSM_ON           (1 << 7)
#define ES8326_MUTE_MASK        (3 << 0)
#define ES8326_MUTE             (3 << 0)
#define ES8326_CLK_ON           (0x7f << 0)
#define ES8326_S24_LE           (0 << 2)
#define ES8326_MIC_SEL_MASK     (3 << 4)
#define ES8326_MIC1_SEL         (1 << 4)
#define ES8326_MIC2_SEL         (1 << 5)
#define ES8326_HP_OFF           0
#define ES8326_HP_FORCE_CAL     ((1 << 7) | (1 << 3))
#define ES8326_HP_ON            ((7 << 4) | (7 << 0))
#define ES8326_ADC_SRC_ANA      0
#define ES8326_HP_DET_SRC_PIN9  (1 << 4)
#define ES8326_HP_TYPE_AUTO     (1 << 0)
#define ES8326_INT_SRC_PIN9     (1 << 3)
#define ES8326_IO_INPUT         0
#define ES8326_HPINSERT_FLAG    (1 << 1)
#define ES8326_HPBUTTON_FLAG    (1 << 0)
#define ES8326_VERSION_B        (3 << 0)
#define ES8326_I2C_ADDR         0x19

static void print_cfprop(io_service_t svc, CFStringRef key, const char *label) {
    CFTypeRef prop = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    if (!prop) return;
    char buf[256] = {0};
    if (CFGetTypeID(prop) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)prop, buf, sizeof(buf), kCFStringEncodingUTF8);
        printf("  %s: %s\n", label, buf);
    } else if (CFGetTypeID(prop) == CFDataGetTypeID()) {
        CFDataRef d = (CFDataRef)prop;
        const UInt8 *bytes = CFDataGetBytePtr(d);
        CFIndex n = CFDataGetLength(d);
        if (bytes && n > 0 && n < (CFIndex)sizeof(buf)) {
            memcpy(buf, bytes, (size_t)n);
            buf[n < 255 ? n : 255] = 0;
            printf("  %s: %s\n", label, buf);
        }
    }
    CFRelease(prop);
}

static void dump_hdas_props(void) {
    CFMutableDictionaryRef hdasMatch = IOServiceNameMatching("HDAS");
    io_service_t hdas = IOServiceGetMatchingService(kIOMainPortDefault, hdasMatch);
    if (!hdas) {
        printf("[-] HDAS not found\n");
        return;
    }
    printf("[*] HDAS registry props:\n");
    print_cfprop(hdas, CFSTR("MedionAudioProbed"), "Probed");
    print_cfprop(hdas, CFSTR("MedionAudioStarted"), "Started");
    print_cfprop(hdas, CFSTR("MedionAudio_Step"), "Step");
    print_cfprop(hdas, CFSTR("MedionAudioDSPStatus"), "DSPStatus");
    print_cfprop(hdas, CFSTR("MedionAudioCodec"), "Codec");
    print_cfprop(hdas, CFSTR("MedionAudio_DSPFail"), "DSPFail");
    print_cfprop(hdas, CFSTR("MedionAudio_ROMStatus"), "ROMStatus");
    IOObjectRelease(hdas);
}

static void dump_device_props(io_service_t service) {
    printf("[*] MedionAudioDevice props:\n");
    print_cfprop(service, CFSTR("Status"), "Status");
    print_cfprop(service, CFSTR("DSPStatus"), "DSPStatus");
    print_cfprop(service, CFSTR("Codec"), "Codec");
    print_cfprop(service, CFSTR("CodecStatus"), "CodecStatus");
    print_cfprop(service, CFSTR("FW-Version"), "FW-Version");
    print_cfprop(service, CFSTR("Topology"), "Topology");
}

static uint32_t amp_padcfg(io_connect_t c);

static kern_return_t gpio_rd(io_connect_t c, uint32_t off, uint32_t *val) {
    uint64_t in = off, out = 0;
    uint32_t outCnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(c, kSel_ReadGPIO32, &in, 1, &out, &outCnt);
    if (kr == KERN_SUCCESS) *val = (uint32_t)out;
    return kr;
}

static kern_return_t gpio_wr(io_connect_t c, uint32_t off, uint32_t val) {
    uint64_t in[2] = { off, val };
    return IOConnectCallScalarMethod(c, kSel_WriteGPIO32, in, 2, NULL, NULL);
}

static int i2c_write_reg(io_connect_t c, uint8_t reg, uint8_t val) {
    uint64_t in[2] = { reg, val };
    kern_return_t k = IOConnectCallScalarMethod(c, kSel_I2CWriteReg, in, 2, NULL, NULL);
    if (k != KERN_SUCCESS) {
        printf("  [-] I2C[0x%02x]=0x%02x failed 0x%x\n", reg, val, k);
        return -1;
    }
    return 0;
}

static uint8_t i2c_read_reg(io_connect_t c, uint8_t reg) {
    uint64_t in = reg, out = 0;
    uint32_t outCnt = 1;
    if (IOConnectCallScalarMethod(c, kSel_I2CReadReg, &in, 1, &out, &outCnt) != KERN_SUCCESS)
        return 0xff;
    return (uint8_t)out;
}

/* ---- ES8326 codec helpers (Linux sound/soc/codecs/es8326.c) ---- */

/* Full init sequence (es8326_init + version-B calibration). Leaves the DAC
 * muted and the speaker amp off; call apply_codec_play() afterwards. */
static void apply_codec_init(io_connect_t c) {
    uint8_t version = i2c_read_reg(c, ES8326_CHIP_VERSION);

    i2c_write_reg(c, ES8326_RESET, 0x1f);
    i2c_write_reg(c, ES8326_VMIDSEL, 0x3e);
    i2c_write_reg(c, ES8326_ANA_LP, 0xf0);
    usleep(12000);
    i2c_write_reg(c, ES8326_HPJACK_TIMER, 0xd9);
    i2c_write_reg(c, ES8326_ANA_MICBIAS, 0xd8);
    i2c_write_reg(c, ES8326_HPDET_TYPE, 0x83);
    i2c_write_reg(c, ES8326_CLK_RESAMPLE, 0x05);
    i2c_write_reg(c, ES8326_CLK_DIV_CPC, 0x89);
    i2c_write_reg(c, ES8326_CLK_CTL, ES8326_CLK_ON);
    i2c_write_reg(c, ES8326_RESET, 0x17);
    i2c_write_reg(c, ES8326_HP_MISC, 0x3d);
    i2c_write_reg(c, ES8326_PULLUP_CTL, 0x00);
    i2c_write_reg(c, ES8326_HP_VOL, 0xc4);
    i2c_write_reg(c, ES8326_HP_DRIVER, 0xa7);
    usleep(3000);
    i2c_write_reg(c, ES8326_HP_DRIVER_REF, 0x23);
    i2c_write_reg(c, ES8326_HP_DRIVER_REF, 0x33);
    i2c_write_reg(c, ES8326_HP_DRIVER, 0xa1);
    i2c_write_reg(c, ES8326_CLK_INV, 0x00);
    i2c_write_reg(c, ES8326_CLK_VMIDS1, 0xc4);
    i2c_write_reg(c, ES8326_CLK_VMIDS2, 0x81);
    i2c_write_reg(c, ES8326_CLK_CAL_TIME, 0x00);

    if (version >= ES8326_VERSION_B) {
        i2c_write_reg(c, ES8326_CLK_INV, 0xc0);
        i2c_write_reg(c, ES8326_CLK_DIV1, 0x03);
        i2c_write_reg(c, ES8326_CLK_DLL, 0x30);
        i2c_write_reg(c, ES8326_CLK_MUX, 0xed);
        i2c_write_reg(c, ES8326_CLK_DAC_SEL, 0x08);
        i2c_write_reg(c, ES8326_CLK_TRI, 0xc1);
        i2c_write_reg(c, ES8326_DAC_MUTE, 0x03);
        i2c_write_reg(c, ES8326_ANA_VSEL, 0x7f);
        i2c_write_reg(c, ES8326_VMIDLOW, 0x23);
        i2c_write_reg(c, ES8326_DAC2HPMIX, 0x88);
        usleep(18000);
        i2c_write_reg(c, ES8326_HP_OFFSET_CAL, 0x8c);
        usleep(18000);
        i2c_write_reg(c, ES8326_RESET, 0xc0);
        usleep(18000);
        i2c_write_reg(c, ES8326_HP_OFFSET_CAL, ES8326_HP_OFF);
        if ((i2c_read_reg(c, ES8326_CSM_MUTE_STA) & 0xf0) != 0x40) usleep(50000);
        i2c_write_reg(c, ES8326_HP_CAL, 0xd4);
        usleep(200000);
        i2c_write_reg(c, ES8326_HP_CAL, 0x4d);
        usleep(200000);
        i2c_write_reg(c, ES8326_HP_CAL, ES8326_HP_OFF);
        uint8_t offL = i2c_read_reg(c, ES8326_HPL_OFFSET_INI);
        uint8_t offR = i2c_read_reg(c, ES8326_HPR_OFFSET_INI);
        i2c_write_reg(c, ES8326_HP_OFFSET_CAL, 0x8c);
        i2c_write_reg(c, ES8326_HPL_OFFSET_INI, offL);
        i2c_write_reg(c, ES8326_HPR_OFFSET_INI, offR);
        i2c_write_reg(c, ES8326_CLK_INV, 0x00);
    }

    i2c_write_reg(c, ES8326_DAC_CROSSTALK, 0xaa);
    i2c_write_reg(c, ES8326_DAC_RAMPRATE, 0x00);
    i2c_write_reg(c, ES8326_HP_CAL, 0x00);
    i2c_write_reg(c, ES8326_ANA_LP, 0xf0);
    i2c_write_reg(c, ES8326_ANA_VSEL, 0x7f);
    i2c_write_reg(c, ES8326_VMIDLOW, 0x03);
    i2c_write_reg(c, ES8326_DAC_DSM, 0x08);
    i2c_write_reg(c, ES8326_DAC_VPPSCALE, 0x15);

    uint8_t hptype = (uint8_t)(0x80 | ES8326_HP_DET_SRC_PIN9 | ES8326_HP_TYPE_AUTO);
    if (version < ES8326_VERSION_B) hptype |= 0x04;
    i2c_write_reg(c, ES8326_HPDET_TYPE, hptype);
    usleep(7000);
    i2c_write_reg(c, ES8326_ANA_MICBIAS,
                  (uint8_t)(i2c_read_reg(c, ES8326_ANA_MICBIAS) | 0x0c));
    usleep(60000);
    i2c_write_reg(c, ES8326_HPDET_TYPE,
                  (uint8_t)(i2c_read_reg(c, ES8326_HPDET_TYPE) & ~0x03));
    i2c_write_reg(c, ES8326_INTOUT_IO, 0x00);
    i2c_write_reg(c, ES8326_SDINOUT1_IO, ES8326_IO_INPUT);
    i2c_write_reg(c, ES8326_SDINOUT23_IO, ES8326_IO_INPUT);
    i2c_write_reg(c, ES8326_ANA_PDN, 0x00);
    i2c_write_reg(c, ES8326_RESET, ES8326_CSM_ON);
    i2c_write_reg(c, ES8326_PGAGAIN,
                  (uint8_t)((i2c_read_reg(c, ES8326_PGAGAIN) & ~ES8326_MIC_SEL_MASK) | ES8326_MIC1_SEL));
    i2c_write_reg(c, ES8326_DAC_MUTE, ES8326_MUTE);
    i2c_write_reg(c, ES8326_ADC_MUTE, 0x0f);
    i2c_write_reg(c, ES8326_CLK_DIV_LRCK, 0xff);
    i2c_write_reg(c, ES8326_ADC1_SRC, 0x44);
    i2c_write_reg(c, ES8326_ADC2_SRC, 0x66);
    i2c_write_reg(c, ES8326_ANA_MICBIAS,
                  (uint8_t)(i2c_read_reg(c, ES8326_ANA_MICBIAS) & ~0x0c));
    if (version > ES8326_VERSION_B) {
        i2c_write_reg(c, ES8326_ANA_MICBIAS,
                      (uint8_t)((i2c_read_reg(c, ES8326_ANA_MICBIAS) & ~0x73) | 0x10));
        i2c_write_reg(c, ES8326_VMIDSEL,
                      (uint8_t)(i2c_read_reg(c, ES8326_VMIDSEL) | 0x40));
    }
    usleep(200000);
    i2c_write_reg(c, ES8326_INT_SOURCE, ES8326_INT_SRC_PIN9);

    /* MCLK = 19.2 MHz, LRCK = 48 kHz (coeff_div_v3 {400, 48000, 19200000}). */
    i2c_write_reg(c, ES8326_CLK_DIV1,    version == 0 ? 0xE9 : 0xE4);
    i2c_write_reg(c, ES8326_CLK_DIV2,    0x04);
    i2c_write_reg(c, ES8326_CLK_DLL,     version == 0 ? 0x0F : 0x35);
    i2c_write_reg(c, ES8326_CLK_MUX,     0x6d);
    i2c_write_reg(c, ES8326_CLK_ADC_SEL, version == 0 ? 0x4A : 0xCA);
    i2c_write_reg(c, ES8326_CLK_DAC_SEL, 0x0A);
    i2c_write_reg(c, ES8326_CLK_ADC_OSR, 0x1F);
    i2c_write_reg(c, ES8326_CLK_DAC_OSR, 0x1F);

    i2c_write_reg(c, ES8326_FMT, ES8326_S24_LE);

    i2c_write_reg(c, ES8326_ADC1_SRC, 0x00);
    i2c_write_reg(c, ES8326_ADC2_SRC, 0x00);
    i2c_write_reg(c, ES8326_PGAGAIN,
                  (uint8_t)((i2c_read_reg(c, ES8326_PGAGAIN) & ~ES8326_MIC_SEL_MASK) | ES8326_MIC1_SEL));
    i2c_write_reg(c, ES8326_PGAGAIN,
                  (uint8_t)((i2c_read_reg(c, ES8326_PGAGAIN) & ~0x0f) | 0x04));
    i2c_write_reg(c, ES8326_ADC1_VOL, 0xbf);
    i2c_write_reg(c, ES8326_ADC2_VOL, 0xbf);
    i2c_write_reg(c, ES8326_DACL_VOL, 0xbf);
    i2c_write_reg(c, ES8326_DACR_VOL, 0xbf);
}

/* Playback unmute (Linux es8326_mute(mute=0)). */
static void apply_codec_play(io_connect_t c) {
    i2c_write_reg(c, ES8326_CLK_INV, 0x00);
    i2c_write_reg(c, ES8326_CLK_MUX, 0x6d);
    i2c_write_reg(c, ES8326_DAC_DSM,
                  (uint8_t)(i2c_read_reg(c, ES8326_DAC_DSM) | 0x01));
    usleep(2000);
    i2c_write_reg(c, ES8326_DAC_DSM,
                  (uint8_t)(i2c_read_reg(c, ES8326_DAC_DSM) & ~0x01));
    usleep(2000);
    i2c_write_reg(c, ES8326_HP_DRIVER_REF, 0x33);
    i2c_write_reg(c, ES8326_HP_DRIVER, 0xa1);
    i2c_write_reg(c, ES8326_HP_CAL, ES8326_HP_ON);
    i2c_write_reg(c, ES8326_DAC_MUTE, 0x00);
}

/* Playback mute (Linux es8326_mute(mute=1)). */
static void apply_codec_mute(io_connect_t c) {
    i2c_write_reg(c, ES8326_HP_CAL, ES8326_HP_OFF);
    i2c_write_reg(c, ES8326_DAC_MUTE, ES8326_MUTE);
    i2c_write_reg(c, ES8326_HP_DRIVER_REF, 0x03);
}

static void dump_play_snapshot(io_connect_t c) {
    static const uint8_t regs[] = {
        ES8326_RESET, ES8326_CLK_CTL, ES8326_CLK_DIV1, ES8326_CLK_DIV2,
        ES8326_CLK_DLL, ES8326_CLK_MUX, ES8326_CLK_ADC_SEL, ES8326_CLK_DAC_SEL,
        ES8326_CLK_ADC_OSR, ES8326_CLK_DAC_OSR, ES8326_FMT, ES8326_DAC_MUTE,
        ES8326_ADC_MUTE, ES8326_ANA_PDN, ES8326_PGAGAIN, ES8326_HP_DRIVER,
        ES8326_HP_CAL, ES8326_HP_DRIVER_REF, ES8326_DACL_VOL, ES8326_DACR_VOL,
        ES8326_HPDET_STA, ES8326_CHIP_VERSION
    };
    uint32_t pad = 0;
    gpio_rd(c, amp_padcfg(c), &pad);
    printf("==== dump-play ====\n");
    printf("GPIO C1/pad8=0x%08x (TX=%u)  [raw: 1=amp ON, 0=OFF]\n",
           pad, pad & 1u);
    printf("Target speakers: C1/pad8 TX=1 | 0x14=00 0x27=77 0xFB&2=jack\n");
    for (size_t i = 0; i < sizeof(regs); i++) {
        uint8_t v = i2c_read_reg(c, regs[i]);
        printf("I2C[0x%02x] = 0x%02x\n", regs[i], v);
    }
}

static uint32_t bar4_rd32(io_connect_t c, uint32_t off) {
    uint64_t in = off, out = 0;
    uint32_t outCnt = 1;
    if (IOConnectCallScalarMethod(c, kSel_ReadBAR4_32, &in, 1, &out, &outCnt) != KERN_SUCCESS)
        return 0;
    return (uint32_t)out;
}

static uint32_t bar0_rd32(io_connect_t c, uint32_t off) {
    uint64_t in = off, out = 0;
    uint32_t outCnt = 1;
    if (IOConnectCallScalarMethod(c, kSel_ReadBAR0_32, &in, 1, &out, &outCnt) != KERN_SUCCESS)
        return 0;
    return (uint32_t)out;
}

static uint16_t bar0_rd16(io_connect_t c, uint32_t off) {
    uint64_t in = off, out = 0;
    uint32_t outCnt = 1;
    if (IOConnectCallScalarMethod(c, kSel_ReadBAR0_16, &in, 1, &out, &outCnt) != KERN_SUCCESS)
        return 0;
    return (uint16_t)out;
}

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/*
 * HDA/DSP stream probe. Decisive question: does the DSP drain the host stream?
 * 48 kHz S16 stereo must advance LPIB at 192000 bytes/s. Much slower ⇒ the DSP
 * host copier is not pulling (pipeline not really running).
 */
static void probe_dma(io_connect_t c, double window) {
    uint16_t gcap = bar0_rd16(c, 0x00);
    int niss = (gcap >> 8) & 0xF, noss = (gcap >> 12) & 0xF;
    printf("GCAP=0x%04x NISS=%d NOSS=%d INTCTL=0x%08x\n",
           gcap, niss, noss, bar0_rd32(c, 0x20));

    uint32_t ppctl_off = 0, spib_off = 0;
    uint32_t off = bar0_rd32(c, 0x14) & 0xFFFFu;
    for (int i = 0; i < 8 && off; i++) {
        uint32_t v = bar0_rd32(c, off);
        uint32_t cid = (v >> 16) & 0xFFFu;
        printf("  cap@0x%04x id=%u\n", off, cid);
        if (cid == 3) ppctl_off = off + 4;
        if (cid == 4) spib_off = off;
        off = v & 0xFFFFu;
    }
    if (ppctl_off)
        printf("PPCTL@0x%04x = 0x%08x (GPROCEN=%u PIE=%u)\n", ppctl_off,
               bar0_rd32(c, ppctl_off), (bar0_rd32(c, ppctl_off) >> 30) & 1u,
               (bar0_rd32(c, ppctl_off) >> 31) & 1u);
    printf("DPLBASE=0x%08x DPUBASE=0x%08x%s\n", bar0_rd32(c, 0x70), bar0_rd32(c, 0x74),
           ((bar0_rd32(c, 0x70) & ~1u) == 0 && (bar0_rd32(c, 0x70) & 1u))
               ? "  <-- WARNING: position buffer at phys 0" : "");
    if (spib_off) printf("SPIB cap@0x%04x ctl=0x%08x\n", spib_off, bar0_rd32(c, spib_off + 4));

    int nsd = niss + noss;
    if (nsd <= 0 || nsd > 16) nsd = 16;
    int running[16], nrun = 0;
    for (int i = 0; i < nsd; i++) {
        uint32_t base = 0x80u + (uint32_t)i * 0x20u;
        uint32_t ctl = bar0_rd32(c, base);
        int run = (int)((ctl >> 1) & 1u);
        printf("  SD%-2d base=0x%03x CTL=0x%06x STS=0x%02x RUN=%d STRM=%u "
               "LPIB=0x%08x CBL=0x%08x LVI=%u FMT=0x%04x\n",
               i, base, ctl & 0xFFFFFFu, (ctl >> 24) & 0xFFu, run,
               (ctl >> 20) & 0xFu, bar0_rd32(c, base + 4), bar0_rd32(c, base + 8),
               bar0_rd16(c, base + 0x0C), bar0_rd16(c, base + 0x12));
        if (run && nrun < 16) running[nrun++] = i;
    }
    if (!nrun) { printf("[-] no stream has RUN=1 — start playback first\n"); return; }

    for (int k = 0; k < nrun; k++) {
        uint32_t base = 0x80u + (uint32_t)running[k] * 0x20u;
        uint32_t cbl = bar0_rd32(c, base + 8);
        if (!cbl) cbl = 0x10000u;
        /* Window must stay under one CBL wrap or the delta is ambiguous:
         * at 192000 B/s a 64 KB ring wraps every 341 ms. */
        double w = window;
        double maxw = (double)cbl / 192000.0 * 0.7;
        if (w > maxw) w = maxw;
        uint32_t a = bar0_rd32(c, base + 4);
        double t0 = now_sec();
        usleep((useconds_t)(w * 1e6));
        uint32_t b = bar0_rd32(c, base + 4);
        double dt = now_sec() - t0;
        uint32_t delta = (b >= a) ? (b - a) : (cbl - a + b);
        double rate = (double)delta / dt;
        printf("SD%-2d LPIB %u -> %u  delta=%u in %.3fs = %.0f B/s "
               "(%.1f%% of 192000 expected) %s\n",
               running[k], a, b, delta, dt, rate, 100.0 * rate / 192000.0,
               (rate > 172000.0 && rate < 212000.0) ? "OK (DSP drains at 48 kHz)"
                                                    : "<-- ANOMALY");
    }

    /* SOF SW-reg window: FW publishes its own stream positions here. All-zero
     * while the host DMA runs ⇒ the DSP side never started. */
    printf("SRAM win0 (SW regs @0x80000):");
    for (uint32_t i = 0; i < 16; i++) {
        if ((i % 8) == 0) printf("\n  +0x%03x:", i * 4);
        printf(" %08x", bar4_rd32(c, 0x80000u + i * 4));
    }
    printf("\n");
}

static void bar4_wr32(io_connect_t c, uint32_t off, uint32_t val) {
    uint64_t in[2] = { off, val };
    IOConnectCallScalarMethod(c, kSel_WriteBAR4_32, in, 2, NULL, NULL);
}

static void drain_hipct(io_connect_t c) {
    for (int i = 0; i < 64; i++) {
        uint32_t hipct = bar4_rd32(c, IPC_HIPCTDR);
        if (!(hipct & IPC_BUSY_DONE)) break;
        bar4_wr32(c, IPC_HIPCTDA, IPC_BUSY_DONE);
        bar4_wr32(c, IPC_HIPCTDR, hipct);
    }
}

static int send_ipc(io_connect_t c, const void *msg, size_t len, uint32_t *replyErr) {
    drain_hipct(c);
    for (int t = 0; t < 500; t++) {
        if (!(bar4_rd32(c, IPC_HIPCIDR) & IPC_BUSY_DONE)) break;
        drain_hipct(c);
        usleep(1000);
    }
    const uint8_t *p = (const uint8_t *)msg;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        memcpy(&w, p + i, (len - i >= 4) ? 4 : (len - i));
        bar4_wr32(c, OUTBOX + (uint32_t)i, w);
    }
    bar4_wr32(c, IPC_HIPCIDA, bar4_rd32(c, IPC_HIPCIDA) | IPC_BUSY_DONE);
    bar4_wr32(c, IPC_HIPCCTL, bar4_rd32(c, IPC_HIPCCTL) | 0x3u);
    bar4_wr32(c, IPC_HIPCIDR, IPC_BUSY_DONE);
    for (int t = 0; t < 1000; t++) {
        if (bar4_rd32(c, IPC_HIPCIDA) & IPC_BUSY_DONE) {
            uint32_t err = bar4_rd32(c, OUTBOX + 8);
            bar4_wr32(c, IPC_HIPCIDA, bar4_rd32(c, IPC_HIPCIDA) | IPC_BUSY_DONE);
            drain_hipct(c);
            if (replyErr) *replyErr = err;
            return 0;
        }
        drain_hipct(c);
        usleep(500);
    }
    if (replyErr) *replyErr = 0xffffffffu;
    return -1;
}

/* sof_ipc_dai_config: hdr 8 | type 8 | dai_index 12 | format 16 (u16) |
 * group_id 18 | flags 19 | reserved[8] 20..51 | union ssp @52.
 * sof_ipc_dai_ssp_params: hdr.size 0 | reserved1 4 | mclk_id 6 | mclk_rate 8 |
 * fsync_rate 12 | bclk_rate 16 | tdm_slots 20 | rx_slots 24 | tx_slots 28 |
 * sample_valid_bits 32 | tdm_slot_width 36 | mclk_direction 40 |
 * frame_pulse_width 44 | tdm_per_slot_padding 46 | clks_control 48 |
 * quirks 52 | bclk_delay 56  => sizeof 60.
 * fmt: 0x4001 = I2S|CBC_CFC (codec slave), 0x0001 = I2S|CBP_CFP (codec master).
 * mclk_direction 1 = MCLK is an input to the codec, i.e. SSP drives it
 * (tplg SND_SOC_TPLG_MCLK_CI). */
static void ssp0_fill_dai_msg_ex(uint8_t m[216], uint8_t flags, uint16_t fmt,
                                 uint32_t mclk, uint32_t bclk, uint32_t bits, uint16_t slot_w) {
    memset(m, 0, 216);
    *(uint32_t *)(m + 0) = 216;
    *(uint32_t *)(m + 4) = 0x80010000u; /* DAI_CONFIG */
    *(uint32_t *)(m + 8) = DAI_SSP;
    *(uint32_t *)(m + 12) = 0; /* ssp0 */
    *(uint16_t *)(m + 16) = fmt;
    m[18] = 0;
    m[19] = flags;
    uint8_t *ssp = m + 52;
    *(uint32_t *)(ssp + 0) = 60;  /* sizeof(sof_ipc_dai_ssp_params) */
    *(uint32_t *)(ssp + 8) = mclk;
    *(uint32_t *)(ssp + 12) = 48000u;
    *(uint32_t *)(ssp + 16) = bclk;
    *(uint32_t *)(ssp + 20) = 2;
    *(uint32_t *)(ssp + 24) = 3;
    *(uint32_t *)(ssp + 28) = 3;
    *(uint32_t *)(ssp + 32) = bits;
    *(uint16_t *)(ssp + 36) = slot_w;
    *(uint32_t *)(ssp + 40) = (fmt == 0x0001) ? 0u : 1u;
    *(uint32_t *)(ssp + 48) = SOF_SSP_CLKS_CONTROL;
}

/* Live Linux topology: SSP MCLK 24 MHz, BCLK 2.4 MHz, 25-bit slots.
 * The machine driver separately configures codec sysclk as 19.2 MHz. */
#define SSP_GOLD_MCLK   24000000u
#define SSP_GOLD_BCLK   2400000u
#define SSP_GOLD_BITS   24u
#define SSP_GOLD_SLOTW  25u

static void ssp0_fill_dai_msg(uint8_t m[216], uint8_t flags, uint16_t fmt) {
    ssp0_fill_dai_msg_ex(m, flags, fmt, SSP_GOLD_MCLK, SSP_GOLD_BCLK,
                         SSP_GOLD_BITS, SSP_GOLD_SLOTW);
}

static int ssp0_dai_config_flags_ex(io_connect_t c, uint8_t flags, uint16_t fmt,
                                    uint32_t mclk, uint32_t bclk, uint32_t bits, uint16_t slot_w,
                                    const char *tag) {
    uint8_t m[216];
    ssp0_fill_dai_msg_ex(m, flags, fmt, mclk, bclk, bits, slot_w);
    uint32_t err = 0;
    if (send_ipc(c, m, sizeof(m), &err) != 0 || err != 0) {
        printf("[-] SSP0 DAI_CONFIG %s failed replyErr=0x%x\n", tag, err);
        return -1;
    }
    printf("[+] SSP0 DAI_CONFIG %s ok (fmt=0x%04x mclk=%u bclk=%u bits=%u)\n",
           tag, fmt, mclk, bclk, bits);
    return 0;
}

static int ssp0_dai_config_flags(io_connect_t c, uint8_t flags, uint16_t fmt, const char *tag) {
    return ssp0_dai_config_flags_ex(c, flags, fmt, SSP_GOLD_MCLK, SSP_GOLD_BCLK,
                                    SSP_GOLD_BITS, SSP_GOLD_SLOTW, tag);
}

static int ipc_trig(io_connect_t c, uint32_t cmd, uint32_t comp_id, const char *tag) {
    uint32_t m[3] = { 12, cmd, comp_id };
    uint32_t err = 0;
    if (send_ipc(c, m, sizeof(m), &err) != 0 || err != 0) {
        printf("[-] %s comp=%u replyErr=0x%x\n", tag, comp_id, err);
        return -1;
    }
    printf("[+] %s comp=%u ok\n", tag, comp_id);
    return 0;
}

static int ssp0_dai_config_hw_params(io_connect_t c) {
    return ssp0_dai_config_flags(c, 0x01, 0x4001, "HW_PARAMS");
}

static int ssp0_dai_config_hw_free(io_connect_t c) {
    return ssp0_dai_config_flags(c, 0x02, 0x4001, "HW_FREE");
}

/* Community-1 PADBAR (0x600 on this board; read from the community window). */
static uint32_t c1_padbar(io_connect_t c) {
    uint32_t v = 0;
    if (gpio_rd(c, GPIO_COMM1 | 0x0Cu, &v) == KERN_SUCCESS) {
        v &= 0xFFFF;
        if (v >= 0x100u && v < 0x8000u) return v;
    }
    return 0x0600u;
}

/* speakers-enable = community 1, pad 8 -> 0x600 + 8*16 = 0x680. */
static uint32_t amp_padcfg(io_connect_t c) {
    return GPIO_COMM1 | (c1_padbar(c) + 8u * 16u);
}

/* Drive the speaker line: raw TXSTATE 1 during playback, 0 at idle. */
static kern_return_t gpio_set_amp(io_connect_t c, int on) {
    uint32_t off = amp_padcfg(c);
    uint32_t old = 0;
    kern_return_t kr = gpio_rd(c, off, &old);
    if (kr != KERN_SUCCESS) return kr;
    uint32_t val = (old & ~1u) | (on ? 1u : 0u);
    kr = gpio_wr(c, off, val);
    uint32_t verify = 0;
    gpio_rd(c, off, &verify);
    printf("  C1 pad8 @0x%x 0x%08x -> wr 0x%08x rd 0x%08x (amp=%s) kr=0x%x\n",
           off & 0xFFFF, old, val, verify, on ? "ON" : "OFF", kr);
    return kr;
}

/*
 * Map capture shared memory if not already mapped for this connection.
 * Returns 0 on success; leaves addr and size set. Caller must unmap.
 */
static int map_capture_if_needed(io_connect_t connect, mach_vm_address_t *addr,
                                 mach_vm_size_t *size) {
    if (*addr != 0 && *size >= CAP_BUFFER_SIZE) return 0;
    mach_vm_address_t a = 0;
    mach_vm_size_t s = 0;
    kern_return_t kr = IOConnectMapMemory64(connect, kMemCapture, mach_task_self(),
                                            &a, &s, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || a == 0) {
        printf("[-] IOConnectMapMemory(type=1 capture) failed: 0x%x\n", kr);
        return -1;
    }
    *addr = a;
    *size = s;
    printf("[+] capture DMA mapped addr=0x%llx size=%llu\n",
           (unsigned long long)a, (unsigned long long)s);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "  status                         Show IORegistry / connection status\n"
        "  init [force]                   TriggerInitDSP embedded; force => reset then init\n"
        "  resetdsp                       ForceResetDSP (power-gate + clear fail props)\n"
        "  loadfw <path.ri> [--boot]      Stage RI to mem type 3 + SetFwStagingLen;\n"
        "                                 --boot also calls InitDSPFromStaging\n"
        "  init-staging [len]             InitDSPFromStaging (selector 28)\n"
        "  fwphys                         Print FW staging physical address\n"
        "  phys                           Print play/cap/flags/fw phys addrs\n"
        "  mapcap                         Map capture DMA (IOConnectMapMemory type 1)\n"
        "  mute-speakers                  C1/pad8 TX=0 (amp OFF) + DAC mute (night default)\n"
        "  unmute-speakers                DAC unmute -> 70 ms -> C1/pad8 TX=1 (amp ON)\n"
        "  unmute-headphones              C1/pad8 TX=0 (amp OFF) + DAC unmute\n"
        "  wait-fsr [timeout_ms]          Poll BAR4 ROM_STATUS until FSR=5 (default 15000)\n"
        "  es8326-config                  Full ES8326 init (19.2 MHz/48k); DAC+amp muted\n"
        "  rdbar0 <8|16|32> <off>         Read BAR0\n"
        "  wrbar0 <8|16|32> <off> <val>   Write BAR0\n"
        "  rdbar4 <8|16|32|64> <off>      Read BAR4 (64 -> lo hi)\n"
        "  wrbar4 <8|16|32|64> <off> <val> [hi]\n"
        "  rdgpio <off>                   Read GPIO dword (community in high 16 bits)\n"
        "  wrgpio <off> <val>             Write GPIO dword (community in high 16 bits)\n"
        "  rdi2c <reg>                    Read ES8326 register\n"
        "  wri2c <reg> <val>              Write ES8326 register\n"
        "  start|stop|pos                 Playback helpers\n"
        "  ssp0-dai-config                SSP0 DAI_CONFIG HW_PARAMS (start clocks)\n"
        "  ssp0-dai-free                  SSP0 DAI_CONFIG HW_FREE (stop clocks)\n"
        "  ssp0-reclock                   HW_FREE then HW_PARAMS (force clock restart)\n"
        "  playtone [freq] [sec] [spk|hp] Sine + SSP0 clocks + amp route\n"
        "                                 optional spk|hp forces route (ignore jack)\n"
        "  dump-play                      I2C key regs + speaker GPIO pad\n"
        "  peek-play                      Inspect playback DMA sample peak\n"
        "  peek-capture [seconds]         Inspect capture channel levels (no recording)\n"
        "  calibrate [spk|hp]             Diff live regs vs ES8326 playback targets\n"
        "  probe-dma [sec]                HDA streams + LPIB rate vs 192000 B/s\n"
        "  capstart|capstop|cappos        Capture helpers (maps DMA if missing)\n",
        argv0);
}

static int parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || !end || *end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

static int open_client(io_service_t *outSvc, io_connect_t *outConn) {
    CFMutableDictionaryRef matching = IOServiceMatching("MedionAudioDevice");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (!service) return -1;
    io_connect_t connect = 0;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connect);
    if (kr != KERN_SUCCESS) {
        IOObjectRelease(service);
        return -2;
    }
    *outSvc = service;
    *outConn = connect;
    return 0;
}

static int width_to_sel_rd(int bar, int width) {
    if (bar == 0) {
        if (width == 8) return kSel_ReadBAR0_8;
        if (width == 16) return kSel_ReadBAR0_16;
        if (width == 32) return kSel_ReadBAR0_32;
    } else {
        if (width == 8) return kSel_ReadBAR4_8;
        if (width == 16) return kSel_ReadBAR4_16;
        if (width == 32) return kSel_ReadBAR4_32;
        if (width == 64) return kSel_ReadBAR4_64;
    }
    return -1;
}

static int width_to_sel_wr(int bar, int width) {
    if (bar == 0) {
        if (width == 8) return kSel_WriteBAR0_8;
        if (width == 16) return kSel_WriteBAR0_16;
        if (width == 32) return kSel_WriteBAR0_32;
    } else {
        if (width == 8) return kSel_WriteBAR4_8;
        if (width == 16) return kSel_WriteBAR4_16;
        if (width == 32) return kSel_WriteBAR4_32;
        if (width == 64) return kSel_WriteBAR4_64;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "status") == 0) {
        dump_hdas_props();
        io_service_t service = 0;
        io_connect_t connect = 0;
        int oc = open_client(&service, &connect);
        if (oc == -1) {
            printf("[-] MedionAudioDevice NOT in IORegistry\n");
            return 1;
        }
        if (oc == -2) {
            printf("[+] MedionAudioDevice present but UserClient open failed\n");
            dump_device_props(service);
            IOObjectRelease(service);
            return 1;
        }
        printf("[+] MedionAudioDevice + UserClient open (svc=0x%x conn=0x%x)\n",
               service, connect);
        dump_device_props(service);
        IOServiceClose(connect);
        IOObjectRelease(service);
        return 0;
    }

    io_service_t service = 0;
    io_connect_t connect = 0;
    int oc = open_client(&service, &connect);
    if (oc != 0) {
        printf("[-] Cannot open MedionAudioDevice UserClient (code %d)\n", oc);
        dump_hdas_props();
        return 1;
    }

    kern_return_t kr = KERN_SUCCESS;
    int rc = 0;
    mach_vm_address_t capMapAddr = 0;
    mach_vm_size_t capMapSize = 0;

    if (strcmp(cmd, "init") == 0) {
        int doForce = 0;
        if (argc >= 3) {
            uint64_t tmp = 0;
            if (parse_u64(argv[2], &tmp) == 0 && tmp) doForce = 1;
            else if (strcmp(argv[2], "force") == 0) doForce = 1;
        }
        if (doForce) {
            printf("[*] ForceResetDSP before init...\n");
            kr = IOConnectCallScalarMethod(connect, kSel_ForceResetDSP, NULL, 0, NULL, NULL);
            printf("[%c] resetdsp -> 0x%x\n", kr == KERN_SUCCESS ? '+' : '-', kr);
        }
        printf("[*] TriggerInitDSP...\n");
        kr = IOConnectCallScalarMethod(connect, kSel_TriggerInitDSP, NULL, 0, NULL, NULL);
        printf("[%c] initDSP -> 0x%x\n", kr == KERN_SUCCESS ? '+' : '-', kr);
        dump_device_props(service);
        dump_hdas_props();
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "resetdsp") == 0) {
        printf("[*] ForceResetDSP...\n");
        kr = IOConnectCallScalarMethod(connect, kSel_ForceResetDSP, NULL, 0, NULL, NULL);
        printf("[%c] resetdsp -> 0x%x\n", kr == KERN_SUCCESS ? '+' : '-', kr);
        dump_device_props(service);
        dump_hdas_props();
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "init-staging") == 0) {
        uint64_t stagedLen = 0;
        uint32_t inCnt = 0;
        if (argc >= 3) {
            if (parse_u64(argv[2], &stagedLen) != 0) {
                fprintf(stderr, "bad len\n"); rc = 1; goto done;
            }
            inCnt = 1;
        }
        printf("[*] InitDSPFromStaging(len=%llu)...\n", (unsigned long long)stagedLen);
        kr = IOConnectCallScalarMethod(connect, kSel_InitDSPFromStaging,
                                       inCnt ? &stagedLen : NULL, inCnt, NULL, NULL);
        printf("[%c] init-staging -> 0x%x\n", kr == KERN_SUCCESS ? '+' : '-', kr);
        dump_device_props(service);
        dump_hdas_props();
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "fwphys") == 0) {
        uint64_t out = 0;
        uint32_t outCnt = 1;
        kr = IOConnectCallScalarMethod(connect, kSel_GetFwPhys, NULL, 0, &out, &outCnt);
        if (kr == KERN_SUCCESS)
            printf("fw_staging_phys = 0x%llx\n", (unsigned long long)out);
        else {
            printf("[-] fwphys failed: 0x%x (needs rebuilt kext)\n", kr);
            rc = 1;
        }
    } else if (strcmp(cmd, "loadfw") == 0) {
        if (argc < 3) { usage(argv[0]); rc = 1; goto done; }
        const char *path = argv[2];
        int doBoot = 0;
        for (int ai = 3; ai < argc; ai++) {
            if (strcmp(argv[ai], "--boot") == 0) doBoot = 1;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("[-] open(%s): %s\n", path, strerror(errno));
            rc = 1; goto done;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            printf("[-] bad firmware file size\n");
            close(fd); rc = 1; goto done;
        }
        if ((size_t)st.st_size > FW_STAGING_SIZE) {
            printf("[-] firmware %lld bytes exceeds staging %u\n",
                   (long long)st.st_size, FW_STAGING_SIZE);
            close(fd); rc = 1; goto done;
        }
        void *fileMap = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (fileMap == MAP_FAILED) {
            printf("[-] mmap firmware: %s\n", strerror(errno));
            rc = 1; goto done;
        }

        mach_vm_address_t fwAddr = 0;
        mach_vm_size_t fwSize = 0;
        kr = IOConnectMapMemory64(connect, kMemFirmware, mach_task_self(),
                                  &fwAddr, &fwSize, kIOMapAnywhere);
        if (kr != KERN_SUCCESS || fwAddr == 0) {
            printf("[-] IOConnectMapMemory(type=3 firmware) failed: 0x%x\n", kr);
            munmap(fileMap, (size_t)st.st_size);
            rc = 1; goto done;
        }
        if (fwSize < (mach_vm_size_t)st.st_size) {
            printf("[-] staging map too small (%llu < %lld)\n",
                   (unsigned long long)fwSize, (long long)st.st_size);
            IOConnectUnmapMemory64(connect, kMemFirmware, mach_task_self(), fwAddr);
            munmap(fileMap, (size_t)st.st_size);
            rc = 1; goto done;
        }

        memset((void *)(uintptr_t)fwAddr, 0, (size_t)fwSize);
        memcpy((void *)(uintptr_t)fwAddr, fileMap, (size_t)st.st_size);
        munmap(fileMap, (size_t)st.st_size);

        uint32_t sig = *(uint32_t *)(uintptr_t)fwAddr;
        printf("[+] staged %lld bytes into mem type 3 (sig=0x%08x map=%llu)\n",
               (long long)st.st_size, sig, (unsigned long long)fwSize);

        uint64_t stagedLen = (uint64_t)st.st_size;
        kr = IOConnectCallScalarMethod(connect, kSel_SetFwStagingLen,
                                       &stagedLen, 1, NULL, NULL);
        if (kr != KERN_SUCCESS) {
            /* Fallback for older kext without selector 30: boot path sets len via 28 */
            printf("[!] SetFwStagingLen failed 0x%x — falling back to InitDSPFromStaging\n", kr);
            doBoot = 1;
        } else {
            printf("[+] fwStagingLen=%llu\n", (unsigned long long)stagedLen);
        }

        if (doBoot) {
            kr = IOConnectCallScalarMethod(connect, kSel_InitDSPFromStaging,
                                           &stagedLen, 1, NULL, NULL);
            printf("[%c] InitDSPFromStaging(len=%llu) -> 0x%x\n",
                   kr == KERN_SUCCESS ? '+' : '-', (unsigned long long)stagedLen, kr);
            dump_device_props(service);
            dump_hdas_props();
        } else {
            printf("[*] staged only — run: %s init-staging\n", argv[0]);
        }
        IOConnectUnmapMemory64(connect, kMemFirmware, mach_task_self(), fwAddr);
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "phys") == 0) {
        uint64_t out[5] = {0};
        uint32_t outCnt = 5;
        kr = IOConnectCallScalarMethod(connect, kSel_GetPhysicalAddrs, NULL, 0, out, &outCnt);
        if (kr == KERN_SUCCESS) {
            printf("playback_phys = 0x%llx\n", (unsigned long long)out[0]);
            printf("capture_phys  = 0x%llx\n", (unsigned long long)out[1]);
            printf("flags_phys    = 0x%llx\n", (unsigned long long)out[2]);
            printf("fw_phys       = 0x%llx\n", (unsigned long long)out[3]);
        } else {
            printf("[-] phys failed: 0x%x\n", kr);
            rc = 1;
        }
    } else if (strcmp(cmd, "mapcap") == 0) {
        rc = (map_capture_if_needed(connect, &capMapAddr, &capMapSize) == 0) ? 0 : 1;
        if (rc == 0) {
            /* Keep mapping briefly so callers can observe; then unmap before exit. */
            usleep(10000);
        }
    } else if (strcmp(cmd, "wait-fsr") == 0) {
        /* Poll BAR4 ROM_STATUS until FSR_FW_ENTERED; exit 0 only on success. */
        int timeout_ms = 15000;
        if (argc >= 3) {
            uint64_t t = 0;
            if (parse_u64(argv[2], &t) != 0 || t < 100 || t > 120000) {
                fprintf(stderr, "bad timeout_ms (100..120000)\n");
                rc = 1;
                goto done;
            }
            timeout_ms = (int)t;
        }
        printf("[*] wait-fsr timeout=%d ms (expect FSR=%u, CapChannels=%d)\n",
               timeout_ms, FSR_FW_ENTERED, CAP_CHANNELS);
        int entered = 0, cse_halt = 0, waited = 0;
        uint32_t rom = 0, ex = 0;
        for (waited = 0; waited <= timeout_ms; waited += 100) {
            uint64_t in = ROM_STATUS, out = 0;
            uint32_t n = 1;
            kr = IOConnectCallScalarMethod(connect, kSel_ReadBAR4_32, &in, 1, &out, &n);
            if (kr != KERN_SUCCESS) {
                fprintf(stderr, "[-] rdbar4 ROM_STATUS failed: 0x%x\n", kr);
                rc = 1;
                goto done;
            }
            rom = (uint32_t)out;
            in = ROM_STATUS_EX; out = 0; n = 1;
            IOConnectCallScalarMethod(connect, kSel_ReadBAR4_32, &in, 1, &out, &n);
            ex = (uint32_t)out;
            uint32_t state = rom & 0xFFFFFFu;
            if (state == FSR_FW_ENTERED) {
                entered = 1;
                printf("[+] wait-fsr: FSR_FW_ENTERED after %d ms\n", waited);
                break;
            }
            if (waited == 0 || (waited % 1000) == 0) {
                printf("[*] wait-fsr %d/%d ms ROM=0x%08x state=%u EX=0x%x\n",
                       waited, timeout_ms, rom, state, ex);
            }
            if ((rom & 0x80000000U) && (state == 0x12u)) {
                cse_halt = 1;
                fprintf(stderr,
                        "[-] CSE halt ROM=0x%08x EX=0x%x — community FW; "
                        "need EFI intel-signed kext (post-reboot)\n", rom, ex);
                break;
            }
            usleep(100000);
        }
        printf("ROM_STATUS=0x%08x\nROM_STATUS_EX=0x%08x\nFSR_STATE=%u\n"
               "FSR_FW_ENTERED=%d\nFSR_CSE_HALT=%d\nFSR_WAIT_MS=%d\nCAP_CHANNELS=%d\n",
               rom, ex, rom & 0xFFFFFF, entered ? 1 : 0, cse_halt, waited, CAP_CHANNELS);
        if (!entered) {
            fprintf(stderr,
                    "[-] wait-fsr failed — run: medion_ctl resetdsp && medion_ctl init\n"
                    "    (or sof_boot / loadfw --boot) after reboot with intel-signed FW\n");
        }
        rc = entered ? 0 : 1;
    } else if (strcmp(cmd, "calibrate") == 0) {
        /* Diff live codec + speaker GPIO against the ES8326 playback targets. */
        int want_spk = 1;
        if (argc >= 3) {
            if (strcmp(argv[2], "hp") == 0 || strcmp(argv[2], "headphones") == 0)
                want_spk = 0;
            else if (strcmp(argv[2], "spk") == 0 || strcmp(argv[2], "speakers") == 0)
                want_spk = 1;
        }
        struct { uint8_t reg; uint8_t exp; const char *name; } gold[] = {
            { ES8326_DAC_MUTE,       0x00, "DAC_MUTE" },
            { ES8326_HP_CAL,         ES8326_HP_ON, "HP_CAL" },
            { ES8326_HP_DRIVER,      0xa1, "HP_DRIVER" },
            { ES8326_HP_DRIVER_REF,  0x33, "HP_DRIVER_REF" },
            { ES8326_CLK_INV,        0x00, "CLK_INV" },
            { ES8326_CLK_MUX,        0x6d, "CLK_MUX" },
            { ES8326_CLK_DIV1,       0xE9, "CLK_DIV1" },
            { ES8326_CLK_DIV2,       0x04, "CLK_DIV2" },
            { ES8326_CLK_DLL,        0x0F, "CLK_DLL" },
            { ES8326_CLK_ADC_SEL,    0x4A, "CLK_ADC_SEL" },
            { ES8326_CLK_DAC_SEL,    0x0A, "CLK_DAC_SEL" },
            { ES8326_CLK_ADC_OSR,    0x1F, "CLK_ADC_OSR" },
            { ES8326_CLK_DAC_OSR,    0x1F, "CLK_DAC_OSR" },
            { ES8326_FMT,            ES8326_S24_LE, "FMT" },
        };
        uint32_t pad = 0;
        gpio_rd(connect, amp_padcfg(connect), &pad);
        printf("==== calibrate %s (ES8326 19.2 MHz / 48 kHz) ====\n",
               want_spk ? "SPEAKERS" : "HEADPHONES");
        printf("GPIO C1/pad8 TX=%u  expect %s (raw: 1=amp ON)\n",
               pad & 1u, want_spk ? "1 (amp ON)" : "0 (amp OFF)");
        int mismatches = 0;
        if (want_spk) {
            if ((pad & 1u) != 1) { printf("  GPIO MISMATCH\n"); mismatches++; }
        } else {
            if ((pad & 1u) != 0) { printf("  GPIO MISMATCH\n"); mismatches++; }
        }
        for (size_t i = 0; i < sizeof(gold)/sizeof(gold[0]); i++) {
            uint8_t live = i2c_read_reg(connect, gold[i].reg);
            int ok = (live == gold[i].exp);
            if (!ok) mismatches++;
            printf("  %s I2C[0x%02x] live=0x%02x expect=0x%02x %s\n",
                   gold[i].name, gold[i].reg, live, gold[i].exp, ok ? "OK" : "DIFF");
        }
        printf("CALIBRATE_%s mismatches=%d\n", want_spk ? "SPK" : "HP", mismatches);
        rc = mismatches ? 1 : 0;
    } else if (strcmp(cmd, "mute-speakers") == 0) {
        printf("[*] mute-speakers: C1/pad8 TX=0 (amp OFF), DAC mute\n");
        kern_return_t k = gpio_set_amp(connect, 0);
        apply_codec_mute(connect);
        uint32_t pad = 0;
        gpio_rd(connect, amp_padcfg(connect), &pad);
        rc = (k == KERN_SUCCESS) ? 0 : 1;
        printf("MUTE_OK=%d CAP_CHANNELS=%d GPIO_PAD=0x%08x DAC_MUTE=0x%02x\n",
               rc == 0 ? 1 : 0, CAP_CHANNELS, pad,
               i2c_read_reg(connect, ES8326_DAC_MUTE));
        printf("[%c] mute-speakers\n", rc == 0 ? '+' : '-');
    } else if (strcmp(cmd, "unmute-speakers") == 0) {
        printf("[*] unmute-speakers: DAC unmute -> 70 ms -> C1/pad8 TX=1 (amp ON)\n");
        apply_codec_play(connect);
        usleep(70000);
        kern_return_t k = gpio_set_amp(connect, 1);
        uint32_t pad = 0;
        gpio_rd(connect, amp_padcfg(connect), &pad);
        rc = (k == KERN_SUCCESS) ? 0 : 1;
        printf("UNMUTE_OK=%d GPIO_PAD=0x%08x DAC_MUTE=0x%02x HP_CAL=0x%02x\n",
               rc == 0 ? 1 : 0, pad,
               i2c_read_reg(connect, ES8326_DAC_MUTE),
               i2c_read_reg(connect, ES8326_HP_CAL));
        printf("[%c] unmute-speakers\n", rc == 0 ? '+' : '-');
    } else if (strcmp(cmd, "unmute-headphones") == 0) {
        printf("[*] unmute-headphones: C1/pad8 TX=0 (amp OFF), DAC unmute\n");
        kern_return_t k = gpio_set_amp(connect, 0);
        apply_codec_play(connect);
        uint32_t pad = 0;
        gpio_rd(connect, amp_padcfg(connect), &pad);
        rc = (k == KERN_SUCCESS) ? 0 : 1;
        printf("UNMUTE_OK=%d GPIO_PAD=0x%08x DAC_MUTE=0x%02x HP_CAL=0x%02x\n",
               rc == 0 ? 1 : 0, pad,
               i2c_read_reg(connect, ES8326_DAC_MUTE),
               i2c_read_reg(connect, ES8326_HP_CAL));
        printf("[%c] unmute-headphones\n", rc == 0 ? '+' : '-');
    } else if (strcmp(cmd, "es8326-config") == 0) {
        printf("[*] es8326-config: full ES8326 init (19.2 MHz/48k); DAC muted, amp OFF\n");
        apply_codec_init(connect);
        apply_codec_mute(connect);
        gpio_set_amp(connect, 0);
        uint32_t pad = 0;
        gpio_rd(connect, amp_padcfg(connect), &pad);
        printf("I2C 0x14=%02x 0x27=%02x 0xFF=%02x GPIO_PAD=0x%08x\n",
               i2c_read_reg(connect, ES8326_DAC_MUTE),
               i2c_read_reg(connect, ES8326_HP_CAL),
               i2c_read_reg(connect, ES8326_CHIP_VERSION), pad);
        rc = 0;
        printf("[+] es8326-config\n");
    } else if (strcmp(cmd, "start") == 0) {
        kr = IOConnectCallScalarMethod(connect, kSel_StartPlayback, NULL, 0, NULL, NULL);
        printf("start -> 0x%x\n", kr); rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "ssp0-dai-config") == 0) {
        rc = ssp0_dai_config_hw_params(connect);
    } else if (strcmp(cmd, "ssp0-dai-free") == 0) {
        rc = ssp0_dai_config_hw_free(connect);
    } else if (strcmp(cmd, "ssp0-reclock") == 0) {
        rc = ssp0_dai_config_hw_free(connect);
        if (rc == 0) rc = ssp0_dai_config_hw_params(connect);
    } else if (strcmp(cmd, "playtone") == 0) {
        /* playtone [freq_hz] [hold_sec] [spk|hp]
         * Keep playback DMA mapped and rewrite sine every 200ms — CoreAudio/HAL
         * otherwise overwrites the shared buffer with silence after we unmap.
         * Optional 3rd route arg forces speakers/headphones (ignore jack detect). */
        double freq = (argc >= 3) ? atof(argv[2]) : 750.0;
        int hold_sec = (argc >= 4) ? atoi(argv[3]) : 5;
        int force_route = 0; /* 0=auto 1=spk 2=hp */
        if (argc >= 5) {
            if (strcmp(argv[4], "spk") == 0 || strcmp(argv[4], "speakers") == 0)
                force_route = 1;
            else if (strcmp(argv[4], "hp") == 0 || strcmp(argv[4], "headphones") == 0)
                force_route = 2;
        }
        if (hold_sec < 1) hold_sec = 1;
        if (hold_sec > 60) hold_sec = 60;
        mach_vm_address_t pbAddr = 0;
        mach_vm_size_t pbSize = 0;
        kr = IOConnectMapMemory64(connect, kMemPlayback, mach_task_self(), &pbAddr, &pbSize, kIOMapAnywhere);
        if (kr != KERN_SUCCESS || pbAddr == 0 || pbSize < 65536) {
            fprintf(stderr, "[-] map playback failed: kr=0x%x size=%llu\n", kr, (unsigned long long)pbSize);
            rc = 1;
        } else {
            int32_t *pcm = (int32_t *)(uintptr_t)pbAddr;
            uint32_t nframes = (uint32_t)(pbSize / 8);   /* S32 stereo full-scale (Linux FE) */
            if (nframes > 16384) nframes = 16384;
            for (uint32_t f = 0; f < nframes; f++) {
                int32_t s = (int32_t)(1500000000.0 * sin(2.0 * M_PI * freq * (double)f / 48000.0));
                pcm[f * 2 + 0] = s;
                pcm[f * 2 + 1] = s;
            }
            /* Do NOT call DAI_CONFIG here — StartPlayback already does
             * HW_PARAMS + TRIG host. A pre-call just races CoreAudio and, if
             * StartPlayback then times out mid-TRIG, the mailbox stays wedged. */
            kr = IOConnectCallScalarMethod(connect, kSel_StartPlayback, NULL, 0, NULL, NULL);
            if (kr != KERN_SUCCESS) {
                printf("[-] StartPlayback 0x%x — resetdsp+init then retry once\n", kr);
                IOConnectCallScalarMethod(connect, kSel_ForceResetDSP, NULL, 0, NULL, NULL);
                usleep(500000);
                IOConnectCallScalarMethod(connect, kSel_TriggerInitDSP, NULL, 0, NULL, NULL);
                usleep(800000);
                kr = IOConnectCallScalarMethod(connect, kSel_StartPlayback, NULL, 0, NULL, NULL);
            }
            if (kr != KERN_SUCCESS) {
                printf("[-] StartPlayback failed: 0x%x\n", kr);
                rc = 1;
            } else {
                /* Re-assert clocks after TRIG_START (FW may gate until running). */
                (void)ssp0_dai_config_hw_params(connect);
                usleep(70000);
                uint64_t sta = 0; uint32_t oc = 1; uint64_t in = ES8326_HPDET_STA;
                IOConnectCallScalarMethod(connect, kSel_I2CReadReg, &in, 1, &sta, &oc);
                bool hp_in = (sta != 0xff) && ((sta & ES8326_HPINSERT_FLAG) != 0);
                if (force_route == 1) hp_in = false;
                if (force_route == 2) hp_in = true;
                apply_codec_play(connect);
                if (hp_in) {
                    gpio_set_amp(connect, 0);            /* headphones: amp OFF */
                } else {
                    usleep(70000);                       /* anti-pop before amp ON */
                    gpio_set_amp(connect, 1);            /* speakers: amp ON */
                }
                uint32_t pad = 0;
                gpio_rd(connect, amp_padcfg(connect), &pad);
                printf("[+] playtone %.1f Hz (kr=0x%x) — listen %ds, sine re-injected (anti-HAL-silence)\n",
                       freq, kr, hold_sec);
                printf("    Route: %s | C1/pad8 TX=%u (raw: 1=amp ON) HPDET_STA=0x%02llx jack=%s%s\n",
                       hp_in ? "HEADPHONES" : "INTERNAL SPEAKERS",
                       pad & 1u, (unsigned long long)(sta & 0xff),
                       hp_in ? "IN" : "OUT",
                       force_route ? " (forced)" : "");
                printf("    Note: HAL/afplay also feeds DMA — prefer: afplay /tmp/medion_tone750.wav\n");
                int steps = hold_sec * 5;
                for (int t = 0; t < steps; t++) {
                    for (uint32_t f = 0; f < nframes; f++) {
                        int32_t s = (int32_t)(1500000000.0 * sin(2.0 * M_PI * freq * (double)f / 48000.0));
                        pcm[f * 2 + 0] = s;
                        pcm[f * 2 + 1] = s;
                    }
                    /* HAL/CoreAudio can nudge the codec — re-assert playback. */
                    if ((t % 5) == 0) {
                        apply_codec_play(connect);
                        uint64_t pos = 0; uint32_t outCnt = 1;
                        IOConnectCallScalarMethod(connect, kSel_GetPosition, NULL, 0, &pos, &outCnt);
                        int peak = 0;
                        for (uint32_t i = 0; i < nframes * 2; i++) {
                            int v = pcm[i]; if (v < 0) v = -v; if (v > peak) peak = v;
                        }
                        printf("    t=%ds pos=%llu peak=%d dac_mute=0x%02x\n", t / 5,
                               (unsigned long long)pos, peak,
                               i2c_read_reg(connect, ES8326_DAC_MUTE));
                    }
                    usleep(200000);
                }
                /* Leave the speakers armed when the jack is out. */
                {
                    uint64_t sta2 = 0; uint32_t oc2 = 1; uint64_t in2 = ES8326_HPDET_STA;
                    IOConnectCallScalarMethod(connect, kSel_I2CReadReg, &in2, 1, &sta2, &oc2);
                    bool jack_in = (sta2 != 0xff) && ((sta2 & ES8326_HPINSERT_FLAG) != 0);
                    if (!jack_in) {
                        apply_codec_play(connect);
                        usleep(70000);
                        gpio_set_amp(connect, 1);
                        printf("[*] playtone exit: restored SPEAKERS (amp ON, DAC unmuted)\n");
                    }
                }
                IOConnectUnmapMemory64(connect, kMemPlayback, mach_task_self(), pbAddr);
                rc = (kr == KERN_SUCCESS) ? 0 : 1;
            }
        }
    } else if (strcmp(cmd, "peek-capture") == 0) {
        int seconds = argc >= 3 ? atoi(argv[2]) : 10;
        if (seconds < 1) seconds = 1;
        if (seconds > 30) seconds = 30;
        if (map_capture_if_needed(connect, &capMapAddr, &capMapSize) != 0) {
            rc = 1;
        } else {
            volatile int32_t *s = (volatile int32_t *)(uintptr_t)capMapAddr;
            size_t frames = (size_t)capMapSize / (2 * sizeof(*s));
            if (frames > 16384) frames = 16384;
            for (int t = 0; t < seconds; ++t) {
                double sum[2] = {0, 0}, squares[2] = {0, 0};
                int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
                for (size_t f = 0; f < frames; ++f) {
                    for (int ch = 0; ch < 2; ++ch) {
                        int32_t raw = s[2 * f + ch];
                        if (raw < lo[ch]) lo[ch] = raw;
                        if (raw > hi[ch]) hi[ch] = raw;
                        double v = (double)raw / 2147483648.0;
                        sum[ch] += v;
                        squares[ch] += v * v;
                    }
                }
                for (int ch = 0; ch < 2; ++ch) {
                    double mean = sum[ch] / frames;
                    double variance = squares[ch] / frames - mean * mean;
                    if (variance < 0) variance = 0;
                    printf("%s mean=%+.6f ac_rms=%.8f range=%d..%d  ", ch ? "R" : "L", mean, sqrt(variance), lo[ch], hi[ch]);
                }
                printf("t=%d\n", t + 1);
                fflush(stdout);
                sleep(1);
            }
        }
    } else if (strcmp(cmd, "peek-play") == 0) {
        mach_vm_address_t addr = 0;
        mach_vm_size_t size = 0;
        kr = IOConnectMapMemory64(connect, kMemPlayback, mach_task_self(), &addr, &size, kIOMapAnywhere);
        if (kr != KERN_SUCCESS || !addr) {
            printf("[-] playback map failed: 0x%x\n", kr);
            rc = 1;
        } else {
            volatile int32_t *samples = (volatile int32_t *)(uintptr_t)addr;
            uint64_t peak = 0, nonzero = 0;
            for (mach_vm_size_t i = 0; i < size / sizeof(*samples); ++i) {
                int64_t sample = samples[i];
                uint64_t magnitude = sample < 0 ? (uint64_t)-sample : (uint64_t)sample;
                if (magnitude > peak) peak = magnitude;
                if (sample) ++nonzero;
            }
            printf("playback bytes=%llu nonzero=%llu peak=%llu\n",
                   (unsigned long long)size, (unsigned long long)nonzero, (unsigned long long)peak);
            IOConnectUnmapMemory64(connect, kMemPlayback, mach_task_self(), addr);
        }
    } else if (strcmp(cmd, "dump-play") == 0) {
        dump_play_snapshot(connect);
        rc = 0;
    } else if (strcmp(cmd, "probe-dma") == 0) {
        double w = (argc >= 3) ? atof(argv[2]) : 1.0;
        if (w < 0.2) w = 0.2;
        probe_dma(connect, w);
        rc = 0;
    } else if (strcmp(cmd, "stop") == 0) {
        kr = IOConnectCallScalarMethod(connect, kSel_StopPlayback, NULL, 0, NULL, NULL);
        printf("stop -> 0x%x\n", kr); rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "pos") == 0) {
        uint64_t out = 0; uint32_t outCnt = 1;
        kr = IOConnectCallScalarMethod(connect, kSel_GetPosition, NULL, 0, &out, &outCnt);
        printf("pos = %llu (kr=0x%x)\n", (unsigned long long)out, kr);
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "capstart") == 0) {
        if (map_capture_if_needed(connect, &capMapAddr, &capMapSize) != 0) {
            rc = 1;
        } else {
            kr = IOConnectCallScalarMethod(connect, kSel_StartCapture, NULL, 0, NULL, NULL);
            printf("capstart -> 0x%x\n", kr); rc = (kr == KERN_SUCCESS) ? 0 : 1;
        }
    } else if (strcmp(cmd, "capstop") == 0) {
        kr = IOConnectCallScalarMethod(connect, kSel_StopCapture, NULL, 0, NULL, NULL);
        printf("capstop -> 0x%x\n", kr); rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "cappos") == 0) {
        if (map_capture_if_needed(connect, &capMapAddr, &capMapSize) != 0) {
            rc = 1;
        } else {
            uint64_t out = 0; uint32_t outCnt = 1;
            kr = IOConnectCallScalarMethod(connect, kSel_GetCapPosition, NULL, 0, &out, &outCnt);
            printf("cappos = %llu (kr=0x%x)\n", (unsigned long long)out, kr);
            rc = (kr == KERN_SUCCESS) ? 0 : 1;
        }
    } else if (strcmp(cmd, "rdbar0") == 0 || strcmp(cmd, "rdbar4") == 0) {
        if (argc < 4) { usage(argv[0]); rc = 1; goto done; }
        int bar = (cmd[5] == '0') ? 0 : 4;
        int width = atoi(argv[2]);
        uint64_t off = 0;
        if (parse_u64(argv[3], &off) != 0) { fprintf(stderr, "bad offset\n"); rc = 1; goto done; }
        int sel = width_to_sel_rd(bar, width);
        if (sel < 0) { fprintf(stderr, "bad width\n"); rc = 1; goto done; }
        uint64_t in = off;
        uint64_t out[2] = {0};
        uint32_t outCnt = (width == 64) ? 2 : 1;
        kr = IOConnectCallScalarMethod(connect, sel, &in, 1, out, &outCnt);
        if (kr != KERN_SUCCESS) {
            printf("[-] read failed: 0x%x\n", kr); rc = 1;
        } else if (width == 64) {
            printf("BAR%d[0x%llx] = 0x%08llx%08llx\n", bar, (unsigned long long)off,
                   (unsigned long long)out[1], (unsigned long long)out[0]);
        } else {
            printf("BAR%d[0x%llx] = 0x%llx\n", bar, (unsigned long long)off,
                   (unsigned long long)out[0]);
        }
    } else if (strcmp(cmd, "wrbar0") == 0 || strcmp(cmd, "wrbar4") == 0) {
        if (argc < 5) { usage(argv[0]); rc = 1; goto done; }
        int bar = (cmd[5] == '0') ? 0 : 4;
        int width = atoi(argv[2]);
        uint64_t off = 0, val = 0, hi = 0;
        if (parse_u64(argv[3], &off) != 0 || parse_u64(argv[4], &val) != 0) {
            fprintf(stderr, "bad args\n"); rc = 1; goto done;
        }
        int sel = width_to_sel_wr(bar, width);
        if (sel < 0) { fprintf(stderr, "bad width\n"); rc = 1; goto done; }
        uint64_t in[3];
        uint32_t inCnt;
        if (width == 64) {
            if (argc < 6 || parse_u64(argv[5], &hi) != 0) {
                /* allow single 64-bit value: split lo/hi */
                in[0] = off;
                in[1] = (uint32_t)(val & 0xFFFFFFFFULL);
                in[2] = (uint32_t)(val >> 32);
            } else {
                in[0] = off; in[1] = val; in[2] = hi;
            }
            inCnt = 3;
        } else {
            in[0] = off; in[1] = val; inCnt = 2;
        }
        kr = IOConnectCallScalarMethod(connect, sel, in, inCnt, NULL, NULL);
        printf("[%c] write BAR%d[0x%llx] width=%d -> 0x%x\n",
               kr == KERN_SUCCESS ? '+' : '-', bar, (unsigned long long)off, width, kr);
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "rdgpio") == 0) {
        if (argc < 3) { usage(argv[0]); rc = 1; goto done; }
        uint64_t off = 0;
        if (parse_u64(argv[2], &off) != 0) { fprintf(stderr, "bad offset\n"); rc = 1; goto done; }
        uint64_t out = 0; uint32_t outCnt = 1;
        kr = IOConnectCallScalarMethod(connect, kSel_ReadGPIO32, &off, 1, &out, &outCnt);
        if (kr == KERN_SUCCESS)
            printf("GPIO[0x%llx] = 0x%08llx\n", (unsigned long long)off, (unsigned long long)out);
        else { printf("[-] rdgpio failed: 0x%x\n", kr); rc = 1; }
    } else if (strcmp(cmd, "wrgpio") == 0) {
        if (argc < 4) { usage(argv[0]); rc = 1; goto done; }
        uint64_t in[2];
        if (parse_u64(argv[2], &in[0]) != 0 || parse_u64(argv[3], &in[1]) != 0) {
            fprintf(stderr, "bad args\n"); rc = 1; goto done;
        }
        kr = IOConnectCallScalarMethod(connect, kSel_WriteGPIO32, in, 2, NULL, NULL);
        printf("[%c] wrgpio -> 0x%x\n", kr == KERN_SUCCESS ? '+' : '-', kr);
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else if (strcmp(cmd, "rdi2c") == 0) {
        if (argc < 3) { usage(argv[0]); rc = 1; goto done; }
        uint64_t reg = 0;
        if (parse_u64(argv[2], &reg) != 0) { fprintf(stderr, "bad reg\n"); rc = 1; goto done; }
        uint64_t out = 0; uint32_t outCnt = 1;
        kr = IOConnectCallScalarMethod(connect, kSel_I2CReadReg, &reg, 1, &out, &outCnt);
        if (kr == KERN_SUCCESS)
            printf("I2C[0x%02llx] = 0x%02llx\n", (unsigned long long)reg, (unsigned long long)out);
        else { printf("[-] rdi2c failed: 0x%x\n", kr); rc = 1; }
    } else if (strcmp(cmd, "wri2c") == 0) {
        if (argc < 4) { usage(argv[0]); rc = 1; goto done; }
        uint64_t in[2];
        if (parse_u64(argv[2], &in[0]) != 0 || parse_u64(argv[3], &in[1]) != 0) {
            fprintf(stderr, "bad args\n"); rc = 1; goto done;
        }
        kr = IOConnectCallScalarMethod(connect, kSel_I2CWriteReg, in, 2, NULL, NULL);
        printf("[%c] wri2c -> 0x%x\n", kr == KERN_SUCCESS ? '+' : '-', kr);
        rc = (kr == KERN_SUCCESS) ? 0 : 1;
    } else {
        usage(argv[0]);
        rc = 1;
    }

done:
    if (capMapAddr != 0) {
        IOConnectUnmapMemory64(connect, kMemCapture, mach_task_self(), capMapAddr);
        capMapAddr = 0;
    }
    IOServiceClose(connect);
    IOObjectRelease(service);
    return rc;
}
