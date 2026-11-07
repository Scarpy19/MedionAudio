#!/usr/bin/env bash
# collect-macos.sh - Recolecta informacion de macOS para portar MedionAudio.
#
# Uso:
#   ./collect-macos.sh [--out DIR] [--efi PATH]
#
# No modifica nada. Algunos comandos piden sudo (log show, dmesg, launchctl).
# Salida: DIR/ y DIR.tar.gz

set -u
set -o pipefail

VERSION="1.0"
OUTDIR=""
EFI_PATH=""
SCRIPT_NAME="$(basename "$0")"

usage() {
  cat <<EOF
Uso: $SCRIPT_NAME [opciones]

  --out DIR     Directorio de salida (por defecto: ./macos-porting-report-FECHA)
  --efi PATH    Ruta a la carpeta EFI/OC (p.ej. /Volumes/EFI/EFI/OC)
  -h, --help    Mostrar esta ayuda
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUTDIR="${2:-}"; shift 2 ;;
    --out=*) OUTDIR="${1#*=}"; shift ;;
    --efi) EFI_PATH="${2:-}"; shift 2 ;;
    --efi=*) EFI_PATH="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opcion desconocida: $1"; usage; exit 2 ;;
  esac
done

if [ -z "$OUTDIR" ]; then
  OUTDIR="$PWD/macos-porting-report-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUTDIR"/{system,ioreg,kexts,audio,logs,efi,acpi}
REPORT="$OUTDIR/report.txt"
exec > >(tee -a "$REPORT") 2>&1

section() {
  echo
  echo "======================================================================"
  echo "== $*"
  echo "======================================================================"
}

run_save() {
  local file="$1"; shift
  echo
  echo "### $file"
  echo "\$ $*"
  { echo "\$ $*"; "$@" 2>&1; } | tee "$OUTDIR/$file"
}

run_sh() {
  local file="$1"; shift
  local cmd="$*"
  echo
  echo "### $file"
  echo "\$ $cmd"
  { echo "\$ $cmd"; bash -c "$cmd" 2>&1; } | tee "$OUTDIR/$file"
}

section "MedionAudio macOS collector v$VERSION"
echo "Fecha:  $(date)"
echo "Salida: $OUTDIR"

echo
echo "Algunos comandos necesitan sudo. Si falla, se omiten."
sudo -v 2>/dev/null || echo "AVISO: sin sudo; se omitiran logs del sistema."

# ---------------------------------------------------------------------------
section "Sistema"
run_save "system/sw_vers.txt" sw_vers
run_save "system/uname.txt" uname -a
run_save "system/hw-model.txt" bash -c 'sysctl -n hw.model; sysctl -n machdep.cpu.brand_string; sysctl -n hw.ncpu; sysctl -n hw.memsize'
run_save "system/system_profiler_hardware.txt" system_profiler SPHardwareDataType
run_save "system/system_profiler_software.txt" system_profiler SPSoftwareDataType
run_save "system/system_profiler_audio.txt" system_profiler SPAudioDataType
run_save "system/system_profiler_hardware.json" system_profiler SPHardwareDataType -json
run_save "system/system_profiler_audio.json" system_profiler SPAudioDataType -json
run_save "system/pmset.txt" pmset -g
run_save "system/pmset-assertions.txt" pmset -g assertions
run_save "system/diskutil.txt" diskutil list
run_save "system/mount.txt" mount
run_save "system/df.txt" df -h
run_save "system/xcode.txt" bash -c 'xcode-select -p 2>&1; clang --version 2>&1 | head -n 3; make --version 2>&1 | head -n 2'

# ---------------------------------------------------------------------------
section "SIP / NVRAM / Secure Boot"
run_save "system/csrutil.txt" csrutil status
run_save "system/csrutil-authenticated-root.txt" csrutil authenticated-root status
run_save "system/spctl.txt" spctl --status
run_save "system/nvram.txt" nvram -p
run_save "system/boot-args.txt" nvram boot-args
run_save "system/csr-active-config.txt" nvram csr-active-config

# ---------------------------------------------------------------------------
section "ioreg"
run_save "ioreg/ioreg-full.txt" ioreg -lw0
run_save "ioreg/ioreg-devicetree.txt" ioreg -p IODeviceTree -lw0
run_save "ioreg/ioreg-ioservice.txt" ioreg -p IOService -lw0
{
  ioreg -lw0 | grep -iE 'ESSX|VoodooI2C|VoodooGPIO|VoodooI2CDeviceNub|AppleALC|AppleHDA|audio|I2C|GPIO' || true
} | tee "$OUTDIR/ioreg/ioreg-audio.txt"
{
  ioreg -lw0 | grep -i -A20 -B5 -E 'ESSX8336|ESSX8316|ES8336|ES8316' || true
} | tee "$OUTDIR/ioreg/ioreg-essx.txt"

