<#
.SYNOPSIS
Prepare a PowerShell session to build Motion Master on Windows.

.DESCRIPTION
Visual Studio keeps the C++ compiler, CMake, and Ninja out of the system PATH. A plain
`cmake --preset x64-windows-debug` therefore fails in a normal shell. This script finds the
Visual Studio installation, imports the MSVC x64 build environment from `vcvars64.bat`, and
puts the CMake and Ninja that ship with Visual Studio on PATH.

Run the script in the session you build from. It changes the environment of the process, so
a plain call is enough. You can dot-source it as well. One call serves the whole session.

The first call takes about four seconds, because vcvars64.bat is slow. It records every
change it made, and each later call replays that record in a few milliseconds. The record
holds the write time of the MSVC toolset directory and of the Windows SDK include directory,
so an update of either one builds a fresh record.

Use -Persist to call the script from the PowerShell profile of the current user. Every new
session then starts ready to build, and pays only the replay.

.PARAMETER Persist
Add a call to this script to the PowerShell profile of the current user.

.PARAMETER Remove
Delete that call from the PowerShell profile of the current user. The current session keeps
the environment it already has.

.PARAMETER Refresh
Ignore the record and read the environment from Visual Studio again. Use this after you
install a CMake or a Ninja of your own, because the record cannot see that.

.PARAMETER Quiet
Print warnings and errors only. The profile block uses this switch.

.EXAMPLE
.\tools\windows-env.ps1
cmake --preset x64-windows-debug
cmake --build --preset x64-windows-debug

.EXAMPLE
.\tools\windows-env.ps1 -Persist
#>
[CmdletBinding()]
param(
    [switch]$Persist,
    [switch]$Remove,
    [switch]$Refresh,
    [switch]$Quiet
)

