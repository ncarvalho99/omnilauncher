<#
.SYNOPSIS
Builds SwitchU locally on Windows through the pinned Docker environment.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-local.ps1 -Mode release -Variant sysmodule
#>
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')] [string] $Mode = 'release',
    [ValidateSet('sysmodule', 'homebrew')] [string] $Variant = 'sysmodule',
    [ValidatePattern('^[A-Za-z]:$')] [string] $ConsoleDrive = 'E:',
    [switch] $TerminationQueueTest,
    [switch] $PreflightMatrixTest,
    [switch] $PreflightEdgeTest,
    [switch] $ResumeFailureTest,
    [switch] $SkipConsoleDeploy
)

$ErrorActionPreference = 'Stop'
$terminationQueueTestMode = if ($TerminationQueueTest) { 'on' } else { 'off' }
$preflightMatrixTestMode = if ($PreflightMatrixTest) { 'on' } else { 'off' }
$preflightEdgeTestMode = if ($PreflightEdgeTest) { 'on' } else { 'off' }
$resumeFailureTestMode = if ($ResumeFailureTest) { 'on' } else { 'off' }
if ($TerminationQueueTest -and $Variant -ne 'sysmodule') {
    throw '-TerminationQueueTest is valid only for the sysmodule variant.'
}
if ($PreflightMatrixTest -and $Variant -ne 'sysmodule') {
    throw '-PreflightMatrixTest is valid only for the sysmodule variant.'
}
if ($PreflightEdgeTest -and $Variant -ne 'sysmodule') {
    throw '-PreflightEdgeTest is valid only for the sysmodule variant.'
}
if ($ResumeFailureTest -and $Variant -ne 'sysmodule') {
    throw '-ResumeFailureTest is valid only for the sysmodule variant.'
}
$diagnosticModeCount = @(
    @($TerminationQueueTest, $PreflightMatrixTest, $PreflightEdgeTest, $ResumeFailureTest) |
        Where-Object { [bool]$_ }
).Count
if ($diagnosticModeCount -gt 1) {
    throw 'Use only one diagnostic build mode at a time.'
}
$dockerRoot = 'C:\Program Files\Docker\Docker\resources\bin'
$docker = Join-Path $dockerRoot 'docker.exe'

if (-not (Test-Path -LiteralPath $docker)) {
    throw "Docker Desktop was not found at $dockerRoot"
}

# Docker Desktop's credential helpers live beside docker.exe. A new Windows
# installation may omit this directory from PATH, which breaks anonymous pulls.
$env:PATH = "$dockerRoot;$env:PATH"

$buildImage = 'switchu-build:latest'
# `docker image inspect switchu-build` can transiently report "No such image"
# while Docker Desktop is finishing its local-image refresh. Listing the image
# is quiet both when it exists and when it does not, so it is safe under the
# script's strict error mode.
$buildImageId = @(& $docker image ls --quiet $buildImage | Select-Object -First 1)
if ($buildImageId.Count -eq 0 -or [string]::IsNullOrWhiteSpace($buildImageId[0])) {
    throw "A imagem local 'switchu-build' nao existe. Execute .\tools\build-image.ps1 -PullBase primeiro."
}

$repo = Split-Path -Parent $PSScriptRoot
$logsDirectory = Join-Path $repo '.logs'
New-Item -ItemType Directory -Force -Path $logsDirectory | Out-Null
$log = Join-Path $logsDirectory 'build-local.log'

function Copy-TreeToConsole {
    param(
        [Parameter(Mandatory)] [string] $Source,
        [Parameter(Mandatory)] [string] $Destination,
        [Parameter(Mandatory)] [string] $Label
    )

    & robocopy $Source $Destination /E /IS /IT /COPY:DAT /DCOPY:DAT /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Host
    $copyExitCode = $LASTEXITCODE
    # Robocopy uses 0–7 for success, including files copied or metadata changed.
    if ($copyExitCode -gt 7) {
        throw "Falha ao copiar $Label para o cartao (robocopy exit $copyExitCode)."
    }
}

function Remove-RetiredDefaultThemeMedia {
    param(
        [Parameter(Mandatory)] [string] $SourceSwitchU,
        [Parameter(Mandatory)] [string] $ConsoleSwitchU
    )

    # The default themes are shipped by SwitchU, but third-party themes in this
    # folder belong to the user. Delete only the two retired built-in media
    # directories when the build intentionally no longer contains them.
    foreach ($themeName in @('Default Dark', 'Default Light')) {
        $sourceMedia = Join-Path $SourceSwitchU ("themes\\{0}\\media" -f $themeName)
        $consoleMedia = Join-Path $ConsoleSwitchU ("themes\\{0}\\media" -f $themeName)
        if ((Test-Path -LiteralPath $sourceMedia) -or -not (Test-Path -LiteralPath $consoleMedia)) {
            continue
        }

        $expectedMedia = [System.IO.Path]::GetFullPath($consoleMedia)
        $actualMedia = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $consoleMedia).Path)
        if ($actualMedia -ne $expectedMedia) {
            throw "Destino de limpeza inesperado: $actualMedia"
        }

        $fileCount = @(Get-ChildItem -LiteralPath $consoleMedia -Recurse -File).Count
        Write-Host "removendo $fileCount assets antigos do tema interno $themeName" -ForegroundColor Cyan
        Remove-Item -LiteralPath $consoleMedia -Recurse -Force
    }
}

