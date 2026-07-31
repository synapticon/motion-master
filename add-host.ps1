<#
.SYNOPSIS
Point this computer's browser at a Motion Master running on another machine (Windows).

.DESCRIPTION
Motion Master's certificate cannot cover a bare IP address — no CA issues those — so a server on
the network is reached under a hostname that encodes its address and that the bundled certificate
does cover:

    192.168.1.50   ->   192-168-1-50.ip.motion-master.synapticon.com

Those names are deliberately absent from public DNS, so this script maps one to its address in the
Windows hosts file. That is enough, and it is not a security compromise: TLS validates a
certificate against the *name*, never against how the name was resolved, so the result is an
ordinary trusted HTTPS connection with no warning to click through.

RUN THIS ON THE MACHINE WITH THE BROWSER, not on the machine running Motion Master. Name
resolution happens at the requesting end — the server never looks up its own name — so an entry on
the server would achieve nothing, and every computer that opens the Console needs its own.

Requires an elevated PowerShell ("Run as administrator"); the hosts file is not writable otherwise.

.PARAMETER Address
IPv4 address of the machine running Motion Master.

.PARAMETER Remove
Remove the entry for that address instead of adding it.

.EXAMPLE
.\add-host.ps1 192.168.1.50

.EXAMPLE
.\add-host.ps1 192.168.1.50 -Remove
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Address,
    [switch] $Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Domain = 'ip.motion-master.synapticon.com'
$Marker = '# motion-master'
$HttpPort = 61447
# $env:HOSTS_FILE lets the script be exercised against a scratch file instead of the real one.
$HostsFile = if ($env:HOSTS_FILE) { $env:HOSTS_FILE }
             else { Join-Path $env:SystemRoot 'System32\drivers\etc\hosts' }

# [ipaddress] parses far more than a dotted quad — "1.2" and bare integers included — so check the
# shape explicitly. A typo should be an error here, not a nonsense entry that fails confusingly
# later.
function Test-Ipv4 {
    param([string] $Value)
    $octets = $Value.Split('.')
    if ($octets.Count -ne 4) {
        return $false
    }
    foreach ($octet in $octets) {
        if ($octet -notmatch '^\d{1,3}$' -or [int] $octet -gt 255) {
            return $false
        }
    }
    return $true
}

if (-not (Test-Ipv4 $Address)) {
    Write-Error "'$Address' is not an IPv4 address"
    exit 1
}

$hostname = ($Address -replace '\.', '-') + '.' + $Domain
$line = "$Address $hostname $Marker"

if (-not (Test-Path -LiteralPath $HostsFile)) {
    Write-Error "$HostsFile does not exist"
    exit 1
}

# Read as a plain array of lines. Windows hosts files are ASCII/UTF-8 without a BOM; writing one
# back with a BOM makes the resolver ignore the first entry, so every write below is BOM-free
# ASCII rather than PowerShell 5's default UTF-16 or UTF-8-with-BOM.
$lines = @(Get-Content -LiteralPath $HostsFile)
$existing = @($lines | Where-Object { $_ -like "* $hostname $Marker" })

function Save-Hosts {
    param([string[]] $Content)
    try {
        [System.IO.File]::WriteAllLines($HostsFile, $Content, [System.Text.UTF8Encoding]::new($false))
    } catch [System.UnauthorizedAccessException] {
        Write-Error "$HostsFile is not writable — re-run PowerShell as administrator"
        exit 1
    }
}

if ($Remove) {
    if ($existing.Count -eq 0) {
        Write-Host "No entry for $hostname in $HostsFile — nothing to remove."
        exit 0
    }
    Save-Hosts @($lines | Where-Object { $_ -notlike "* $hostname $Marker" })
    Write-Host "Removed $hostname from $HostsFile"
    exit 0
}

if ($existing.Count -eq 1 -and $existing[0] -eq $line) {
    Write-Host "$HostsFile already maps $hostname to $Address"
} else {
    # A stale mapping for this same name must go first: the resolver takes the first match in the
    # file, so an old line would silently win over the one appended below.
    $kept = @($lines | Where-Object { $_ -notlike "* $hostname $Marker" })
    Copy-Item -LiteralPath $HostsFile -Destination "$HostsFile.motion-master.bak" -Force
    Save-Hosts ($kept + $line)
    Write-Host "Added to $HostsFile (previous copy saved as $HostsFile.motion-master.bak):"
    Write-Host "  $line"
}

Write-Host ''
Write-Host "Open https://${hostname}:$HttpPort — or set the Console's Host field to:"
Write-Host "  $hostname"
Write-Host ''
Write-Host 'The server must be running with server.bindAddress set to "0.0.0.0".'
