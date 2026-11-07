#!/usr/bin/env bash
# collect-linux.sh - Recolecta informacion de hardware/audio para portar MedionAudio
# a otro portatil con SOF + ES8336.
#
# Uso:
#   sudo ./collect-linux.sh [--out DIR] [--no-audio] [--install-deps]
#
# El script NO escribe registros del codec ni GPIOs. Solo lee, reproduce tonos
# y graba audio para capturar estados.
#
# Dependencias (Debian/Ubuntu/Mint):
#   sudo apt install alsa-utils i2c-tools evtest gpiod acpica-tools dmidecode pciutils usbutils kmod

set -u
set -o pipefail

VERSION="1.0"
OUTDIR=""
DO_AUDIO=1
INSTALL_DEPS=0
SCRIPT_NAME="$(basename "$0")"

usage() {
  cat <<EOF
Uso: sudo $SCRIPT_NAME [opciones]

  --out DIR         Directorio de salida (por defecto: ./porting-report-FECHA)
  --no-audio        No reproducir/grabar audio ni pedir estados de jack
  --install-deps    Instalar dependencias que falten con apt-get
  -h, --help        Mostrar esta ayuda

Salida: DIR/ (informe) y DIR.tar.gz
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUTDIR="${2:-}"; shift 2 ;;
    --out=*) OUTDIR="${1#*=}"; shift ;;
    --no-audio) DO_AUDIO=0; shift ;;
    --install-deps) INSTALL_DEPS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Opcion desconocida: $1"; usage; exit 2 ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: este script necesita root (dmesg, i2cdump, debugfs, dmidecode, acpidump)."
  echo "Ejecutalo asi: sudo $SCRIPT_NAME $*"
  exit 1
fi

RUN_USER="${SUDO_USER:-$(id -un)}"
RUN_GROUP="$(id -gn "$RUN_USER" 2>/dev/null || echo "$RUN_USER")"
RUN_HOME="$(getent passwd "$RUN_USER" 2>/dev/null | cut -d: -f6)"
[ -n "${RUN_HOME:-}" ] || RUN_HOME="$HOME"

if [ -z "$OUTDIR" ]; then
  OUTDIR="$PWD/porting-report-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUTDIR"/{system,pci,acpi,audio,sof,i2c,gpio,codec,capture,logs}
REPORT="$OUTDIR/report.txt"
exec > >(tee -a "$REPORT") 2>&1

section() {
  echo
  echo "======================================================================"
  echo "== $*"
  echo "======================================================================"
}

# Ejecuta un comando, lo muestra y guarda su salida en $OUTDIR/$file.
run_save() {
  local file="$1"; shift
  echo
  echo "### $file"
  echo "\$ $*"
  { echo "\$ $*"; "$@" 2>&1; } | tee "$OUTDIR/$file"
}

# Ejecuta una cadena shell, la muestra y guarda su salida en $OUTDIR/$file.
run_sh() {
  local file="$1"; shift
  local cmd="$*"
  echo
  echo "### $file"
  echo "\$ $cmd"
  { echo "\$ $cmd"; bash -c "$cmd" 2>&1; } | tee "$OUTDIR/$file"
}

pause() {
  if [ -t 0 ]; then
    echo
    read -rp "$* Pulsa Enter para continuar..." _
  else
    echo "(sin TTY; se omite la pausa: $*)"
  fi
}

section "MedionAudio porting collector v$VERSION"
echo "Fecha:  $(date -Is)"
echo "Salida: $OUTDIR"
echo "Usuario: $RUN_USER"

# ---------------------------------------------------------------------------
section "Dependencias"
MISSING=()
check_cmd() { command -v "$1" >/dev/null 2>&1 || MISSING+=("$1"); }
for c in aplay arecord speaker-test alsa-info.sh i2cdetect i2cdump i2cget \
         evtest gpioinfo acpidump dmidecode lspci lsusb modinfo; do
  check_cmd "$c"
