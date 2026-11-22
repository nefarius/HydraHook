# Downloads vcpkg.exe once. MSBuild evaluates the pre-build event in every
# project before any of them run, so a parallel build would otherwise start
# several downloads of the same file.
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vcpkgRoot = Join-Path $repoRoot 'vcpkg'
$exe = Join-Path $vcpkgRoot 'vcpkg.exe'
$bootstrap = Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat'

if (-not (Test-Path -LiteralPath $bootstrap)) {
    Write-Error "bootstrap-vcpkg.bat was not found at $bootstrap"
}

$mutex = New-Object System.Threading.Mutex($false, 'Local\HydraHookVcpkgBootstrap')
$acquired = $false
try {
    try {
        $acquired = $mutex.WaitOne()
    }
    catch [System.Threading.AbandonedMutexException] {
        $acquired = $true
    }

    if (Test-Path -LiteralPath $exe) {
        exit 0
    }

    & $bootstrap
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Error "bootstrap-vcpkg.bat exited 0 without writing $exe"
    }
}
finally {
    if ($acquired) {
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
}
