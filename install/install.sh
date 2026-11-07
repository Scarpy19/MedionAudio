#!/bin/zsh
# Installs the macOS side of Medion audio: HAL plugin, medion_ctl, boot-init LaunchDaemon.
# The kext itself is injected by OpenCore (EFI/OC/Kexts/MedionAudio.kext).
# Usage: sudo ./install.sh          (uninstall: sudo ./install.sh uninstall)
set -e
[[ $EUID -eq 0 ]] || { echo "run it with sudo"; exit 1; }
D=${0:A:h}/..
HAL=/Library/Audio/Plug-Ins/HAL/MedionAudio.driver
LIB=/usr/local/libexec/medion
PLIST=/Library/LaunchDaemons/com.hackintosh.medionaudio.init.plist

if [[ $1 == uninstall ]]; then
  launchctl bootout system $PLIST 2>/dev/null || true
  rm -rf $PLIST $LIB $HAL
  killall coreaudiod 2>/dev/null || true
  echo "uninstalled"; exit 0
fi

U=${SUDO_USER:-root}
sudo -u $U make -C $D/plugin
sudo -u $U make -C $D/tools medion_ctl

rm -rf $HAL
mkdir -p /Library/Audio/Plug-Ins/HAL
cp -R $D/plugin/MedionAudio.driver $HAL
install -d -m 755 $LIB
install -m 755 -o root -g wheel $D/tools/medion_ctl $D/tools/medion_boot_init.sh $LIB/
install -m 644 -o root -g wheel $D/install/com.hackintosh.medionaudio.init.plist $PLIST
launchctl bootout system $PLIST 2>/dev/null || true
launchctl bootstrap system $PLIST      # runs the init script now (it restarts coreaudiod)
echo "audio installed (log: /var/log/medion_audio_init.log)"
