# Diagnostico de audio SOF/ES8326 para MEDION. Compatible con Windows PowerShell 5.1.
# Solo lectura: no graba audio, no instala software ni modifica controladores.
# Se recomienda ejecutar como administrador para incluir recursos de dispositivos.
$ErrorActionPreference = 'Continue'
$desktop = [Environment]::GetFolderPath('Desktop')
if (-not $desktop) { $desktop = $env:USERPROFILE }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $desktop "medion-audio-windows-$stamp"
New-Item -ItemType Directory -Path $out -ErrorAction Stop | Out-Null

function Save-Report {
    param([string]$Name, [scriptblock]$Command)
    $path = Join-Path $out $Name
    try {
        & $Command 2>&1 | Out-String -Width 240 | Set-Content -LiteralPath $path -Encoding UTF8
    } catch {
        ("Error: " + $_.Exception.Message) | Set-Content -LiteralPath $path -Encoding UTF8
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Save-Report '00-sistema.txt' {
    "Fecha: $(Get-Date -Format o)"
    "Ejecutado como administrador: $isAdmin"
    Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer, Model, SystemType
    Get-CimInstance Win32_BaseBoard | Select-Object Manufacturer, Product, Version
    Get-CimInstance Win32_BIOS | Select-Object Manufacturer, SMBIOSBIOSVersion, ReleaseDate
    Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture
}

# Includes inactive devices so we can identify a hidden microphone or disconnected headset.
Save-Report '01-dispositivos-pnp.txt' {
    Get-PnpDevice | Where-Object {
        $_.Class -in @('AudioEndpoint', 'MEDIA', 'Sound', 'System', 'SoftwareComponent', 'HIDClass') -or
        $_.InstanceId -match 'ESSX|ES8326|INTELAUDIO|VEN_8086&DEV_02C8|VEN_8086&DEV_02C5|DMIC|AUDIO'
    } | Sort-Object Class, FriendlyName | Format-List Status, Class, FriendlyName, InstanceId, Problem
}

Save-Report '02-dispositivos-cim.txt' {
    Get-CimInstance Win32_PnPEntity | Where-Object {
        $_.PNPClass -in @('AudioEndpoint', 'MEDIA', 'Sound', 'System', 'SoftwareComponent') -or
        $_.PNPDeviceID -match 'ESSX|ES8326|INTELAUDIO|VEN_8086&DEV_02C8|VEN_8086&DEV_02C5|DMIC|AUDIO'
    } | Select-Object Name, Manufacturer, PNPClass, PNPDeviceID, HardwareID, CompatibleID,
        Service, Status, ConfigManagerErrorCode | Format-List
}

Save-Report '03-controladores-firmados.txt' {
    Get-CimInstance Win32_PnPSignedDriver | Where-Object {
        $_.DeviceClass -match 'MEDIA|AudioEndpoint|Sound|System|SoftwareComponent' -or
        $_.DeviceID -match 'ESSX|ES8326|INTELAUDIO|VEN_8086&DEV_02C8|VEN_8086&DEV_02C5|DMIC|AUDIO'
    } | Select-Object DeviceName, DeviceID, Manufacturer, DriverProviderName, DriverVersion,
        DriverDate, InfName, DriverName, IsSigned | Format-List
}

Save-Report '04-dispositivos-sonido.txt' {
    Get-CimInstance Win32_SoundDevice | Format-List Name, Manufacturer, PNPDeviceID, Status
    Get-Service AudioEndpointBuilder, Audiosrv | Format-List Name, Status, StartType
}

# Microphone endpoint names, availability and associated device properties (no audio samples).
Save-Report '05-entradas-audio-registro.txt' {
    $root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
    Get-ChildItem -LiteralPath $root | ForEach-Object {
        "`n=== $($_.PSChildName) ==="
        Get-ItemProperty -LiteralPath $_.PSPath | Format-List *
        $props = Join-Path $_.PSPath 'Properties'
        if (Test-Path -LiteralPath $props) { Get-ItemProperty -LiteralPath $props | Format-List * }
    }
}

$targets = @(Get-PnpDevice | Where-Object {
    $_.Class -in @('AudioEndpoint', 'MEDIA', 'Sound') -or
    $_.InstanceId -match 'ESSX|ES8326|INTELAUDIO|VEN_8086&DEV_02C8|VEN_8086&DEV_02C5|DMIC'
})
Save-Report '06-propiedades-y-recursos.txt' {
    foreach ($device in $targets) {
        "`n=== $($device.FriendlyName) [$($device.InstanceId)] ==="
        try { Get-PnpDeviceProperty -InstanceId $device.InstanceId | Format-List KeyName, Type, Data } catch { $_ }
        # pnputil options differ between Windows builds; include errors rather than guessing.
        & pnputil.exe /enum-devices /instanceid $device.InstanceId /resources 2>&1
    }
}

Save-Report '07-paquetes-controlador.txt' {
    & pnputil.exe /enum-drivers 2>&1
}

Save-Report 'LEEME.txt' {
    'Informe de diagnostico de audio Medion.'
    'No contiene grabaciones de audio ni cambia la configuracion.'
    'Puede incluir nombres de dispositivo, identificadores de hardware y rutas de controladores.'
    'Revise los archivos antes de compartirlos si desea eliminar informacion.'
    if (-not $isAdmin) { 'AVISO: no se ejecuto como administrador; algunos recursos pueden faltar.' }
    'Indique por separado el nombre del microfono interno seleccionado en Configuracion > Sistema > Sonido > Entrada.'
}

$zip = "$out.zip"
try {
    Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -CompressionLevel Optimal -ErrorAction Stop
    Write-Host "`nInforme creado: $zip" -ForegroundColor Green
    Write-Host "La carpeta sin comprimir tambien se conserva: $out"
} catch {
    Write-Warning "No se pudo crear el ZIP: $($_.Exception.Message)"
    Write-Host "Comparta la carpeta: $out"
}
if (-not $isAdmin) { Write-Warning 'Ejecute como administrador si faltan recursos de dispositivos.' }
