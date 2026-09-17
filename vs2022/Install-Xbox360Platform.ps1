#Requires -Version 5.1
<#
.SYNOPSIS
    Registers the Xbox 360 XDK platform with Visual Studio 2022.

.DESCRIPTION
    The Xbox 360 SDK only integrates with the Visual Studio 2010-era MSBuild layout
    (C:\Program Files (x86)\MSBuild\Microsoft.Cpp\v4.0\Platforms\Xbox 360), which
    Visual Studio 2022 does not scan. This script:

      1. Copies the XDK platform folder into VS2022's platform folder
         (<VS2022>\MSBuild\Microsoft\VC\v170\Platforms\Xbox 360)
      2. Installs the new-style bridge files VS2022 requires (Platform.props,
         Platform.targets, Platform.Default.props and PlatformToolsets\2010-01\
         Toolset.props / Toolset.targets) which forward to the XDK files
      3. Patches the copied Microsoft.Cpp.Xbox 360.targets so the XDK build tasks
         (CL/Link/ImageXex/DeployToHardDrive) are registered before VS2022's own
         and run in a 32-bit task host. The XDK task assembly is 32-bit and cannot
         be loaded directly by the 64-bit Visual Studio 2022 process.
      4. Restores build-phase properties (LinkCompiled/TargetExt/OutputType) and
         tool metadata defaults the VS2010-era Microsoft.Cpp.props used to supply
         (handled inside Platform.props)

    The original XDK files are never modified; all changes live in the VS2022 copy.
    The script is idempotent - re-run it if a VS2022 reinstall or update removes
    the platform.

.PARAMETER VsPath
    Path to the Visual Studio 2022 installation root.
    Auto-detected via vswhere if omitted.

.PARAMETER NoPause
    Do not wait for a key press before exiting (for automation).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File vs2022\Install-Xbox360Platform.ps1
#>
param(
    [string]$VsPath,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$logFile = Join-Path $env:TEMP 'Install-Xbox360Platform.log'

function Log($msg) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $msg
    Write-Host $line
    Add-Content -LiteralPath $logFile -Value $line
}

# --- Self-elevate: writing to the VS2022 installation folder requires admin rights ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Administrator rights are required - relaunching elevated (UAC prompt)...'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($VsPath)  { $argList += @('-VsPath', "`"$VsPath`"") }
    if ($NoPause) { $argList += '-NoPause' }
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -Wait -PassThru
    exit $p.ExitCode
}

Remove-Item -LiteralPath $logFile -ErrorAction SilentlyContinue