# ---------------------------------------------------------------------------
section "Kexts / plugins"
run_save "kexts/kextstat.txt" kextstat
run_save "kexts/kmutil-showloaded.txt" kmutil showloaded
run_sh "kexts/kexts-audio.txt" "kextstat | grep -iE 'voodoo|applealc|medion|lilu|virtualsmc|whatevergreen|amfipass' || true"
run_save "kexts/Library-Extensions.txt" ls -la /Library/Extensions
run_save "kexts/HAL-plugins.txt" ls -la /Library/Audio/Plug-Ins/HAL
run_sh "kexts/System-Extensions-audio.txt" "ls -la /System/Library/Extensions | grep -iE 'audio|hda|voodoo|applealc' || true"
run_save "kexts/launchdaemons.txt" bash -c 'ls -la /Library/LaunchDaemons 2>/dev/null | grep -iE "medion|audio" || true'
run_save "kexts/launchctl.txt" bash -c 'launchctl list 2>/dev/null | grep -iE "medion|coreaudio|audio" || true'
run_save "kexts/packages.txt" bash -c 'pkgutil --pkgs 2>/dev/null | grep -iE "audio|voodoo|medion|hda" || true'

# ---------------------------------------------------------------------------
section "Audio / logs"
run_save "audio/coreaudiod-log.txt" sudo log show --last 30m --style compact --predicate 'process == "coreaudiod"'
{
  sudo log show --last 30m --style compact --predicate 'eventMessage CONTAINS[c] "audio" OR eventMessage CONTAINS[c] "VoodooI2C" OR eventMessage CONTAINS[c] "ESSX" OR eventMessage CONTAINS[c] "AppleALC"' || true
} | tee "$OUTDIR/audio/audio-log.txt"
run_save "logs/dmesg.txt" sudo dmesg
run_save "logs/system-log-errors.txt" sudo log show --last 30m --style compact --predicate 'messageType == error OR messageType == fault'

# ---------------------------------------------------------------------------
section "EFI / OpenCore"
if [ -z "$EFI_PATH" ]; then
  for p in /Volumes/EFI/EFI/OC "$HOME/EFI/OC" "$PWD/EFI/OC"; do
    if [ -f "$p/config.plist" ]; then EFI_PATH="$p"; break; fi
  done
fi
if [ -n "$EFI_PATH" ] && [ -d "$EFI_PATH" ]; then
  echo "EFI: $EFI_PATH"
  cp "$EFI_PATH/config.plist" "$OUTDIR/efi/config.plist" 2>/dev/null || true
  run_save "efi/kexts-listing.txt" ls -la "$EFI_PATH/Kexts"
  run_save "efi/drivers-listing.txt" ls -la "$EFI_PATH/Drivers"
  run_save "efi/acpi-listing.txt" ls -la "$EFI_PATH/ACPI"
  run_save "efi/config-plist.txt" plutil -p "$EFI_PATH/config.plist"
else
  echo "AVISO: no se encontro EFI/OC. Usa --efi /ruta/a/EFI/OC."
fi

# ---------------------------------------------------------------------------
section "ACPI visible en macOS"
run_save "acpi/ioreg-devicetree-acpi.txt" bash -c 'ioreg -p IODeviceTree -lw0 | grep -iE "ACPI|I2C|GPIO|ESSX|audio" || true'

# ---------------------------------------------------------------------------
section "Resumen"
{
  echo "MedionAudio macOS porting report"
  echo "Fecha: $(date)"
  echo
  echo "== macOS =="
  sw_vers
  echo
  echo "== Hardware =="
  sysctl -n hw.model
  sysctl -n machdep.cpu.brand_string
  echo
  echo "== SIP =="
  csrutil status 2>&1 || true
  echo
  echo "== boot-args =="
  nvram boot-args 2>&1 || true
  echo
  echo "== Audio =="
  system_profiler SPAudioDataType 2>/dev/null || true
  echo
  echo "== Kexts relevantes =="
  kextstat 2>/dev/null | grep -iE 'voodoo|applealc|medion|lilu|virtualsmc|whatevergreen|amfipass' || true
  echo
  echo "== VoodooI2C en ioreg =="
  ioreg -lw0 2>/dev/null | grep -iE 'VoodooI2C|VoodooGPIO|ESSX' | head -n 80 || true
} | tee "$OUTDIR/SUMMARY.txt"

# ---------------------------------------------------------------------------
section "Empaquetando"
TARBALL="$OUTDIR.tar.gz"
tar -czf "$TARBALL" -C "$(dirname "$OUTDIR")" "$(basename "$OUTDIR")" 2>/dev/null || true
echo
echo "Informe:  $OUTDIR"
echo "Resumen:  $OUTDIR/SUMMARY.txt"
echo "Tarball:  $TARBALL"
