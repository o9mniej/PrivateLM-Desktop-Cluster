param(
    [int]$Port = 50052,
    [int]$Threads = 0,
    [string]$Name = $env:COMPUTERNAME
)

$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Here "build"
$Exe = Join-Path $BuildDir "bin\privatelm-worker.exe"
$Python = Get-Command python -ErrorAction SilentlyContinue

if (-not $Python) {
    throw "Python 3 was not found. Install Python 3 and reopen PowerShell."
}
if (-not (Test-Path $Exe)) {
    throw "Worker executable not found at $Exe. Build it first; see README.md."
}

$ArgsList = @((Join-Path $Here "worker.py"), "--rpc-exe", $Exe, "--port", "$Port", "--name", $Name)
if ($Threads -gt 0) {
    $ArgsList += @("--threads", "$Threads")
}
& $Python.Source @ArgsList
