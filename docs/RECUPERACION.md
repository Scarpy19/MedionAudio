# Recuperación de MedionAudio

Esta copia corresponde al MEDION E15415 con macOS 15.8.2. Altavoces, auriculares y micrófono del jack se han probado; el micrófono interno no funciona tampoco en Linux ni Windows y probablemente tenga un problema físico, sin confirmar. La suspensión no se puede evaluar de forma fiable en este Hackintosh.

## Contenido de la copia final

- `EFI-opencore.tar.gz`: la carpeta EFI de arranque **sin `EFI/Microsoft`**. Incluye OpenCore, la configuración y los kexts de audio y sus dependencias.
- `HAL.tar.gz`: `MedionAudio.driver`, el plugin de CoreAudio que estaba instalado.
- `arranque.tar.gz`: `medion_ctl`, `medion_boot_init.sh` y el LaunchDaemon.
- `proyecto.tar.gz`: código fuente y documentación del proyecto.
- `SHA256SUMS`: hashes para comprobar la integridad de los archivos.

La copia no incluye `EFI/Microsoft`; nunca la elimines ni la sobrescribas al restaurar audio. Los archivos de Windows pueden haber cambiado desde la copia.

## Comprobar antes de restaurar

En macOS, abre Terminal y entra en la carpeta que contiene los archivos de la copia:

```bash
cd /ruta/a/MedionAudio-recuperacion
shasum -a 256 -c SHA256SUMS
```

Revisa `diskutil list` e identifica la partición EFI correcta antes de montarla. En este equipo era `disk0s1`, pero **el número puede cambiar**. El kext va exclusivamente en `EFI/OC/Kexts/MedionAudio.kext`; **no lo instales en `/Library/Extensions`**.

## Restaurar el plugin y el servicio

Desde la carpeta de la copia, descomprime primero en una carpeta nueva:

```bash
mkdir -p "$HOME/Desktop/MedionAudio-restaurar"
tar -xzf HAL.tar.gz -C "$HOME/Desktop/MedionAudio-restaurar"
tar -xzf arranque.tar.gz -C "$HOME/Desktop/MedionAudio-restaurar"
```

Después copia los componentes guardados. Si ya existen y contienen modificaciones que quieras conservar, haz una copia antes: `ditto` puede sobrescribir archivos del mismo nombre.

```bash
R="$HOME/Desktop/MedionAudio-restaurar"
sudo mkdir -p /Library/Audio/Plug-Ins/HAL /usr/local/libexec/medion
sudo ditto "$R/MedionAudio.driver" /Library/Audio/Plug-Ins/HAL/MedionAudio.driver
sudo ditto "$R/medion" /usr/local/libexec/medion
sudo install -m 644 -o root -g wheel "$R/com.hackintosh.medionaudio.init.plist" /Library/LaunchDaemons/
sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/MedionAudio.driver /usr/local/libexec/medion
sudo launchctl bootout system /Library/LaunchDaemons/com.hackintosh.medionaudio.init.plist 2>/dev/null || true
sudo launchctl bootstrap system /Library/LaunchDaemons/com.hackintosh.medionaudio.init.plist
```

El servicio intenta iniciar el DSP y, si lo consigue, reinicia `coreaudiod`. Su registro es `/var/log/medion_audio_init.log`.

## Restaurar los componentes de OpenCore

Descomprime `EFI-opencore.tar.gz` en otra carpeta de trabajo y monta la ESP correcta. Copia a `EFI/OC/Kexts/` el `MedionAudio.kext` guardado. La configuración probada también requiere `VoodooI2CServices.kext` y `VoodooGPIO.kext` independientes, cargados **antes** de `VoodooI2C.kext`; `MedionAudio.kext` va después. El VoodooI2C de esta copia incluye el identificador I2C4 `8086:02c5`.

El `EFI/OC/config.plist` guardado contiene todo el estado de OpenCore, no solo audio. Restaúralo únicamente si deseas volver a esa configuración completa; de otro modo incorpora las entradas y el parche a tu configuración actual. **No sustituyas toda la carpeta EFI ni toques `EFI/Microsoft`.** Reinicia para que OpenCore inyecte los kexts.

## Verificación

```bash
kmutil showloaded | grep -Ei 'VoodooI2C|MedionAudio'
/usr/local/libexec/medion/medion_ctl status
sudo launchctl print system/com.hackintosh.medionaudio.init
```

El estado esperado es DSP `Active`, topología sin errores y un `VoodooI2CDeviceNub` para `ESSX8326` en `0x19`. `kmutil showloaded` debe mostrar el kext; su mera presencia en la EFI no demuestra que se haya cargado. Prueba altavoces, auriculares y micrófono del jack después de restaurar.
