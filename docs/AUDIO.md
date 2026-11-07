# Audio — MEDION Akoya E15415 (NP1xCM)

Experimental port: speakers, headphones and headset microphone have been tested on macOS 15.8.2. The internal microphone is silent in macOS, Linux and Windows; a physical fault is possible and reportedly likely, but not confirmed. Sleep/wake cannot currently be evaluated reliably because system-wide laptop sleep is unreliable on this Hackintosh.

| | |
|---|---|
| Audio path | Intel cAVS DSP running **Sound Open Firmware (SOF, IPC3)** → **SSP0 (I2S)** → **Everest ES8326** codec |
| Codec control | I2C4 (`8086:02c5`), shared with the trackpad through VoodooI2C (`ESSX8326`, address `0x19`) |
| Microphones | headset via codec capture on SSP0 RX (PIPE2); internal microphone routing remains unverified |
| macOS parts | `MedionAudio.kext` (DSP + codec), CoreAudio HAL plugin `MedionAudio.driver`, boot script |
| Linux reference | `sof_es8336` + `es8326` drivers, topology `sof-cml-es8336-ssp0.tplg`, quirk `0x0` |

There is no HDA codec on this machine, so AppleALC cannot help: the whole SOF path has to be
driven by hand. The implementation uses Linux as a reference, but clocking, routing and register values still require hardware verification.

---

## 1. Hardware facts (what Linux does)

### 1.1 SSP0 format and clocks

| Parameter | Value |
|---|---|
| Host (front-end) format | `S32_LE` |
| DAI / SSP widget format | `S24_LE` |
| SSP MCLK | **24 MHz**, from the exact Linux Live topology |
| Codec sysclk setting | **19.2 MHz**, set separately by `sof_es8336_hw_params` |
| BCLK | 2.4 MHz = 48000 × 2 × 25 |
| Valid bits / slot width | 24 / 25 |
| Slots | 2 (L + R), I2S, codec is clock slave (`0x4001`, CBC_CFC) |

The exact Linux Live topology confirms 24 MHz SSP MCLK, 2.4 MHz BCLK and 25-bit slots. Linux separately supplies a 19.2 MHz codec sysclk; preserve this distinction.

Live Linux reports chip version `0x00`; its coefficients match `coeff_div_v0 {400, 48000, 19200000}`:

| Register | Value |
|---|---|
| `CLK_DIV1` (`0x04`) | `0xE9` |
| `CLK_DIV2` (`0x05`) | `0x04` |
| `CLK_DLL` (`0x06`) | `0x0F` |
| `CLK_MUX` (`0x07`) | `0x6D` |
| `CLK_ADC_SEL` (`0x08`) | `0x4A` |
| `CLK_DAC_SEL` (`0x09`) | `0x0A` |
| `CLK_ADC_OSR` (`0x0A`) | `0x1F` |
| `CLK_DAC_OSR` (`0x0B`) | `0x1F` |

### 1.2 ES8326 playback state

| Reg | Name | Value |
|---|---|---|
| `0x14` | `DAC_MUTE` | `0x00` unmuted / `0x03` muted |
| `0x27` | `HP_CAL` | `0x77` when playing |
| `0x24` | `HP_DRIVER` | `0xA1` |
| `0x28` | `HP_DRIVER_REF` | `0x33` (bits 4–5 = `0x30`) |
| `0x50` | `DACL_VOL` | `0xAF` (8 dB below codec maximum) |
| `0x13` | `FMT` | `0x00` (I2S, 24-bit, codec slave) |
| `0xFB` | `HPDET_STA` | bit 1 = jack inserted, bit 0 = 0 → 4-pole headset |

Playback unmute follows `es8326_mute(mute=0)`: `CLK_INV` bits `0xC0 = 0`, `CLK_MUX` bit
`0x80 = 0`, a `DAC_DSM` bit-0 pulse, `HP_DRIVER_REF` bits `0x30 → 0x20 → 0x30`,
`HP_DRIVER = 0xA1`, `HP_CAL = 0x77`, then `DAC_MUTE` bits 0–1 = 0.

### 1.3 Speaker amplifier GPIO (community 1, pad 8)

The DSDT exposes one `GpioIo("speakers-enable")` line:

- `GNUM(0x04030008)` → group 3, pin 8;
- `GPCL[3] = { PCR 0x006D0000, 24 pins, PADBAR 0x0600 }` → community 1 (`0xFD6D0000`);
- PADCFG0 = `0x600 + 8 × 16 = 0x680`.

Linux reports it as `gpio-616` / line 104 and marks the descriptor active-low. The machine driver inverts the DAPM state; live observations show:

| Amplifier | TXSTATE (bit 0) |
|---|---|
| ON (playback) | **1** |
| OFF (idle) | **0** |

There is no separate headphone amplifier GPIO: headphones are driven by the codec's own
charge pump, and the plugin only turns the speaker amp off on hotplug.

### 1.4 Microphones

Linux quirk `0x0` maps internal mic to MIC1 and headset to MIC2. On this machine the internal microphone remains silent even in Linux and Windows, so its actual routing is not verified. The working headset uses SSP0 RX; `PGAGAIN` remains at `MIC1_SEL` for both selections.

- Capture unmute: wait 300 ms, version > B: `ANA_MICBIAS` bits `0x70 = 0x70`,
  `VMIDSEL` bit `0x40 = 0`; wait 40 ms, `ADC_HPFS1/2` low nibble = `0x08`, `ADC_MUTE = 0`.
- Capture mute: `ADC_MUTE = 0x0F`, version > B: `VMIDSEL` bit `0x40 = 1`,
  `ANA_MICBIAS` bits `0x70 = 0x30`, `ADC_HPFS1/2` low nibble = `0x04`.
