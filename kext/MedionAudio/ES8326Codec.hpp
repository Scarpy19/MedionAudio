#ifndef ES8326Codec_hpp
#define ES8326Codec_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

// Forward declaration of VoodooI2CDeviceNub
// NOTE: writeI2C, readI2C, and writeReadI2C are NON-VIRTUAL member functions in VoodooI2CDeviceNub.
// Declaring them as virtual in a forward declaration creates an invalid vtable layout that dispatches
// to random IOService methods (causing a fatal kernel panic). They must remain non-virtual.
class VoodooI2CDeviceNub : public IOService {
    OSDeclareDefaultStructors(VoodooI2CDeviceNub);
public:
    IOReturn writeI2C(UInt8 *values, UInt16 length);
    IOReturn readI2C(UInt8 *values, UInt16 length);
    IOReturn writeReadI2C(UInt8 *write_buffer, UInt16 write_length, UInt8 *read_buffer, UInt16 read_length);
};

/* Everest ES8326 register space (Linux sound/soc/codecs/es8326.h). */
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

/* ES8326_RESET */
#define ES8326_CSM_ON           (1 << 7)
#define ES8326_MUTE_MASK        (3 << 0)
#define ES8326_MUTE             (3 << 0)

/* ES8326_CLK_CTL */
#define ES8326_CLK_ON           (0x7f << 0)
#define ES8326_CLK_OFF          (0 << 0)

/* ES8326_FMT */
#define ES8326_S24_LE           (0 << 2)
#define ES8326_S20_3_LE         (1 << 2)
#define ES8326_S18_LE           (2 << 2)
#define ES8326_S16_LE           (3 << 2)
#define ES8326_S32_LE           (4 << 2)
#define ES8326_DATA_LEN_MASK    (7 << 2)
#define ES8326_DAIFMT_MASK      ((1 << 5) | (3 << 0))
#define ES8326_DAIFMT_I2S       0

/* ES8326_PGAGAIN */
#define ES8326_MIC_SEL_MASK     (3 << 4)
#define ES8326_MIC1_SEL         (1 << 4)
#define ES8326_MIC2_SEL         (1 << 5)

/* ES8326_HP_CAL */
#define ES8326_HP_OFF           0
#define ES8326_HP_FORCE_CAL     ((1 << 7) | (1 << 3))
#define ES8326_HP_ON            ((7 << 4) | (7 << 0))

/* ES8326_ADC1_SRC / ES8326_ADC2_SRC */
#define ES8326_ADC_SRC_ANA          0
#define ES8326_ADC_SRC_ANA_INV_SW0  1
#define ES8326_ADC_SRC_ANA_INV_SW1  2
#define ES8326_ADC_SRC_DMIC_SDIN2   4

/* ES8326_HPDET_TYPE */
#define ES8326_HP_DET_SRC_PIN27 (1 << 5)
#define ES8326_HP_DET_SRC_PIN9  (1 << 4)
#define ES8326_HP_DET_JACK_POL  (1 << 3)
#define ES8326_HP_DET_BUTTON_POL (1 << 2)
#define ES8326_HP_TYPE_OMTP     (3 << 0)
#define ES8326_HP_TYPE_CTIA     (2 << 0)
#define ES8326_HP_TYPE_AUTO     (1 << 0)
#define ES8326_HP_TYPE_AUTO_INV (0 << 0)

/* ES8326_INT_SOURCE */
#define ES8326_INT_SRC_PIN9     (1 << 3)

/* ES8326_SDINOUT1_IO */
#define ES8326_IO_INPUT         0

/* ES8326_HPDET_STA */
#define ES8326_HPINSERT_FLAG    (1 << 1)   /* 1 = jack inserted */
#define ES8326_HPBUTTON_FLAG    (1 << 0)   /* 0 = 4-pole headset, 1 = plain HP */

/* ES8326_CHIP_VERSION */
#define ES8326_VERSION_B        (3 << 0)

#define ES8326_I2C_ADDR         0x19

/* Medion E15415 speaker amplifier enable.
 *
 * DSDT ESSX._CRS exposes one GpioIo "speakers-enable" with
 *   GNUM(0x04030008) -> group 3, pin 8.
 * GPCL[3] = { PCR 0x006D0000, 24 pins, PADBAR 0x0600, ... base offset 0x60 }.
 * So the pad is in GPIO community 1 (0xFD6D0000), pad index 8, and PADCFG0
 * lives at 0x600 + 8*16 = 0x680.  Linux reports it as gpio-616 = line 104
 * with ACTIVE LOW. The machine driver's DAPM event inverts speaker_en:
 * Linux live reports raw TXSTATE 1 during playback and 0 at idle. */
#define MEDION_SPK_GPIO_LINE        104u
#define MEDION_SPK_COMMUNITY        1u
#define MEDION_SPK_PAD              8u
#define MEDION_SPK_PADBAR_FALLBACK  0x0600u
#define MEDION_PAD_STRIDE           16u
#define MEDION_PADBAR_REG           0x0Cu

class ES8326Codec {
public:
    ES8326Codec();
    ~ES8326Codec();

    bool init();
    void free();

    bool writeReg(UInt8 reg, UInt8 val);
    bool readRegChecked(UInt8 reg, UInt8 *val);
    UInt8 readReg(UInt8 reg);
    bool updateBits(UInt8 reg, UInt8 mask, UInt8 val);

    bool probe();
    bool configure();

    /* Playback */
    void mute(bool muted);
    void playbackUnmute();

    /* Jack */
    bool isHeadphoneInserted();
    bool isHeadsetMicPresent();

    /* Speaker amplifier GPIO (raw high during playback) */
    void setSpeakerEnable(bool enable);
    void silenceAmps();

    /* Capture: MIC1 = internal, MIC2 = headset (Linux sof_es8336 quirk 0x0). */
    void setCaptureMic(bool headset);
    void setCaptureMute(bool muted);

    UInt8 chipVersion() const { return version; }
    bool isReady() const { return (voodooNub != nullptr); }
    const char *getStatus() const { return statusMsg; }
    volatile UInt8 *getGpioBase() const { return gpioBase; }
    IOByteCount getGpioLength() const;
    volatile UInt8 *getGpioAmpBase() const { return gpioAmpBase; }
    IOByteCount getGpioAmpLength() const;

private:
    VoodooI2CDeviceNub *voodooNub;
    IOMemoryMap        *gpioMap;
    volatile UInt8     *gpioBase;     /* Community 0 @ 0xFD6E0000 */
    IOMemoryMap        *gpioAmpMap;
    volatile UInt8     *gpioAmpBase;  /* Community 1 @ 0xFD6D0000 (speaker pad) */
    char                statusMsg[128];

    UInt8 version;
    bool  calibrated;
    bool  captureMicHeadset;
    bool  ioError;

    bool initVoodooNub();
    bool initGPIO();
    void calibrate();
    void enableMicbias();
    void disableMicbias();
    void setAmpPadcfg(bool on);
    UInt32 ampPadcfgOffset();
};

#endif /* ES8326Codec_hpp */