done
if [ ${#MISSING[@]} -gt 0 ]; then
  echo "Faltan comandos: ${MISSING[*]}"
  echo "Paquetes: alsa-utils i2c-tools evtest gpiod acpica-tools dmidecode pciutils usbutils kmod"
  if [ "$INSTALL_DEPS" -eq 1 ] && command -v apt-get >/dev/null 2>&1; then
    echo "Instalando dependencias..."
    DEBIAN_FRONTEND=noninteractive apt-get update -qq || true
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
      alsa-utils i2c-tools evtest gpiod acpica-tools dmidecode pciutils usbutils kmod || true
  else
    echo "Instalalos manualmente o ejecuta otra vez con --install-deps."
  fi
else
  echo "Todas las dependencias presentes."
fi

# ---------------------------------------------------------------------------
section "Preparando kernel"
run_sh "logs/modprobe-i2c-dev.txt" "modprobe i2c-dev 2>&1 || true"
run_sh "logs/lsmod-i2c.txt" "lsmod | grep -iE 'i2c|gpio|pinctrl' || true"
if [ -d /sys/kernel/debug ]; then
  if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
    mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
  fi
  echo "debugfs: $(mountpoint -q /sys/kernel/debug && echo montado || echo NO montado)"
else
  echo "AVISO: /sys/kernel/debug no existe"
fi

# ---------------------------------------------------------------------------
section "Sistema"
run_save "system/uname.txt" uname -a
run_save "system/os-release.txt" cat /etc/os-release
run_save "system/cmdline.txt" cat /proc/cmdline
run_save "system/lscpu.txt" lscpu
run_save "system/free.txt" free -h
run_save "system/dmi-sysfs.txt" bash -c 'for f in sys_vendor product_name product_version board_vendor board_name board_version bios_vendor bios_version bios_date chassis_vendor chassis_type; do printf "%-18s " "$f"; cat "/sys/class/dmi/id/$f" 2>/dev/null || echo "(n/a)"; done'
run_save "system/dmidecode.txt" dmidecode -t system -t baseboard -t bios -t processor
run_save "system/hostnamectl.txt" hostnamectl

# ---------------------------------------------------------------------------
section "PCI / USB"
run_save "pci/lspci-nn.txt" lspci -nn
run_save "pci/lspci-vv.txt" lspci -vv
run_save "pci/lspci-tree.txt" lspci -t
run_save "pci/lsusb.txt" lsusb
AUDIO_PCI="$(lspci -nn 2>/dev/null | grep -i audio | head -n1 | awk '{print $1}')"
if [ -n "$AUDIO_PCI" ]; then
  echo "Audio PCI: $AUDIO_PCI"
  run_save "pci/audio-lspci-vv.txt" lspci -vv -s "$AUDIO_PCI"
  run_save "pci/audio-lspci-xxx.txt" lspci -xxx -s "$AUDIO_PCI"
  run_save "pci/audio-resource.txt" cat "/sys/bus/pci/devices/0000:$AUDIO_PCI/resource"
  {
    for f in "/sys/bus/pci/devices/0000:$AUDIO_PCI"/power/*; do
      [ -f "$f" ] || continue
      echo "$(basename "$f")=$(cat "$f" 2>/dev/null)"
    done
  } | tee "$OUTDIR/pci/audio-power.txt"
fi
run_save "pci/interrupts.txt" cat /proc/interrupts
run_save "pci/iomem.txt" cat /proc/iomem

# ---------------------------------------------------------------------------
section "Modulos del kernel"
run_save "system/lsmod.txt" lsmod
run_sh "system/lsmod-audio.txt" "lsmod | grep -iE 'snd|sof|es83|i2c|gpio|pinctrl' || true"
for m in snd_sof snd_sof_pci snd_sof_pci_intel_cnl snd_sof_pci_intel_tgl \
         snd_sof_pci_intel_adl snd_soc_es8316 snd_soc_sof_es8336; do
  run_save "system/modinfo-$m.txt" modinfo "$m"
done
{
  for d in /sys/module/snd_sof /sys/module/snd_sof_pci \
           /sys/module/snd_soc_es8316 /sys/module/snd_soc_sof_es8336; do
    [ -d "$d/parameters" ] || continue
    echo "== $d"
    for f in "$d"/parameters/*; do
      [ -f "$f" ] || continue
      echo "$(basename "$f")=$(cat "$f" 2>/dev/null)"
    done
  done
} | tee "$OUTDIR/system/module-params.txt"

# ---------------------------------------------------------------------------
section "ACPI"
mkdir -p "$OUTDIR/acpi"
cp /sys/firmware/acpi/tables/* "$OUTDIR/acpi/" 2>/dev/null || true
if command -v acpidump >/dev/null 2>&1; then
  ( cd "$OUTDIR/acpi" && acpidump -b >/dev/null 2>&1 ) || true
fi
if command -v iasl >/dev/null 2>&1; then
  for f in "$OUTDIR"/acpi/*.dat; do
    [ -f "$f" ] || continue
    iasl -d "$f" >/dev/null 2>&1 || true
  done
fi
{
  for f in "$OUTDIR"/acpi/*.dat; do
    [ -f "$f" ] || continue
    echo "== $f"
    strings "$f" | grep -i -E 'ESSX8336|ESSX8316|ES8336|ES8316|speaker|headphone|spk|hp|amp|gpio|i2c|_DSD|GpioIo|GpioInt' || true
  done
} | tee "$OUTDIR/acpi/essx-strings.txt"
{
  grep -R -i -n -E 'ESSX8336|ESSX8316|ES8336|ES8316|speaker|headphone|spk|hp|amp|gpio|i2c|_DSD|GpioIo|GpioInt' \
    "$OUTDIR"/acpi/*.dsl 2>/dev/null || true
} | tee "$OUTDIR/acpi/essx-dsl.txt"
run_save "acpi/listing.txt" ls -l "$OUTDIR/acpi"

# ---------------------------------------------------------------------------
section "Audio (ALSA / PipeWire)"
run_save "audio/proc-cards.txt" cat /proc/asound/cards
run_save "audio/proc-devices.txt" cat /proc/asound/devices
run_save "audio/proc-pcm.txt" cat /proc/asound/pcm
run_save "audio/aplay-l.txt" aplay -l
run_save "audio/aplay-L.txt" aplay -L
run_save "audio/arecord-l.txt" arecord -l
run_save "audio/arecord-L.txt" arecord -L

CARD_NUM="$(awk '/^[[:space:]]*[0-9]+ \[/ {gsub(/[^0-9]/,"",$1); print $1; exit}' /proc/asound/cards)"
CARD_ID="$(awk -F'[][]' '/^[[:space:]]*[0-9]+ \[/ {print $2; exit}' /proc/asound/cards | xargs)"
echo "Card: num=${CARD_NUM:-?} id=${CARD_ID:-?}"

if [ -n "$CARD_NUM" ]; then
  run_save "audio/amixer-controls.txt" amixer -c "$CARD_NUM" controls
  run_save "audio/amixer-contents.txt" amixer -c "$CARD_NUM" contents
  run_save "audio/alsactl-state.txt" alsactl store -f "$OUTDIR/audio/alsactl.state"
fi

if command -v alsa-info.sh >/dev/null 2>&1; then
  echo "Ejecutando alsa-info.sh (sin subir)..."
  alsa-info.sh --no-upload > "$OUTDIR/audio/alsa-info.txt" 2>&1 || true
fi
if command -v pactl >/dev/null 2>&1; then
  run_save "audio/pactl-info.txt" pactl info
  run_save "audio/pactl-cards.txt" pactl list cards
  run_save "audio/pactl-sinks.txt" pactl list sinks
  run_save "audio/pactl-sources.txt" pactl list sources
fi
if command -v wpctl >/dev/null 2>&1; then
  run_save "audio/wpctl-status.txt" wpctl status
fi
{
  for f in /proc/asound/card*/codec*; do
    [ -e "$f" ] || continue
    echo "== $f"
    cat "$f"
  done
} | tee "$OUTDIR/audio/proc-codec.txt"

# ---------------------------------------------------------------------------
section "SOF"
run_save "logs/dmesg.txt" dmesg
run_save "logs/journal-kernel.txt" journalctl -k -b --no-pager
run_sh "sof/dmesg-filtered.txt" "dmesg | grep -iE 'sof|es83|quirk|ipc|ssp|dmic|tplg|firmware|hda|audio|i2c|gpio' || true"
run_save "sof/debugfs-listing.txt" ls -lR /sys/kernel/debug/sof
for f in fw_version status; do
  [ -f "/sys/kernel/debug/sof/$f" ] && run_save "sof/debugfs-$f.txt" cat "/sys/kernel/debug/sof/$f"
done
{
  find /lib/firmware/intel -maxdepth 3 \
    \( -iname '*sof*' -o -iname '*es8336*' -o -iname '*cml*' \
       -o -iname '*tgl*' -o -iname '*adl*' \) \
    -printf '%p %s bytes\n' 2>/dev/null | sort || true
} | tee "$OUTDIR/sof/firmware-files.txt"
{
  find /lib/firmware/intel -maxdepth 3 -iname '*es8336*' \
    -printf '%p %s bytes\n' 2>/dev/null | sort || true
} | tee "$OUTDIR/sof/tplg-es8336.txt"
{
  cat /sys/kernel/debug/clk/clk_summary 2>/dev/null | grep -iE 'ssp|mclk|bclk|i2s|audio' || true
} | tee "$OUTDIR/sof/clocks.txt"
{
  cat /proc/interrupts | grep -iE 'sof|audio|hda|i2c|gpio' || true
} | tee "$OUTDIR/sof/interrupts.txt"

# ---------------------------------------------------------------------------
section "I2C"
run_save "i2c/i2cdetect-l.txt" i2cdetect -l
{
  for d in /sys/bus/i2c/devices/i2c-*; do
    [ -d "$d" ] || continue
    echo "$d: $(cat "$d/name" 2>/dev/null)"
  done
} | tee "$OUTDIR/i2c/buses.txt"
{
  for d in /sys/bus/i2c/devices/*-*; do
    [ -d "$d" ] || continue
    echo "$d: $(cat "$d/name" 2>/dev/null)"
  done
} | tee "$OUTDIR/i2c/clients.txt"

CODEC_BUS=""
CODEC_ADDR=""
CODEC_SYS="$(grep -l -i -E 'ESSX8336|ESSX8316|es8336|es8316' /sys/bus/i2c/devices/*/name 2>/dev/null | head -n1)"
if [ -n "$CODEC_SYS" ]; then
  CODEC_DIR="$(dirname "$CODEC_SYS")"
  CODEC_NAME="$(basename "$CODEC_DIR")"       # p.ej. i2c-0-0010
  CODEC_BUS="${CODEC_NAME#i2c-}"; CODEC_BUS="${CODEC_BUS%%-*}"
  CODEC_ADDR="0x${CODEC_NAME##*-}"
  echo "Codec detectado: $CODEC_DIR (bus=$CODEC_BUS addr=$CODEC_ADDR)"
else
  echo "AVISO: no se detecto ESSX8336/ES8316 en /sys/bus/i2c/devices/*/name"
fi
if [ -z "$CODEC_BUS" ] && [ -t 0 ]; then
  read -rp "Bus I2C del codec (numero, p.ej. 0), o Enter para omitir: " CODEC_BUS
  if [ -n "$CODEC_BUS" ]; then
    read -rp "Direccion I2C (hex, p.ej. 0x10): " CODEC_ADDR
  fi
fi
if [ -n "$CODEC_BUS" ] && [ -n "$CODEC_ADDR" ]; then
  run_save "i2c/i2cdetect-codec-bus.txt" i2cdetect -y -r "$CODEC_BUS"
  run_save "i2c/i2cdump-codec-idle.txt" i2cdump -y -f "$CODEC_BUS" "$CODEC_ADDR"
  run_sh "i2c/i2cget-0x4f-idle.txt" "i2cget -y -f $CODEC_BUS $CODEC_ADDR 0x4f"
fi

# ---------------------------------------------------------------------------
section "GPIO / pinctrl"
run_save "gpio/debugfs-gpio.txt" cat /sys/kernel/debug/gpio
if command -v gpioinfo >/dev/null 2>&1; then
  run_save "gpio/gpioinfo.txt" gpioinfo
fi
run_save "gpio/pinctrl-listing.txt" ls -lR /sys/kernel/debug/pinctrl
{
  for d in /sys/kernel/debug/pinctrl/*; do
    [ -d "$d" ] || continue
    echo "== $d"
    [ -f "$d/pinmux-pins" ] && cat "$d/pinmux-pins"
  done
} | tee "$OUTDIR/gpio/pinmux-pins.txt"
{
  for d in /sys/kernel/debug/pinctrl/*; do
    [ -d "$d" ] || continue
    echo "== $d"
    [ -f "$d/pins" ] && cat "$d/pins"
  done
} | tee "$OUTDIR/gpio/pins.txt"
{
  cat /sys/kernel/debug/regulator/regulator_summary 2>/dev/null || true
} | tee "$OUTDIR/gpio/regulator-summary.txt"
{
  cat /sys/kernel/debug/clk/clk_summary 2>/dev/null | grep -iE 'ssp|mclk|bclk|i2s|audio' || true
} | tee "$OUTDIR/gpio/clocks.txt"

# ---------------------------------------------------------------------------
dump_codec_state() {
  local label="$1"
  [ -n "$CODEC_BUS" ] && [ -n "$CODEC_ADDR" ] || return 0
  i2cdump -y -f "$CODEC_BUS" "$CODEC_ADDR" > "$OUTDIR/codec/regs-$label.txt" 2>&1 || true
  {
    echo "== i2cget 0x4f"
    i2cget -y -f "$CODEC_BUS" "$CODEC_ADDR" 0x4f 2>&1 || true
    echo "== gpio"
    cat /sys/kernel/debug/gpio 2>/dev/null || true
    echo "== gpioinfo"
    if command -v gpioinfo >/dev/null 2>&1; then gpioinfo 2>&1 || true; fi
    echo "== hw_params"
    cat /proc/asound/card*/pcm*/sub*/hw_params 2>/dev/null || true
  } > "$OUTDIR/codec/state-$label.txt" 2>&1
  echo "  -> codec/regs-$label.txt"
}

TONE_PID=""
stop_tone() {
  if [ -n "${TONE_PID:-}" ]; then
    kill "$TONE_PID" 2>/dev/null || true
    wait "$TONE_PID" 2>/dev/null || true
    TONE_PID=""
  fi
}
play_tone() {
  local pcm="$1"
  command -v speaker-test >/dev/null 2>&1 || return 1
  timeout 30 bash -c "while :; do speaker-test -D '$pcm' -c 2 -t sine -f 1000 -l 1 >/dev/null 2>&1 || break; done" &
  TONE_PID=$!
  sleep 2
  return 0
}

if [ "$DO_AUDIO" -eq 1 ]; then
  section "Estados del codec"
  TONE_PCM=""
  for p in "${PCM:-}" "plughw:${CARD_NUM:-0},0" "default"; do
    [ -n "$p" ] || continue
    if timeout 3 speaker-test -D "$p" -c 2 -t sine -f 1000 -l 1 >/dev/null 2>&1; then
      TONE_PCM="$p"
      break
    fi
  done
  echo "PCM para el tono: ${TONE_PCM:-no disponible}"

  if [ -t 0 ]; then
    pause "Desconecta auriculares y salidas externas. No reproduzcas audio."
    dump_codec_state "idle"

    pause "Vamos a reproducir un tono en los altavoces internos. Sube el volumen."
    play_tone "$TONE_PCM" || echo "No se pudo reproducir el tono."
    sleep 1
    dump_codec_state "speakers"
    stop_tone

    pause "Conecta auriculares y comprueba que se oyen."
    play_tone "$TONE_PCM" || true
    sleep 1
    dump_codec_state "headphones"
    stop_tone

    pause "Conecta un headset de 4 polos (con microfono)."
    dump_codec_state "headset-plugged"
  else
    dump_codec_state "idle"
  fi
fi

# ---------------------------------------------------------------------------
section "Captura (DMIC / micro)"
if [ "$DO_AUDIO" -eq 1 ]; then
  mkdir -p "$OUTDIR/capture"
  while IFS= read -r line; do
    card="$(echo "$line" | sed -n 's/^card \([0-9]*\):.*/\1/p')"
    dev="$(echo "$line" | sed -n 's/.*device \([0-9]*\):.*/\1/p')"
    [ -n "$card" ] && [ -n "$dev" ] || continue
    out="$OUTDIR/capture/card${card}-dev${dev}.wav"
    echo "Grabando 3 s de hw:$card,$dev -> $out"
    timeout 8 arecord -D "hw:$card,$dev" -f S32_LE -r 48000 -c 2 -d 3 "$out" >/dev/null 2>&1 || \
    timeout 8 arecord -D "plughw:$card,$dev" -r 48000 -c 2 -d 3 "$out" >/dev/null 2>&1 || \
    echo "  fallo al grabar card=$card dev=$dev"
    arecord --dump-hw-params -D "hw:$card,$dev" -d 1 /dev/null \
      > "$OUTDIR/capture/hw-params-card${card}-dev${dev}.txt" 2>&1 || true
  done < <(arecord -l 2>/dev/null)
fi

# ---------------------------------------------------------------------------
section "Jack / evtest"
if command -v evtest >/dev/null 2>&1 && [ -t 0 ]; then
  echo "Dispositivos de entrada (eventN + nombre):"
  awk '
    /^N: Name=/ {name=$0; sub(/^N: Name="/,"",name); sub(/"$/,"",name)}
    /^H: Handlers=/ {h=$0; if (match(h, /event[0-9]+/)) print substr(h,RSTART,RLENGTH) "  " name}
  ' /proc/bus/input/devices
  read -rp "Numero de eventN del jack (o Enter para omitir): " EVNUM
  if [ -n "$EVNUM" ]; then
    echo "Conecta y desconecta los auriculares varias veces durante 20 s..."
    timeout 20 evtest "/dev/input/event$EVNUM" | tee "$OUTDIR/audio/evtest-jack.txt" || true
  fi
fi

# ---------------------------------------------------------------------------
section "Resumen"
{
  echo "MedionAudio porting report"
  echo "Fecha: $(date -Is)"
  echo
  echo "== DMI =="
  for f in sys_vendor product_name board_name board_version bios_version; do
    printf "%-18s %s\n" "$f" "$(cat "/sys/class/dmi/id/$f" 2>/dev/null)"
  done
  echo
  echo "== CPU =="
  lscpu | grep -E 'Model name|Vendor ID|CPU family|Model:|Stepping' || true
  echo
  echo "== Kernel =="
  uname -a
  echo
  echo "== Audio PCI =="
  lspci -nn | grep -i audio || true
  echo
  echo "== Codec I2C =="
  echo "bus=${CODEC_BUS:-?} addr=${CODEC_ADDR:-?}"
  echo
  echo "== SOF / quirk / tplg =="
  dmesg | grep -iE 'sof|es83|quirk|ipc|ssp|dmic|tplg|firmware' | tail -n 120 || true
  echo
  echo "== GPIO (lineas con speaker/headphone/amp) =="
  grep -iE 'speaker|headphone|spk|hp|amp' "$OUTDIR/gpio/debugfs-gpio.txt" 2>/dev/null || true
  echo
  echo "== PCMs =="
  cat "$OUTDIR/audio/aplay-l.txt" 2>/dev/null || true
  echo
  cat "$OUTDIR/audio/arecord-l.txt" 2>/dev/null || true
} | tee "$OUTDIR/SUMMARY.txt"

# ---------------------------------------------------------------------------
section "Empaquetando"
TARBALL="$OUTDIR.tar.gz"
tar -czf "$TARBALL" -C "$(dirname "$OUTDIR")" "$(basename "$OUTDIR")" 2>/dev/null || true
if [ "$RUN_USER" != "root" ]; then
  chown -R "$RUN_USER:$RUN_GROUP" "$OUTDIR" "$TARBALL" 2>/dev/null || true
fi
echo
echo "Informe:  $OUTDIR"
echo "Resumen:  $OUTDIR/SUMMARY.txt"
echo "Tarball:  $TARBALL"
echo
echo "Pega o adjunta el tarball y, si puedes, el SUMMARY.txt."
