# Compile, flashe et surveille Apple2_ProjectESP32.
#
#   .\build.ps1                    compile seulement
#   .\build.ps1 -Flash             compile et flashe (port detecte automatiquement)
#   .\build.ps1 -Flash -Monitor 20 ... puis affiche 20 s de journal serie
#   .\build.ps1 -Monitor 20        journal serie seul (reset de la carte), sans compiler
#   .\build.ps1 -Env wifi -Flash   second firmware (transfert de fichiers par WiFi), partition ota_1
#
# ESP-IDF refuse les chemins contenant des espaces : les sources de ce dossier
# sont donc recopiees dans $BuildRoot avant la compilation.
#
# (Fichier volontairement sans accents : Windows PowerShell 5.1 lit les scripts
# sans BOM dans la page de code ANSI.)

param(
    [string]$Env = "psram",
    [switch]$Flash,
    [int]$Monitor = 0,
    [string]$Port = "",
    [string]$BuildRoot = "C:\Apple2_ProjectESP32_build"
)

# "Continue" : avec "Stop", Windows PowerShell traite la moindre ligne ecrite sur
# stderr par python/PlatformIO (simples avertissements) comme une erreur fatale.
# Les echecs reels sont detectes par les codes de retour.
$ErrorActionPreference = "Continue"

$src = Join-Path $PSScriptRoot "Apple2_ProjectESP32"
$dst = Join-Path $BuildRoot "Apple2_ProjectESP32"
$compile = $Flash -or ($Monitor -eq 0)

# Python : le premier qui a le module platformio (celui du PATH peut etre un venv sans lui)
$candidates = @((Get-Command python -All -ErrorAction SilentlyContinue).Source)
$candidates += Get-ChildItem "$env:LOCALAPPDATA\Python\*\python.exe", "$env:LOCALAPPDATA\Programs\Python\*\python.exe",
                             "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -ErrorAction SilentlyContinue |
               ForEach-Object FullName
$python = $null
foreach ($c in $candidates) {
    if (-not $c) { continue }
    & $c -c "import platformio, serial" 2>$null
    if ($LASTEXITCODE -eq 0) { $python = $c; break }
}
if (-not $python) { throw "Aucun Python avec PlatformIO trouve (pip install platformio)." }

if (($Flash -or $Monitor -gt 0) -and -not $Port) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Where-Object { $_ -ne "COM1" }
    if (-not $ports) { throw "Aucun port serie detecte : branche la carte ou precise -Port COMx." }
    $Port = @($ports)[-1]
    Write-Host "Port serie : $Port" -ForegroundColor Green
}

if ($compile) {
    # Synchronisation vers le dossier de build : miroir exact des sources.
    # Les sdkconfig.* sont traites a part avec /XO (ne remplace pas un fichier plus
    # recent cote build) : PlatformIO les reecrit, et les recopier a chaque fois
    # declencherait une recompilation complete.
    Write-Host "Synchronisation vers $dst" -ForegroundColor Cyan
    robocopy $src $dst /MIR /XD .pio /XF dependencies.lock sdkconfig.* /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy a echoue (code $LASTEXITCODE)." }
    robocopy $src $dst sdkconfig.* /XO /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy a echoue (code $LASTEXITCODE)." }

    Push-Location $dst
    try {
        $pioArgs = @("-m", "platformio", "run", "-e", $Env)
        # Le firmware "wifi" va dans la seconde partition d'application : il est
        # flashe a part, sans toucher a l'emulateur ni a la table des partitions
        if ($Flash -and $Env -ne "wifi") { $pioArgs += @("--target", "upload", "--upload-port", $Port) }
        & $python @pioArgs
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO a echoue (code $LASTEXITCODE)." }
        if ($Flash -and $Env -eq "wifi") {
            $esptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
            & $python $esptool --chip esp32 --port $Port --baud 460800 write_flash 0x220000 ".pio\build\wifi\firmware.bin"
            if ($LASTEXITCODE -ne 0) { throw "esptool a echoue (code $LASTEXITCODE)." }
        }
    } finally {
        Pop-Location
    }

    # PlatformIO peut normaliser le sdkconfig : la version de build fait reference
    $sdk = "sdkconfig.$Env"
    if (Test-Path (Join-Path $dst $sdk)) {
        robocopy $dst $src $sdk /XO /NFL /NDL /NJH /NJS /NP | Out-Null
    }
}

if ($Monitor -gt 0) {
    & $python (Join-Path $PSScriptRoot "scripts\read_serial.py") $Port 115200 $Monitor
}
