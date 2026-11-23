<#
.SYNOPSIS
    Maps a Git tag ref to the four-part numeric version stamped into resources.

.DESCRIPTION
    Windows VERSIONINFO components are 16-bit integers, so the tag cannot be
    passed through as-is. Stable tags vX.Y.Z become X.Y.Z.0. Prerelease tags
    vX.Y.Z-preNNN become X.Y.Z.NNN after converting NNN to an integer
    (pre001 -> 1). Every returned component is rejected unless it fits in
    0..65535; the prerelease sequence must also be at least 1.
#>

Set-StrictMode -Version Latest

function ConvertTo-VersionComponent
{
    [CmdletBinding()]
    [OutputType([int64])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [int64]$Minimum = 0,

        [int64]$Maximum = 65535
    )

    try
    {
        $parsed = [int64]$Value
    }
    catch
    {
        throw "$Name component $Value is outside the valid Windows version-component range ${Minimum}..${Maximum}."
    }

    if ($parsed -lt $Minimum -or $parsed -gt $Maximum)
    {
        throw "$Name component $Value is outside the valid Windows version-component range ${Minimum}..${Maximum}."
    }

    return $parsed
}

function Get-ReleaseBuildVersion
{
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Ref
    )

    if ($Ref -notmatch '^refs/tags/(?<tag>.+)$')
    {
        throw "Ref '$Ref' is not a tag ref."
    }

    $tag = $Matches['tag']

    if ($tag -match '^v(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)$')
    {
        $major = ConvertTo-VersionComponent -Value $Matches['major'] -Name 'Major'
        $minor = ConvertTo-VersionComponent -Value $Matches['minor'] -Name 'Minor'
        $patch = ConvertTo-VersionComponent -Value $Matches['patch'] -Name 'Patch'
        return "$major.$minor.$patch.0"
    }

    if ($tag -match '^v(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)-pre(?<pre>\d+)$')
    {
        $major = ConvertTo-VersionComponent -Value $Matches['major'] -Name 'Major'
        $minor = ConvertTo-VersionComponent -Value $Matches['minor'] -Name 'Minor'
        $patch = ConvertTo-VersionComponent -Value $Matches['patch'] -Name 'Patch'
        $pre = ConvertTo-VersionComponent -Value $Matches['pre'] -Name 'Prerelease sequence' -Minimum 1
        return "$major.$minor.$patch.$pre"
    }

    throw "Unsupported release tag '$tag'. Expected vX.Y.Z or vX.Y.Z-preNNN."
}