function Deploy-SysmoduleToConsole {
    if ($SkipConsoleDeploy) {
        Write-Host 'implantacao no console ignorada por parametro' -ForegroundColor Yellow
        return
    }

    if (-not (Test-Path -LiteralPath $ConsoleDrive)) {
        Write-Host "cartao do console ($ConsoleDrive) nao conectado; ZIP local mantido" -ForegroundColor Yellow
        return
    }

    $sourceRoot = Join-Path $repo ("dist\{0}\{1}" -f $Variant, $Mode)
    $sourceAtmosphere = Join-Path $sourceRoot 'atmosphere'
    $sourceSwitch = Join-Path $sourceRoot 'switch'
    $destinationAtmosphere = Join-Path $ConsoleDrive 'atmosphere'
    $destinationSwitch = Join-Path $ConsoleDrive 'switch'
    $consoleSwitchU = Join-Path $destinationSwitch 'SwitchU'

    if (-not (Test-Path -LiteralPath $sourceAtmosphere) -or
        -not (Test-Path -LiteralPath $sourceSwitch)) {
        throw "Layout de build sysmodule incompleto em $sourceRoot; nao foi copiado."
    }
    if (-not (Test-Path -LiteralPath $destinationAtmosphere) -or
        -not (Test-Path -LiteralPath $consoleSwitchU)) {
        throw "$ConsoleDrive esta montado, mas nao possui o layout esperado do cartao Switch; copia cancelada."
    }

    Write-Host "implantando sysmodule no cartao $ConsoleDrive ..." -ForegroundColor Cyan
    Copy-TreeToConsole -Source $sourceAtmosphere -Destination $destinationAtmosphere -Label 'atmosphere'
    Copy-TreeToConsole -Source $sourceSwitch -Destination $destinationSwitch -Label 'switch'
    Remove-RetiredDefaultThemeMedia -SourceSwitchU (Join-Path $sourceSwitch 'SwitchU') -ConsoleSwitchU $consoleSwitchU

    # Garante que todos os arquivos i18n sejam copiados sem serem ignorados pelo robocopy no FAT32
    $sourceI18n = Join-Path $sourceSwitch 'SwitchU\i18n'
    $consoleI18n = Join-Path $consoleSwitchU 'i18n'
    if (Test-Path -LiteralPath $sourceI18n) {
        if (-not (Test-Path -LiteralPath $consoleI18n)) {
            New-Item -ItemType Directory -Path $consoleI18n -Force | Out-Null
        }
        Get-ChildItem -LiteralPath $sourceI18n -Filter *.json | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $consoleI18n $_.Name) -Force
        }
    }

    $checks = @(
        @{ Source = Join-Path $sourceAtmosphere 'contents\0100000000001000\exefs.nsp'; Destination = Join-Path $destinationAtmosphere 'contents\0100000000001000\exefs.nsp' },
        @{ Source = Join-Path $sourceSwitch 'SwitchU\bin\menu\main'; Destination = Join-Path $destinationSwitch 'SwitchU\bin\menu\main' },
        @{ Source = Join-Path $sourceSwitch 'SwitchU\bin\menu\main.npdm'; Destination = Join-Path $destinationSwitch 'SwitchU\bin\menu\main.npdm' }
    )
    foreach ($check in $checks) {
        $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $check.Source).Hash
        $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $check.Destination).Hash
        if ($sourceHash -ne $destinationHash) {
            throw "Verificacao SHA-256 falhou para $($check.Destination)."
        }
    }
    Write-Host "cartao $ConsoleDrive atualizado e verificado; ejete-o com seguranca antes de usar no Switch" -ForegroundColor Green
}

if (& $docker ps -q --filter "ancestor=$buildImage") {
    throw 'Ja existe um build SwitchU rodando. Espere ele terminar antes de iniciar outro.'
}

if ([string]::IsNullOrWhiteSpace($env:SWITCHU_CLIENT_KEY)) {
    $clientKeyFile = Join-Path $env:USERPROFILE '.switchu\client-key'
    if (Test-Path $clientKeyFile) {
        $env:SWITCHU_CLIENT_KEY = (Get-Content -Path $clientKeyFile -Raw).Trim()
    }
}
if ([string]::IsNullOrWhiteSpace($env:SWITCHU_CLIENT_KEY)) {
    throw 'Official client key is missing! Set SWITCHU_CLIENT_KEY environment variable or create %USERPROFILE%\.switchu\client-key.'
}

Write-Host "build $Mode/$Variant -> $log" -ForegroundColor Cyan
$started = Get-Date

# xmake installs packages below /root/.xmake. Keep that state in a named volume:
# its project cache survives on the bind mount, so discarding package state at
# every --rm run would make later builds incorrectly think dependencies exist.
$ErrorActionPreference = 'Continue'
& $docker run --rm -v "${repo}:/src" -v switchu-xmake:/root/.xmake -e "SWITCHU_CLIENT_KEY=$($env:SWITCHU_CLIENT_KEY)" `
    $buildImage bash /src/tools/build-inside.sh $Mode $Variant $terminationQueueTestMode $preflightMatrixTestMode $preflightEdgeTestMode $resumeFailureTestMode |
    Tee-Object -FilePath $log
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'

$seconds = [int]((Get-Date) - $started).TotalSeconds
$minutes = [int]($seconds / 60)
$elapsed = if ($minutes -gt 0) { "${minutes}m $($seconds % 60)s" } else { "${seconds}s" }

if ($rc -eq 0) {
    Write-Host "ok em $elapsed" -ForegroundColor Green
    if ($Variant -eq 'sysmodule') {
        Deploy-SysmoduleToConsole
    } else {
        Write-Host 'build homebrew nao e implantado automaticamente no cartao' -ForegroundColor Yellow
    }
} else {
    Write-Host "falhou (exit $rc) em $elapsed -- log completo em $log" -ForegroundColor Red
}
exit $rc