- Headset detect: `HPDET_STA` bit 1 = inserted; bit 0 = 0 → headset with mic.

---

## 2. macOS architecture

### 2.1 `MedionAudio.kext`

- Loads the SOF firmware (`sof-cml.ri`) into the cAVS DSP, then builds the pipelines over IPC3:
  - **PIPE1** playback: host `S32` → buffer → DAI SSP0 `S24`;
  - **PIPE2** capture: DAI SSP0 `S24` → buffer → host `S32` (shared by both mics).
- Programs SSP0 with the Linux topology values (24 MHz / 2.4 MHz / 24-in-25 bits,
  `clks_control = 0`). SSP0 clocks are shared by playback and capture: `HW_PARAMS` /
  `HW_FREE` is only sent by whichever stream starts first / stops last.
- Configures the ES8326 over I2C (`ES8326Codec`), drives the speaker amp, and
  exposes a UserClient used by the HAL plugin and by `medion_ctl`.
- Capture (`ES8326Codec::setCaptureMic`): keeps the observed working `MIC1_SEL`, configures
  MicBias, and unmutes the ADC. The headset route is working; internal routing remains unresolved.

### 2.2 HAL plugin `MedionAudio.driver`

- CoreAudio AudioServerPlugIn, 48 kHz stereo, S32 container; the DMA ring buffers are shared
  with the kext (no copy through the UserClient in the IO path).
- **Output DSP** (`MedionDSP.h`, header-only, no allocation or lock on the IO thread):
  both profiles are flat, matching the Linux SOF pipeline — volume plus a look-ahead peak
  limiter (speakers −1.0 dBFS, headphones −0.5 dBFS). No EQ, virtual bass or bass limiter.
- **Volume**: linear amplitude curve (gain = s, dB = 20·log10 s, −60…0 dB).
- **Jack handling**: a background thread polls `HPDET_STA` (`0xFB`) from plugin load,
  switches the speaker amp + DSP profile on hotplug (DAC muted 70 ms around the switch to
  avoid pops) and checks bit 0 for a headset mic.
- **Data sources** (like a real Mac): output shows *Internal Speakers* or *Headphones*, input
  shows *Internal Microphone* or *External Microphone* — only the connected source is listed.
- **Sleep / wake (not reliably testable on this Hackintosh)**: I2C paused on `WillSleep`;
  on wake the DSP is re-initialised (`TriggerInitDSP`), running streams are restarted and the
  codec is restored without muting. Lid-open only does the soft restore; the clamshell mute
  flag is debounced by 150 ms.
- Real-time hygiene: helper threads run at `QOS_CLASS_BACKGROUND`, DSP state is `mlock`ed,
  no UserClient call per IO cycle.

### 2.3 Boot script `medion_boot_init.sh`

Run once at boot by a LaunchDaemon: waits for the kext, initializes the DSP and restarts
`coreaudiod` after successful initialization. Jack routing remains the plugin's responsibility.

---

## 3. Potential issues inherited from earlier work

These observations are not evidence of fixes verified on the Medion.

| Symptom | Cause | Fix |
|---|---|---|
| Silence | DSP wedged / wrong codec init | reset the DSP, run the ES8326 init sequence |
| IPC mailbox wedge (`0xffffffff`) | `TRIG_START` sent to DAI/scheduler components | trigger host only |
| No position updates | DPLBASE buffer at physical address 0 | proper DMA position buffer |
| Silence while DMA runs | host S16 vs 24-bit SSP | host S32 |
| "Air" / hiss instead of sound | host **and** DAI both S32, codec still LEN_16 | DAI **S24** + codec **LEN_24** |
| Wrong amp behaviour | wrong GPIO community/pad | community 1 pad 8 (`0x680`), raw high in playback |
| Muted after StartIO | DAC left muted | `DAC_MUTE = 0x00` after unmute sequence |
| No sound after sleep | DSP lost its firmware (`hwReady = false`) | plugin re-inits the DSP on wake |
| Panic on wake | jack I2C polling during sleep (VoodooI2C) | pause polling around sleep |
| Dropouts on lid blips | mute + re-route on every clamshell event | soft restore + 150 ms debounce |
| Headset mic silent | wrong analog input / ADC muted | MIC2 select + MicBias + ADC unmute |
| Boot with headphones plugged: mic dead | boot script forced the speaker setup | jack-aware script + `coreaudiod` restart |

---

## 4. Debugging

```bash
cd tools && make medion_ctl
sudo ./medion_ctl status            # DSP / firmware / topology state
sudo ./medion_ctl dump-play         # codec + speaker GPIO state
sudo ./medion_ctl calibrate spk     # live registers vs. expected values (spk | hp)
sudo ./medion_ctl es8326-config     # full ES8326 init, then DAC/amp muted
sudo ./medion_ctl playtone 750 5 spk
sudo ./medion_ctl rdi2c 0xFB        # HPDET_STA: bit1 = jack in, bit0 = 0 headset
sudo ./medion_ctl probe-dma 0.2
sudo ./medion_ctl resetdsp && sudo ./medion_ctl init force
log stream --predicate 'eventMessage CONTAINS "MedionAudio"'
```

A new kext needs a reboot (it is injected by OpenCore); the HAL plugin is reloaded by
restarting `coreaudiod` (`sudo killall coreaudiod`).

---

## 5. Lessons

1. Copy what Linux does on the live system (register dumps while playing) rather than old
   comments or documentation.
2. Host and DAI formats must follow the topology (`s32le` host, `s24le` DAI), not both S32.
3. The codec's own clock coefficients must match the MCLK the DSP actually drives
   (19.2 MHz here, not the D15's 24 MHz).
4. Check the complete GPIO software path and live raw values; the descriptor polarity alone is insufficient.
5. Anything touching the shared I2C bus must stop before sleep.
