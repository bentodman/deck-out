[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

function Find-ISCC {
    $candidates = @(
        (Get-Command iscc -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue)
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
        "${env:LocalAppData}\Programs\Inno Setup 6\ISCC.exe"
    )

    foreach ($path in $candidates) {
        if ($path -and (Test-Path -Path $path)) {
            return (Resolve-Path $path).Path
        }
    }

    return $null
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"
    $PluginDir = "${ProjectRoot}/release/${Configuration}/${ProductName}"

    if ( ! ( Test-Path -Path $PluginDir ) ) {
        throw "Installed plugin files not found at ${PluginDir}. Run cmake --install first."
    }

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*-Installer.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Join-Path "${ProjectRoot}/release/${Configuration}" $ProductName)
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Force = $true
    }
    # Compress-Archive includes the folder name, so extract into plugins/ yields plugins/deck-out/...
    Compress-Archive @CompressArgs
    Log-Group

    $Iscc = Find-ISCC
    if ( ! $Iscc ) {
        throw "Inno Setup (iscc) not found. Install with: choco install innosetup -y"
    }

    $IsccFile = "${ProjectRoot}/build_${Target}/installer-Windows.generated.iss"
    if ( ! ( Test-Path -Path $IsccFile ) ) {
        throw 'InnoSetup install script not found. Re-run cmake configure, then cmake --install.'
    }

    Log-Group "Creating InnoSetup installer..."
    Push-Location -Stack BuildTemp
    Ensure-Location -Path "${ProjectRoot}/release"
    if (Test-Path Package) {
        Remove-Item -Path Package -Recurse -Force
    }
    Copy-Item -Path ${Configuration} -Destination Package -Recurse
    Invoke-External $Iscc ${IsccFile} /O"${ProjectRoot}/release" /F"${OutputName}-Installer"
    Remove-Item -Path Package -Recurse -Force
    Pop-Location -Stack BuildTemp
    Log-Group

    Log-Information "Created ${ProjectRoot}/release/${OutputName}.zip"
    Log-Information "Created ${ProjectRoot}/release/${OutputName}-Installer.exe"
}

Package
