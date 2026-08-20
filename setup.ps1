<#
.SYNOPSIS
Download the auto-tuning executable that Motion Master starts as a child process (Windows).

.DESCRIPTION
The auto-tuning executable is about 65 MB. The Motion Master binary is about 5 MB. Motion
Master ships on every tag, and auto-tuning changes a few times a year. A copy in every zip
would therefore make each download many times larger, for a file that rarely changes. So the
file lives in one rolling release, at a URL that does not change, and every install path
downloads it from there.

Motion Master runs without auto-tuning. A failed download is therefore not an error. This
script reports what happened, and the server still starts.

There is nothing else to set up on Windows. The binary needs no file capabilities, unlike the
Linux build. What it may do comes from the account that runs it.

Run this script from the directory you extracted, as yourself. It needs no elevation.

.EXAMPLE
.\setup.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$target = Join-Path $dir 'auto-tuning.exe'

# One asset for every Windows machine. There is no build for ARM64 Windows, because one
# mandatory numerical dependency of auto-tuning has never shipped a wheel for it. ARM64 Windows
# runs this x64 executable under the emulation that the operating system provides.
$url = 'https://github.com/synapticon/motion-master/releases/download/auto-tuning/standalone-autotuning-windows-x86_64.exe'

# Keep a file that is already there. Motion Master is released often, and auto-tuning is not. A
# download of 65 MB on every upgrade is the cost this whole arrangement removes.
if (Test-Path $target) {
    Write-Output "Auto-tuning is already installed: $(& $target --version)"
    Write-Output "To replace it, delete $target and run this script again."
    exit 0
}

# Download to a temporary name, then move the file. A partial download must never look like an
# installed executable.
$partial = "$target.part"
try {
    Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing
    Move-Item -Force -Path $partial -Destination $target
    # Run the executable once. This proves that the file works, and it prints the version. It
    # takes about one second, because the executable unpacks itself at every start.
    Write-Output "Installed auto-tuning $(& $target --version) in $dir"
} catch {
    Remove-Item -Force -ErrorAction SilentlyContinue -Path $partial
    Write-Output "Could not download the auto-tuning executable: $($_.Exception.Message)"
    Write-Output "Motion Master runs without auto-tuning. Run .\setup.ps1 again when this machine is online."
}