# The whole script runs inside this function for two reasons. A session that dot-sources the
# file gets no stray variable and no changed $ErrorActionPreference. And the function can use
# `return`, which a dot-sourced script cannot do with `exit`: `exit` closes the window of the
# reader who dot-sources the file. Environment changes still reach the session, because the
# process owns the environment and a PowerShell scope cannot hide it.
function Initialize-MotionMasterBuildEnvironment {
    [CmdletBinding()]
    param([switch]$Persist, [switch]$Remove, [switch]$Refresh, [switch]$Quiet)

    $ErrorActionPreference = 'Stop'

    function Write-Report([string]$text) {
        if (-not $Quiet) { Write-Host $text }
    }

    # CMakeLists.txt sets cmake_minimum_required(VERSION 4.0). Visual Studio 2022 ships CMake
    # 3.x in its older updates, and that version cannot configure this project.
    $minimumCMakeMajor = 4

    # Raise this number after a change to the shape of the record. An old file then fails the
    # check and the script writes a new one, instead of reading fields that moved.
    $recordSchema = 1

    $scriptPath = $PSCommandPath
    $repoRoot = Split-Path -Parent (Split-Path -Parent $scriptPath)

    # LOCALAPPDATA holds the per-machine state of one user. The record names one Visual Studio
    # installation on this machine, so a roaming location would carry it to a machine where it
    # is wrong.
    $recordPath = Join-Path $env:LOCALAPPDATA 'motion-master\windows-env.json'

    $profilePath = $PROFILE.CurrentUserAllHosts
    $beginMark = '# >>> motion-master build environment >>>'
    $endMark = '# <<< motion-master build environment <<<'

    # ---------------------------------------------------------------------------------
    # -Remove: take the block out of the profile and stop.
    # ---------------------------------------------------------------------------------

    if ($Remove) {
        if (-not (Test-Path -LiteralPath $profilePath)) {
            Write-Report "The profile does not exist, so there is nothing to remove: $profilePath"
            return
        }
        # Get-Content -Raw answers $null for an empty file, and the regex below rejects $null.
        $text = (Get-Content -LiteralPath $profilePath -Raw)
        if (-not $text) { $text = '' }
        $pattern = [regex]::Escape($beginMark) + '.*?' + [regex]::Escape($endMark) + '\r?\n?'
        $stripped = [regex]::Replace($text, $pattern, '', 'Singleline')
        if ($stripped -eq $text) {
            Write-Report "The profile holds no block of this script: $profilePath"
            return
        }
        if ($stripped.Trim().Length -eq 0) {
            Remove-Item -LiteralPath $profilePath
            Write-Report "Removed the block and deleted the empty profile: $profilePath"
        } else {
            Set-Content -LiteralPath $profilePath -Value $stripped -NoNewline
            Write-Report "Removed the block from $profilePath"
        }
        Write-Report 'Open a new session to get a shell without the build environment.'
        return
    }

    # ---------------------------------------------------------------------------------
    # Helpers.
    # ---------------------------------------------------------------------------------

    # Answer the write time of a file or a directory, or 0 when it is absent. The write time of
    # a directory changes when an entry appears in it or disappears from it. An update of
    # Visual Studio adds a directory for the new toolset, and an update of the Windows SDK adds
    # one for the new SDK, so these numbers catch both. Each check is one metadata read, which
    # is what makes the record safe to trust at every shell start.
    function Get-Stamp([string]$path) {
        if (-not $path) { return 0 }
        $item = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
        if (-not $item) { return 0 }
        return $item.LastWriteTimeUtc.Ticks
    }

    function Get-EnvironmentSnapshot {
        $snapshot = @{}
        foreach ($item in Get-ChildItem env:) { $snapshot[$item.Name] = $item.Value }
        return $snapshot
    }

    function Add-PathEntry([string]$directory) {
        if (-not (Test-Path -LiteralPath $directory)) { return }
        foreach ($entry in ($env:PATH -split ';' | Where-Object { $_ })) {
            if ($entry.TrimEnd('\') -ieq $directory.TrimEnd('\')) { return }
        }
        $env:PATH = $directory + ';' + $env:PATH
    }

    function Get-ToolMajorVersion([string]$name) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue |
                   Select-Object -First 1
        if (-not $command) { return -1 }
        $line = & $command.Source --version 2>$null | Select-Object -First 1
        if ($line -match '(\d+)\.(\d+)') { return [int]$Matches[1] }
        return -1
    }

    # Split a search path into its directories, with the empty pieces dropped.
    function Split-SearchPath([string]$value) {
        if (-not $value) { return @() }
        return @($value -split ';' | Where-Object { $_ })
    }

    # Apply one recorded change to the environment of this process. Applying a change twice
    # must leave the same result as applying it once, and no change may drop a value that the
    # user put there.
    #
    # `pathAdd` carries the directories to put in front of PATH. `prepend` carries a piece of
    # text to put in front of the value. `set` owns the whole value.
    function Set-EnvironmentChange([string]$name, [string]$action, [string]$value) {
        $current = [Environment]::GetEnvironmentVariable($name, 'Process')
        if ($action -eq 'pathAdd') {
            $present = @{}
            foreach ($entry in (Split-SearchPath $current)) { $present[$entry.TrimEnd('\')] = $true }
            $missing = @()
            foreach ($entry in (Split-SearchPath $value)) {
                if (-not $present.ContainsKey($entry.TrimEnd('\'))) { $missing += $entry }
            }
            if ($missing.Count -eq 0) { return }
            $head = ($missing -join ';') + ';'
            Set-Item -LiteralPath "env:$name" -Value ($head + $current)
        } elseif ($action -eq 'prepend') {
            if ($current -and $current.StartsWith($value, 'OrdinalIgnoreCase')) { return }
            Set-Item -LiteralPath "env:$name" -Value ($value + $current)
        } elseif ($current -cne $value) {
            Set-Item -LiteralPath "env:$name" -Value $value
        }
    }

    # ---------------------------------------------------------------------------------
    # Replay the record when it still describes this machine.
    # ---------------------------------------------------------------------------------

    $record = $null
    if (-not $Refresh -and (Test-Path -LiteralPath $recordPath)) {
        try {
            $candidate = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
            $stale = $candidate.schema -ne $recordSchema -or
                     -not $candidate.vsPath -or
                     -not (Test-Path -LiteralPath $candidate.vsPath) -or
                     (Get-Stamp $candidate.toolsetDir) -ne $candidate.toolsetStamp -or
                     (Get-Stamp $candidate.sdkDir) -ne $candidate.sdkStamp -or
                     (Get-Stamp $scriptPath) -ne $candidate.scriptStamp
            if (-not $stale) { $record = $candidate }
        } catch {
            # A truncated file or a hand-edited one is not an error. Build the environment again.
            $record = $null
        }
    }

    if ($record) {
        foreach ($change in $record.changes) {
            Set-EnvironmentChange $change.name $change.action $change.value
        }
        $vsPath = $record.vsPath
        Write-Report "Applied the recorded MSVC x64 environment of $vsPath"
    } else {

        # -----------------------------------------------------------------------------
        # Find Visual Studio.
        # -----------------------------------------------------------------------------

        # vswhere.exe sits at a fixed path. Microsoft documents that path, and every Visual
        # Studio installer since 2017 writes the file there. So the search needs no registry
        # read.
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) {
            throw "Visual Studio is not installed: $vswhere is missing. Install Visual Studio with the workload 'Desktop development with C++'."
        }

        # Ask for an installation that carries the x64 compiler. An installation that holds
        # only the C# tools answers an unfiltered query, and vcvars64.bat is absent from it.
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1

        if (-not $vsPath) {
            throw "No Visual Studio installation carries the x64 C++ tools. Open the Visual Studio Installer and add the workload 'Desktop development with C++'."
        }

        $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
        if (-not (Test-Path -LiteralPath $vcvars)) {
            throw "The x64 compiler environment script is missing: $vcvars"
        }

        # -----------------------------------------------------------------------------
        # Import the MSVC x64 environment.
        # -----------------------------------------------------------------------------

        # A Developer PowerShell window starts with the MSVC environment already in it, and so
        # does a session that ran this script before the record went stale. Two things follow.
        # A second import would add the same directories to PATH, INCLUDE and LIB again. And a
        # record taken here would hold almost nothing, because this session and the imported
        # one differ in almost nothing — a replay of that record in a clean shell would then
        # set up no compiler at all, and report success. So take the session as it stands, and
        # write no record.
        $alreadyConfigured = $env:VSINSTALLDIR -and
                             ($env:VSINSTALLDIR.TrimEnd('\') -eq $vsPath.TrimEnd('\'))

        $before = $null
        if ($alreadyConfigured) {
            Write-Report "The MSVC x64 environment is already in this session: $vsPath"
        } else {
            Write-Report "Reading the MSVC x64 environment from $vsPath"

            # Take the snapshot before anything changes. The difference against the snapshot at
            # the end becomes the record, so the record covers the CMake and Ninja steps below
            # as well as this import.
            $before = Get-EnvironmentSnapshot

            # vcvars64.bat sets its variables in the cmd.exe that runs it. Ask that cmd.exe to
            # print the result, then copy each line into this process. Discard the banner. The
            # `&&` runs `set` only after a successful import, so a failed import produces no
            # output, and the check below catches it.
            & "$env:ComSpec" /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
                if ($_ -match '^([^=]+)=(.*)$') {
                    Set-Item -LiteralPath "env:$($Matches[1])" -Value $Matches[2]
                }
            }
            if (-not $env:VSINSTALLDIR) {
                throw "vcvars64.bat produced no environment. Run it in a cmd.exe window to read its error: $vcvars"
            }
        }

        # -----------------------------------------------------------------------------
        # Put CMake and Ninja on PATH.
        # -----------------------------------------------------------------------------

        $vsTools = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'

        # Prefer a CMake that the developer installed and put on PATH. Fall back to the copy
        # inside Visual Studio. Either one must be new enough for this project.
        if ((Get-ToolMajorVersion 'cmake') -lt $minimumCMakeMajor) {
            Add-PathEntry (Join-Path $vsTools 'CMake\bin')
        }
        $cmakeMajor = Get-ToolMajorVersion 'cmake'
        if ($cmakeMajor -lt 0) {
            throw "CMake is not installed. Add the component 'C++ CMake tools for Windows' in the Visual Studio Installer, or install CMake $minimumCMakeMajor.0 or newer from cmake.org."
        }
        if ($cmakeMajor -lt $minimumCMakeMajor) {
            throw "CMake $cmakeMajor.x is too old. This project needs CMake $minimumCMakeMajor.0 or newer. Update Visual Studio, or install CMake from cmake.org and put it on PATH."
        }

        if (-not (Get-Command ninja -CommandType Application -ErrorAction SilentlyContinue)) {
            Add-PathEntry (Join-Path $vsTools 'Ninja')
        }
        if (-not (Get-Command ninja -CommandType Application -ErrorAction SilentlyContinue)) {
            throw "Ninja is not installed. Add the component 'C++ CMake tools for Windows' in the Visual Studio Installer, or install Ninja and put it on PATH."
        }

        # -----------------------------------------------------------------------------
        # Write the record.
        # -----------------------------------------------------------------------------

        if ($alreadyConfigured) {
            Write-Report 'Wrote no record, because this session already carried the environment.'
            Write-Report 'Run the script in a plain PowerShell window to write one.'
        } else {
            $after = Get-EnvironmentSnapshot
            $changes = @()
            foreach ($name in $after.Keys) {
                $new = $after[$name]
                $old = $before[$name]
                if ($name -ieq 'PATH') {
                    # vcvars64.bat rewrites PATH. It does not only grow the front: it also
                    # drops the duplicates it finds and it reorders what is left. So the
                    # difference of the two texts is useless here, and a recorded whole PATH
                    # would delete every directory the user adds later. Record the directories
                    # that appeared, and replay them in front of whatever PATH holds then.
                    $had = @{}
                    foreach ($entry in (Split-SearchPath $old)) {
                        $had[$entry.TrimEnd('\')] = $true
                    }
                    $added = @()
                    foreach ($entry in (Split-SearchPath $new)) {
                        if (-not $had.ContainsKey($entry.TrimEnd('\'))) { $added += $entry }
                    }
                    if ($added.Count -gt 0) {
                        $changes += [ordered]@{
                            name = $name; action = 'pathAdd'; value = ($added -join ';')
                        }
                    }
                } elseif ($null -eq $old) {
                    $changes += [ordered]@{ name = $name; action = 'set'; value = $new }
                } elseif ($new -ceq $old) {
                    continue
                } elseif ($new.EndsWith($old, 'Ordinal')) {
                    # INCLUDE, LIB and LIBPATH grow at the front. Record only that front, so a
                    # value the user adds later survives the replay.
                    $changes += [ordered]@{
                        name = $name; action = 'prepend'
                        value = $new.Substring(0, $new.Length - $old.Length)
                    }
                } else {
                    $changes += [ordered]@{ name = $name; action = 'set'; value = $new }
                }
            }

            $toolsetDir = Join-Path $vsPath 'VC\Tools\MSVC'
            $sdkDir = ''
            if ($env:WindowsSdkDir) { $sdkDir = Join-Path $env:WindowsSdkDir 'Include' }

            $document = [ordered]@{
                schema = $recordSchema
                vsPath = $vsPath
                toolsetDir = $toolsetDir
                toolsetStamp = Get-Stamp $toolsetDir
                sdkDir = $sdkDir
                sdkStamp = Get-Stamp $sdkDir
                # An edit of this script may change what it puts in the environment, so the
                # record must not outlive the version that wrote it.
                scriptStamp = Get-Stamp $scriptPath
                changes = $changes
            }

            $recordDirectory = Split-Path -Parent $recordPath
            if (-not (Test-Path -LiteralPath $recordDirectory)) {
                New-Item -ItemType Directory -Path $recordDirectory -Force | Out-Null
            }
            $document | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recordPath
            Write-Report "Recorded it in $recordPath"
        }
    }

    # ---------------------------------------------------------------------------------
    # Check the vcpkg submodule.
    # ---------------------------------------------------------------------------------

    # The presets name extern/vcpkg as the CMake toolchain file. An empty submodule fails the
    # configure with a message about a missing toolchain file, which hides the true cause.
    $toolchain = Join-Path $repoRoot 'extern\vcpkg\scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $toolchain)) {
        Write-Warning 'The vcpkg submodule is empty. Run: git submodule update --init --recursive'
    }

    # ---------------------------------------------------------------------------------
    # -Persist: call this script from the profile of the current user.
    # ---------------------------------------------------------------------------------

    if ($Persist) {
        # $PROFILE.CurrentUserAllHosts covers the console and every other host of this
        # PowerShell edition. Windows PowerShell 5.1 and PowerShell 7 keep separate profiles,
        # so the block reaches the edition that runs this script.
        $block = @"
$beginMark
# Added by tools/windows-env.ps1 of Motion Master. Run that script with -Remove to delete this
# block. The test keeps the profile silent after a move or a delete of the repository.
`$mmBuildEnv = '$scriptPath'
if (Test-Path -LiteralPath `$mmBuildEnv) { & `$mmBuildEnv -Quiet }
Remove-Variable mmBuildEnv
$endMark
"@

        $directory = Split-Path -Parent $profilePath
        if (-not (Test-Path -LiteralPath $directory)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }

        # Get-Content -Raw answers $null for an empty file, and the regex below rejects $null.
        $text = ''
        if (Test-Path -LiteralPath $profilePath) {
            $text = (Get-Content -LiteralPath $profilePath -Raw)
            if (-not $text) { $text = '' }
        }

        $pattern = [regex]::Escape($beginMark) + '.*?' + [regex]::Escape($endMark)
        if ([regex]::IsMatch($text, $pattern, 'Singleline')) {
            # Replace the block that is there. The path of the repository may have changed.
            $text = [regex]::Replace($text, $pattern, $block.Trim(), 'Singleline')
            Set-Content -LiteralPath $profilePath -Value $text -NoNewline
            Write-Report "Updated the block in $profilePath"
        } else {
            if ($text -and -not $text.EndsWith("`n")) { $text += [Environment]::NewLine }
            if ($text) { $text += [Environment]::NewLine }
            Set-Content -LiteralPath $profilePath -Value ($text + $block) -NoNewline
            Write-Report "Added the block to $profilePath"
        }
        Write-Report 'Every new PowerShell session now starts ready to build.'
    }

    # ---------------------------------------------------------------------------------
    # Report.
    # ---------------------------------------------------------------------------------

    if (-not $Quiet) {
        # Read the compiler version from the environment, not from a run of cl.exe. cl.exe
        # writes its banner and its usage text to two different streams, and the two arrive in
        # either order. A report built from that output names the wrong line about half the
        # time.
        Write-Report ''
        Write-Report "  msvc  : $env:VCToolsVersion [$((Get-Command cl).Source)]"
        Write-Report "  cmake : $(& cmake --version | Select-Object -First 1) [$((Get-Command cmake).Source)]"
        Write-Report "  ninja : ninja $(& ninja --version) [$((Get-Command ninja).Source)]"
        Write-Report ''
        Write-Report '  cmake --preset x64-windows-debug'
        Write-Report '  cmake --build --preset x64-windows-debug'
        Write-Report '  ctest --test-dir build/x64-windows-debug --output-on-failure'
        Write-Report ''
    }
}

Initialize-MotionMasterBuildEnvironment -Persist:$Persist -Remove:$Remove -Refresh:$Refresh -Quiet:$Quiet
