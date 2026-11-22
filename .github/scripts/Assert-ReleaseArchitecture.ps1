<#
.SYNOPSIS
    Verifies a staged HydraHook release directory for one architecture.

.DESCRIPTION
    Release assets are architecture-specific archives. This script checks that
    the directory contains exactly the expected binaries and font, that every
    DLL is the PE machine type claimed by -Architecture, and, when
    -RequireSigned is set, that every DLL has a valid Authenticode signature.
#>

Set-StrictMode -Version Latest

$script:ReleaseArchitectures = @{
    'x86' = 0x014C # IMAGE_FILE_MACHINE_I386
    'x64' = 0x8664 # IMAGE_FILE_MACHINE_AMD64
}

$script:ExpectedFiles = @(
    'HydraHook.dll',
    'HydraHook-ImGui.dll',
    'HydraHook-FPS.dll',
    'HydraHook-DirectXTK.dll',
    'Arial.spritefont',
    'HydraHook-OpenCV.dll'
)

$script:MachineNames = @{
    0x014C = 'i386 (32-bit)'
    0x01C4 = 'ARM Thumb-2 (32-bit)'
    0x0200 = 'Itanium'
    0x8664 = 'AMD64'
    0xAA64 = 'ARM64'
}

function Get-PeMachineType
{
    [OutputType([int])]
    param(
        [Parameter(Mandatory)]
        [string] $Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    try
    {
        $reader = [System.IO.BinaryReader]::new($stream)

        if ($reader.ReadUInt16() -ne 0x5A4D)
        {
            throw "$Path is not a PE image (missing MZ signature)."
        }

        $stream.Position = 0x3C
        $stream.Position = $reader.ReadUInt32()

        if ($reader.ReadUInt32() -ne 0x00004550)
        {
            throw "$Path is not a PE image (missing PE\0\0 signature)."
        }

        return [int] $reader.ReadUInt16()
    }
    finally
    {
        $stream.Dispose()
    }
}

function Assert-ReleaseArchitecture
{
    param(
        [Parameter(Mandatory)]
        [string] $Path,

        [Parameter(Mandatory)]
        [ValidateSet('x86', 'x64')]
        [string] $Architecture,

        [switch] $RequireSigned
    )

    if (-not (Test-Path -LiteralPath $Path))
    {
        throw "Release payload directory $Path does not exist."
    }

    $entries = @(Get-ChildItem -LiteralPath $Path -Force)
    $directories = @($entries | Where-Object { $_.PSIsContainer })
    if ($directories.Count -gt 0)
    {
        $directoryNames = @($directories | Select-Object -ExpandProperty Name)
        throw "Release payload $Architecture contains directories: $($directoryNames -join ', ')."
    }

    $files = @($entries | Where-Object { -not $_.PSIsContainer })
    $names = @($files | Select-Object -ExpandProperty Name)

    $unexpected = @($names | Where-Object { $_ -notin $script:ExpectedFiles })
    if ($unexpected.Count -gt 0)
    {
        throw "Unexpected files in ${Architecture}: $($unexpected -join ', '). Allowed: $($script:ExpectedFiles -join ', ')."
    }

    $missing = @($script:ExpectedFiles | Where-Object { $_ -notin $names })
    if ($missing.Count -gt 0)
    {
        throw "Architecture $Architecture is missing: $($missing -join ', ')."
    }

    $expectedMachine = $script:ReleaseArchitectures[$Architecture]
    foreach ($file in $files)
    {
        if ($file.Extension -ne '.dll')
        {
            continue
        }

        $machine = Get-PeMachineType -Path $file.FullName
        if ($machine -ne $expectedMachine)
        {
            $actual = if ($script:MachineNames.ContainsKey($machine)) { $script:MachineNames[$machine] } else { '0x{0:X4}' -f $machine }
            throw "$($file.FullName) is a $actual image but sits in the $Architecture payload."
        }

        if ($RequireSigned)
        {
            $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
            if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate)
            {
                throw "$($file.FullName) is not validly signed (Status=$($signature.Status))."
            }
        }

        Write-Output "OK: $($file.FullName) ($Architecture)"
    }
}