try {
    # --- Locate VS2022 ---
    if (-not $VsPath) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $VsPath = (& $vswhere -version '[17.0,18.0)' -property installationPath | Select-Object -First 1)
        }
        if (-not $VsPath) { throw 'Could not find a Visual Studio 2022 installation. Pass it manually via -VsPath.' }
    }
    $dest = Join-Path $VsPath 'MSBuild\Microsoft\VC\v170\Platforms\Xbox 360'
    $src  = 'C:\Program Files (x86)\MSBuild\Microsoft.Cpp\v4.0\Platforms\Xbox 360'

    Log "VS2022:     $VsPath"
    Log "XDK source: $src"

    if (-not (Test-Path -LiteralPath $src)) {
        throw 'Xbox 360 XDK platform folder not found. Is the Xbox 360 SDK installed (FULL preset)?'
    }

    # --- 1. Copy the full XDK platform folder (merges/overwrites if re-run) ---
    Copy-Item -LiteralPath $src -Destination (Split-Path $dest -Parent) -Recurse -Force
    Log 'Copied XDK platform folder.'

    # Keep the legacy task and its VC10 dependencies together so MSBuild's
    # out-of-process task host can resolve them without relying on VS2010's GAC.
    $legacyCppRoot = 'C:\Program Files (x86)\MSBuild\Microsoft.Cpp\v4.0'
    $taskDependencies = @(
        (Join-Path $env:XEDK 'bin\win32\Microsoft.Xna.Xbox360.Build.dll'),
        (Join-Path $legacyCppRoot 'Microsoft.Build.CPPTasks.Common.dll'),
        (Join-Path $legacyCppRoot 'Platforms\Win32\Microsoft.Build.CPPTasks.Win32.dll')
    )
    foreach ($dependency in $taskDependencies) {
        if (-not (Test-Path -LiteralPath $dependency)) { throw "Required legacy build task not found: $dependency" }
        Copy-Item -LiteralPath $dependency -Destination $dest -Force
    }
    Log 'Installed legacy XDK task assembly and VC10 dependencies.'

    # --- 2. Patch UsingTask order/hosting in the copied Microsoft.Cpp.Xbox 360.targets ---
    $targetsFile = Join-Path $dest 'Microsoft.Cpp.Xbox 360.targets'
    $content = Get-Content -LiteralPath $targetsFile -Raw
    $content = [regex]::Replace($content, '\s*<!-- VS2022 bridge patch:.*?-->\s*', "`n")
    $m = [regex]::Match($content, '(?s)<UsingTask TaskName="CL".*?<UsingTask TaskName="DeployToHardDrive".*?/>\s*')
    if (-not $m.Success) { throw 'Could not locate UsingTask block in Microsoft.Cpp.Xbox 360.targets' }

    # TaskHostFactory isolates the 32-bit XDK assembly from VS2022's 64-bit
    # in-process MSBuild host. This remains compatible with command-line MSBuild.
    $taskNamespace = 'Microsoft.Xna.Xbox360.Build.Tasks.'
    $block = [regex]::Replace(
        $m.Value,
        '<UsingTask\s+TaskName="([^"]+)"\s+AssemblyFile="([^"]+)"\s*/>',
        ('<UsingTask TaskName="' + $taskNamespace + '$1" AssemblyFile="$(MSBuildThisFileDirectory)Microsoft.Xna.Xbox360.Build.dll" TaskFactory="TaskHostFactory" Architecture="x86" Runtime="CLR4"/>')
    )
    $content = $content.Remove($m.Index, $m.Length)

    # Use the fully qualified task names at call sites. VS2022 registers its own
    # CL and Link tasks before some IDE builds are evaluated; explicit XDK type
    # names avoid that alias collision.
    foreach ($taskName in @('CL', 'Link', 'ImageXex', 'DeployToHardDrive')) {
        $taskBlock = '(?s)<' + $taskName + '(?=\s+(?:Condition|Sources|InputFile))(?<body>.*?)</' + $taskName + '>'
        $replacement = '<' + $taskNamespace + $taskName + '${body}</' + $taskNamespace + $taskName + '>'
        $content = [regex]::Replace($content, $taskBlock, $replacement)
    }
    $content = [regex]::Replace(
        $content,
        '(?s)<ImageXex(?=\s+InputFile)(?<body>.*?)/>',
        ('<' + $taskNamespace + 'ImageXex${body}/>')
    )
    $marker = '<!-- VS2022 bridge patch: register XDK tasks first and host the 32-bit assembly out of process -->'
    $content = [regex]::Replace($content, '(?s)(<Project[^>]*>)', ('$1' + "`n" + $marker + "`n" + $block), 1)
    Set-Content -LiteralPath $targetsFile -Value $content -NoNewline
    Log 'Patched XDK task order and 32-bit hosting in Microsoft.Cpp.Xbox 360.targets.'

    # --- 3. Install the bridge files shipped next to this script ---
    $bridgeFiles = @(
        'Platform.props',
        'Platform.targets',
        'Platform.Default.props',
        'PlatformToolsets\2010-01\Toolset.props',
        'PlatformToolsets\2010-01\Toolset.targets'
    )
    foreach ($rel in $bridgeFiles) {
        $from = Join-Path $PSScriptRoot $rel
        if (-not (Test-Path -LiteralPath $from)) { throw "Bridge file missing next to the script: $rel" }
        Copy-Item -LiteralPath $from -Destination (Join-Path $dest $rel) -Force
    }
    Log 'Installed bridge files.'

    # --- 4. Verify everything VS2022 looks for ---
    $required = @(
        'Platform.props',
        'Platform.targets',
        'Platform.Default.props',
        'PlatformToolsets\2010-01\Toolset.props',
        'PlatformToolsets\2010-01\Toolset.targets',
        'Microsoft.Cpp.Xbox 360.props',
        'Microsoft.Cpp.Xbox 360.targets',
        'Microsoft.Cpp.Xbox 360.default.props',
        'PlatformToolsets\2010-01\Microsoft.Cpp.Xbox 360.2010-01.props',
        'PlatformToolsets\2010-01\Microsoft.Cpp.Xbox 360.2010-01.targets',
        'Microsoft.Xna.Xbox360.Build.dll',
        'Microsoft.Build.CPPTasks.Common.dll',
        'Microsoft.Build.CPPTasks.Win32.dll'
    )
    $missing = $required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $dest $_)) }
    if ($missing) { throw ('Missing files after install: ' + ($missing -join '; ')) }

    Log 'SUCCESS: Xbox 360 platform registered with VS2022.'
    Log 'You can now open hiddriver.sln in Visual Studio 2022 and build.'
    if (-not $NoPause) { [void](Read-Host 'Press Enter to close') }
    exit 0
}
catch {
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Add-Content -LiteralPath $logFile -Value "FAILED: $($_.Exception.Message)"
    if (-not $NoPause) { [void](Read-Host 'Press Enter to close') }
    exit 1
}
