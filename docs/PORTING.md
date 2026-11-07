# Porting to another SOF + ES8326 / ES8336 laptop

MedionAudio is built for the **MEDION Akoya E15415** (Comet Lake, board `NP1xCM`,
BIOS `CML_NP15CM_V1.0.7`). The SOF IPC3 path is generic; the values below are board-specific.
The easiest way to get them is to boot a Linux live USB on the target machine, where
`sof_es8336` already works, and copy what Linux does.

## 1. Collect the facts under Linux

```bash
lspci -nn | grep -i audio                       # DSP PCI ID (here 8086:02c8, Comet Lake-LP)
dmesg | grep -iE 'sof|es83|quirk'               # firmware, topology, sof_es8336 quirk mask
cat /sys/class/dmi/id/board_name                # board name (quirk table key)
cat /sys/kernel/debug/gpio                      # "speakers-enable" / "headphone-enable" lines
sudo i2cdump -y <bus> 0x19                      # ES8326 registers while audio is playing
```

The `sof_es8336` quirk mask tells you the SSP port, whether there is a DMIC, which GPIO
drives the amplifier and which mic input the headset uses (`SOC_ES8336_HEADSET_MIC1`).
This board uses quirk **`0x0`** = SSP0, no DMIC, no headphone GPIO, headset mic on **MIC2**.

The DSDT also matters: `ESSX._CRS` lists the amplifier GPIO as
`GpioIo("speakers-enable")` with `GNUM(0x04030008)` = group 3, pin 8. `GPCL[3]` gives the
community (`PCR 0x006D0000`, PADBAR `0x0600`), so the pad is **community 1, pad 8**.

## 2. Values to change

| What | Where | MEDION E15415 value |
|---|---|---|
| DSP PCI ID | `kext/MedionAudio/Info.plist` → `IOPCIMatch` | `0x02c88086` |
| SOF firmware | `kext/MedionAudio/Firmware/sof-*.ri` (embedded by `sof_firmware_embed.s`) | `sof-cml.ri` (IPC3, ABI 3:22:1) |
| Topology reference | pipelines are built by hand in `MedionAudioDevice.cpp` (PIPE1 / PIPE2), mirroring the `.tplg` Linux loads | `sof-cml-es8336-ssp0.tplg` (quirk `0x0`) |
| SSP port and clocks | `SSP_GOLD_*` constants and `sspDaiConfig()` in `MedionAudioDevice.cpp` | SSP0; Linux topology: MCLK 24 MHz, BCLK 2.4 MHz, 25-bit slots |
| Codec clock coefficients | `ES8326Codec::configure()` (`CLK_DIV1/2`, `CLK_DLL`, `CLK_MUX`, `CLK_*_SEL/OSR`) | Linux uses 19.2 MHz sysclk; table v0 for version 0, v3 otherwise |
| Amplifier GPIO | `MEDION_SPK_*` defines in `ES8326Codec.hpp` **and** `MB_SetAmp()` / PADBAR fallback in `plugin/MedionAudioPlugin.c` | community 1, pad 8, PADCFG0 `0x680`, raw 1 during playback |
| Headset mic input | `ES8326Codec::setCaptureMic()` | Linux machine mapping labels headset MIC2; observed working codec `PGAGAIN` selector remains `MIC1_SEL` |
| Codec I2C device | ACPI `ESSX8326` matched through VoodooI2C (`ES8326Codec.cpp`) | address `0x19` on I2C4 (`8086:02c5`) |
| Jack detection | `ES8326_HPDET_STA` (`0xFB`) in the kext and plugin | bit 1 = inserted, bit 0 = 0 → 4-pole headset |

### Speaker amplifier polarity

Linux marks the descriptor active-low, but the machine driver's DAPM path inverts the logical state. Observed on the Live USB: raw TXSTATE is **1 during playback** and **0 at idle**. Do not infer the amplifier state from the descriptor flag alone.

### I2C4 on VoodooI2C

The codec sits on the Comet Lake **I2C4** controller (`8086:02c5`). Stock VoodooI2C only
binds I2C0–I2C3 on this generation, so `EFI/OC/Kexts/VoodooI2C.kext/Contents/Info.plist`
adds `0x02c58086&0xFFFFFFFF` to `VoodooI2CPCILakeController > IOPCIMatch`. After a reboot
verify that `ESSX@0` gets a `VoodooI2CDeviceNub`:

```bash
ioreg -l -w0 | grep -i -E 'ESSX|VoodooI2CDeviceNub'
```

If the nub is missing, nothing else will work: the kext logs
`VoodooI2CDeviceNub not yet matched, will retry`.

## 3. Proposed bring-up order (not yet verified)

1. Kext loads, DSP boots the firmware (`medion_ctl status` shows the ROM status / FW ready).
2. Playback pipeline + SSP0 DAI_CONFIG accepted (no IPC errors in `log stream`).
3. Codec registers match the ES8326 playback targets (`medion_ctl calibrate spk`).
4. Speaker amplifier: hear the speakers (`medion_ctl unmute-speakers`).
5. Headphones + headset mic on MIC2, internal mic on MIC1.
6. Sleep / wake.

`docs/AUDIO.md` lists the architecture and the bugs fixed along the way; most of them
(S24 vs S32 formats, LEN_24, DAC mute, triggering the host component only) apply to any
SOF + ES83xx port.

## 4. Output tuning

Both output profiles in `plugin/MedionDSP.h` are deliberately flat, matching the Linux SOF
pipeline: volume plus a look-ahead peak limiter (speakers −1.0 dBFS, headphones −0.5 dBFS).
There is no EQ, virtual bass or bass limiter on this board. If you want to tune another
laptop, measure with `tools/measure/fr_measure.c` and design filters with
`tools/measure/eqdesign.py`.
