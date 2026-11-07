# MedionAudio

macOS audio driver for laptops using **Intel Sound Open Firmware (SOF) + Everest ES8326** —
built for the **MEDION Akoya E15415** (Comet Lake, board `NP1xCM`).

This is an experimental fork of [OctoDevStudios/MateBookAudio](https://github.com/OctoDevStudios/MateBookAudio), originally developed for the Huawei MateBook D15. Thanks to the original author and contributors. The ES8326 port prioritizes the MEDION; **the original D15 is no longer supported by this branch**.

The MEDION port was developed with AI assistance from **GPT-6.1 Terra**; hardware testing and feedback were provided by the maintainer.

These machines have no HDA codec: audio goes through the Intel cAVS DSP and an I2S codec on
I2C. AppleALC cannot drive that, so Hackintosh users usually fall back to USB or Bluetooth audio.
MedionAudio drives the whole path itself:

| | |
|---|---|
| Speakers | Working; flat profile and limiter |
| Headphones (hot-plug) | Working and detected |
| Internal microphone | Not working; physical fault is possible and reportedly likely, but unconfirmed |
| Headset microphone | Working with adjustable gain and noise expander |
| Volume and mute | Working; sleep / wake cannot currently be evaluated reliably on this Hackintosh |

**Experimental port:** speakers, headphones and headset mic have been tested on macOS 15.8.2. The internal mic remains unresolved; sleep/wake cannot currently be evaluated reliably on this Hackintosh.

### What changed from MateBookAudio

- Replaced ES8336/ES8316 codec programming with an ES8326 implementation for ACPI `ESSX8326` at I2C address `0x19`. Codec revision 0 and later revisions use their respective Linux clock coefficient tables.
- Added Comet Lake I2C4 (`8086:02c5`) matching to the **local** VoodooI2C configuration; this repository does not include an EFI or a patched VoodooI2C binary.
- Adapted the SOF IPC3 pipeline for SSP0 playback and capture, without the original D15's Intel DMIC pipeline. The Linux reference topology uses a 24 MHz SSP MCLK and 2.4 MHz BCLK, while its codec driver configures a 19.2 MHz codec sysclk.
- Changed the speaker amplifier control to the MEDION's community 1 pad 8 and matched its observed playback/idle GPIO levels. Jack and headset detection use ES8326 `HPDET_STA`.
- Corrected the headset capture path and added input gain and a gentle noise expander. The internal microphone is silent in macOS, Linux and Windows; a physical issue is suspected but not confirmed.
- Replaced the D15 speaker-specific EQ and virtual bass with a flat profile, volume control and peak limiter. Added playback stop muting to reduce pops.
- Renamed the kext, HAL plugin, tools and service to MedionAudio. AppleALC and `alcid=15` were removed from the tested EFI; no EFI is published here.

The normal macOS firmware startup chime is not supported: this driver initializes the DSP only after macOS loads. No replacement startup sound is installed.

## How it works

```
CoreAudio ── MedionAudio.driver (HAL plugin, DSP, jack/mic logic)
                 │  shared DMA buffers + UserClient
             MedionAudio.kext ── Intel cAVS DSP running SOF (IPC3) ── SSP0 (I2S) ── ES8326
                 │                                                                      │
                 └──────────── I2C4 through VoodooI2C (ESSX8326, 0x19) ─────────────────┘
```

- **Kext** — loads the SOF firmware into the DSP, builds the playback (SSP0) and capture
  (SSP0 RX) pipelines over IPC3, programs the ES8326 over I2C and switches the
  speaker amplifier on GPIO community 1, pad 8.
- **HAL plugin** — a CoreAudio AudioServerPlugIn that maps the DMA ring buffers, runs the output
  DSP and follows the headphone jack and headset mic. Sleep recovery is implemented but unverified.
- **Boot script** — a LaunchDaemon that initialises the DSP at boot; the plugin handles jack routing.

The full write-up (register values, bugs, architecture) is in [docs/AUDIO.md](docs/AUDIO.md).

## Requirements

- macOS 15.8.2; other versions remain unverified.
- [VoodooI2C](https://github.com/VoodooI2C/VoodooI2C) loaded, with the Comet Lake **I2C4**
  PCI ID (`8086:02c5`) added to `VoodooI2CPCILakeController` (the codec sits on the same
  controller family as the trackpad).
- OpenCore load order: `VoodooI2CServices.kext`, `VoodooGPIO.kext`, `VoodooI2C.kext`,
  then `MedionAudio.kext`. The tested EFI uses standalone copies of the first two dependencies.
- Remove AppleALC and `alcid` only if they are not required for another audio device in your configuration. Keep a known-good EFI and a bootable recovery method before making changes.
- Xcode Command Line Tools to build.

## Install

1. Build the kext on macOS with Xcode Command Line Tools:
   ```bash
   make -C kext            # → kext/MedionAudio.kext
   ```
   Add it only to `EFI/OC/Kexts/` (never `/Library/Extensions`) and to `config.plist` → `Kernel > Add` **after VoodooI2C**. Configure the I2C4 match and dependency order above, then reboot. A matching EFI file alone does not prove the kext was loaded.
2. Install the plugin, `medion_ctl` and the boot LaunchDaemon:
   ```bash
   sudo install/install.sh            # uninstall: sudo install/install.sh uninstall
   ```
   The installer replaces the existing `MedionAudio.driver` and its helper files; back up any custom version first. It installs the HAL plugin, tools and LaunchDaemon, **not** the kext.

3. Confirm `kmutil showloaded | grep -Ei 'VoodooI2C|MedionAudio'` lists the kexts and `/usr/local/libexec/medion/medion_ctl status` reports an active DSP. The LaunchDaemon initializes the DSP on boot and restarts `coreaudiod` once initialization succeeds.

The original project targeted a Huawei MateBook D15; its EFI is not a ready-made Medion EFI.

For recovery guidance, see [docs/RECUPERACION.md](docs/RECUPERACION.md). Backup archives and EFI files are not included in this repository; preserve Windows boot files separately.

## Earlier project changelog

**v1.1.1**
- Data sources use Apple's codes (`ispk`/`hdpn` for speakers/headphones, `imic`/`emic` for the
  mics), like a real Mac. macOS now finds the internal speakers, so the charging chime
  (PowerChime) plays.

**v1.1.0**
- The HAL clock now follows the real DMA position of the card instead of a free-running host
  clock. Speaker → mic latency is now fixed (~37 ms) instead of drifting between ~10 and
  ~350 ms, which broke echo cancellation: calls using Apple voice processing sounded wrong.
- The mic is read at a fixed offset from playback (re-measured if capture restarts).
- Denormals flushed in the IO cycle (high CPU use while the DSP filters decay to silence).
- Fixed a boot-time deadlock with VoodooI2C that could freeze the trackpad.
- Tested on macOS Tahoe 26.7.1.

**v1.0.0** — first release (Sonoma).

## Other laptops

Other machines with SOF + ES8326/ES8336 need a few board-specific values changed — PCI ID,
amplifier GPIO community/pad/polarity, SSP port and MCLK, headset-mic input, firmware.
See [docs/PORTING.md](docs/PORTING.md). Reports and pull requests for other models are welcome.

## Debugging

```bash
make -C tools && sudo tools/medion_ctl status
log stream --predicate 'eventMessage CONTAINS "MedionAudio"'
```

More commands in [docs/AUDIO.md](docs/AUDIO.md#4-debugging).

## License

- Code: [MIT](LICENSE).
- `kext/MedionAudio/Firmware/sof-cml.ri` is a Sound Open Firmware binary from [sof-bin](https://github.com/thesofproject/sof-bin), embedded in the kext under [LICENSES/LICENCE.SOF-Intel](LICENSES/LICENCE.SOF-Intel).
- The register sequences follow the Linux `sof_es8336` / `es8326` drivers.
