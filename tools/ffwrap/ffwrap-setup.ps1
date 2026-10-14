# ffwrap-setup.ps1 -- installs or removes ffwrap for Jellyfin on Windows.
# Run it through install-ffwrap.bat / uninstall-ffwrap.bat (they ask for
# administrator rights and keep the window open).  See README.txt.
#
# Testing without touching a live server: set FFWRAP_TEST=1 and point
# FFWRAP_CONFIG_DIR / FFWRAP_TARGET_DIR at scratch copies.  Test mode skips the
# administrator check and leaves Jellyfin running.

param([ValidateSet('install', 'uninstall')][string]$Mode = 'install')

$ErrorActionPreference = 'Stop'
$Test = $env:FFWRAP_TEST -eq '1'

function Say([string]$m) { Write-Host $m }
function Step([string]$m) { Write-Host ''; Write-Host "== $m" -ForegroundColor Cyan }

# What this run stopped, so every way out -- success or failure -- starts it again.
$script:stoppedService = $false
$script:stoppedTray    = $false
$script:jfDir          = Join-Path $env:ProgramFiles 'Jellyfin\Server'
function Restart-Jellyfin {
    try {
        if ($script:stoppedService) {
            Step 'Starting Jellyfin'
            Start-Service -Name 'JellyfinServer'
            $script:stoppedService = $false
            Say 'Jellyfin service started.'
        } elseif ($script:stoppedTray) {
            Step 'Starting Jellyfin'
            $script:stoppedTray = $false
            $tray = Join-Path $script:jfDir 'jellyfin-windows-tray\Jellyfin.Windows.Tray.exe'
            if (Test-Path -LiteralPath $tray) {
                # Through Explorer, so Jellyfin runs as you and not as administrator.
                Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$tray`""
                Say 'Jellyfin is starting again.'
            } else {
                Say 'Start Jellyfin again from the Start menu.'
            }
        }
    } catch {
        Say ("Could not start Jellyfin again (" + $_.Exception.Message + "). Start it from the Start menu.")
    }
}

function Finish([bool]$ok, [string]$m) {
    Restart-Jellyfin
    Write-Host ''
    if ($ok) {
        Write-Host '=============================================' -ForegroundColor Green
        Write-Host " SUCCESS: $m" -ForegroundColor Green
        Write-Host '=============================================' -ForegroundColor Green
        exit 0
    }
    Write-Host '=============================================' -ForegroundColor Red
    Write-Host " FAILED: $m" -ForegroundColor Red
    Write-Host '=============================================' -ForegroundColor Red
    exit 1
}

try {
    $configDir = if ($env:FFWRAP_CONFIG_DIR) { $env:FFWRAP_CONFIG_DIR } else { Join-Path $env:ProgramData 'Jellyfin\Server\config' }
    $targetDir = if ($env:FFWRAP_TARGET_DIR) { $env:FFWRAP_TARGET_DIR } else { Join-Path $env:ProgramData 'Jellyfin\ffwrap' }
    $encoding  = Join-Path $configDir 'encoding.xml'
    $wrapper   = Join-Path $targetDir 'ffmpeg.exe'
    $ini       = Join-Path $targetDir 'ffwrap.ini'
    $source    = Join-Path $PSScriptRoot 'ffwrap.cs'
    $csc       = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'

    if ($Test) { Say "TEST MODE: config $configDir, target $targetDir (Jellyfin is not stopped)" }

    if (-not $Test) {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            Finish $false 'this needs administrator rights. Right-click the .bat and choose "Run as administrator".'
        }
    }

    Step 'Checking'
    if (-not (Test-Path -LiteralPath $encoding)) {
        Finish $false "Jellyfin's settings were not found at $encoding. Is Jellyfin Server installed on this PC?"
    }
    if ($Mode -eq 'install') {
        if (-not (Test-Path -LiteralPath $source)) { Finish $false "ffwrap.cs is missing. Keep all the files from the zip together in one folder." }
        if (-not (Test-Path -LiteralPath $csc))    { Finish $false "the C# compiler that comes with Windows (.NET Framework 4) was not found at $csc." }
    }
    Say "Jellyfin settings: $encoding"

    # ---- read the current ffmpeg setting -------------------------------------------
    $doc = New-Object Xml.XmlDocument
    $doc.PreserveWhitespace = $true
    $doc.Load($encoding)
    $root = $doc.DocumentElement
    function Node([string]$name) {
        $n = $root.SelectSingleNode($name)
        if (-not $n) { $n = $doc.CreateElement($name); [void]$root.AppendChild($n) }
        return $n
    }
    $current = ([string]$root.EncoderAppPath).Trim()
    function IniValue([string]$key) {
        if (-not (Test-Path -LiteralPath $ini)) { return $null }
        foreach ($l in Get-Content -LiteralPath $ini) {
            if ($l.Trim() -match "^$key=(.*)$") { return $Matches[1].Trim() }
        }
        return $null
    }
    $proc = Get-Process jellyfin -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($proc -and $proc.Path) { $script:jfDir = Split-Path $proc.Path }
    $jfDir = $script:jfDir

    # The real ffmpeg.  Whatever Jellyfin points at may itself be an ffwrap -- this
    # one, or an earlier copy in another folder (a few KB; a real ffmpeg is tens of
    # MB) -- so follow wrappers through their ffwrap.ini, or to Jellyfin's own copy.
    function Resolve-Real([string]$cand) {
        for ($hop = 0; $hop -lt 4 -and $cand; $hop++) {
            if (-not (Test-Path -LiteralPath $cand)) { return $null }
            if ((Get-Item -LiteralPath $cand).Length -ge 2MB) { return $cand }
            $next = $null
            $wini = Join-Path (Split-Path $cand) 'ffwrap.ini'
            if (Test-Path -LiteralPath $wini) {
                foreach ($l in Get-Content -LiteralPath $wini) {
                    if ($l.Trim() -match '^ffmpeg=(.*)$') { $next = $Matches[1].Trim() }
                }
            }
            if (-not $next -or $next -eq $cand) { $next = Join-Path $jfDir 'ffmpeg.exe' }
            if ($next -eq $cand) { return $null }
            $cand = $next
        }
        return $null
    }
    $real = $null
    if ($current) { $real = Resolve-Real $current }
    if (-not $real) { $real = Resolve-Real (IniValue 'ffmpeg') }
    if (-not $real) { $real = Resolve-Real (Join-Path $jfDir 'ffmpeg.exe') }
    if (-not $real) {
        Finish $false "Jellyfin's own ffmpeg.exe was not found (Jellyfin points at '$current'; also looked in $jfDir)."
    }
    Say "Jellyfin's ffmpeg: $real"

    # ---- stop Jellyfin: it writes its settings back on exit ---------------------------
    if (-not $Test) {
        Step 'Stopping Jellyfin'
        $service = Get-Service -Name 'JellyfinServer' -ErrorAction SilentlyContinue
        if ($service -and $service.Status -eq 'Running') {
            Stop-Service -Name 'JellyfinServer' -Force
            $script:stoppedService = $true
            Say 'Jellyfin service stopped.'
        } elseif (Get-Process jellyfin -ErrorAction SilentlyContinue) {
            Say 'Jellyfin is running. Right-click the Jellyfin icon by the clock and choose'
            Say '"Exit" (or close Jellyfin), then come back to this window.'
            $waited = 0
            while (Get-Process jellyfin -ErrorAction SilentlyContinue) {
                if ($waited -ge 300) { Finish $false 'Jellyfin was still running after 5 minutes. Nothing was changed.' }
                Start-Sleep -Seconds 1; $waited++
                if ($waited % 10 -eq 0) { Say "  still waiting for Jellyfin to close ($waited s)..." }
            }
            Say 'Jellyfin has closed.'
            $script:stoppedTray = $true
        } else {
            Say 'Jellyfin is not running.'
        }
        Start-Sleep -Seconds 2
    }

    function SaveSettings([string]$path) {
        (Node 'EncoderAppPath').InnerText = $path
        (Node 'EncoderAppPathDisplay').InnerText = $path
        $w = New-Object IO.StreamWriter($encoding, $false, (New-Object Text.UTF8Encoding($false)))
        try { $doc.Save($w) } finally { $w.Close() }
    }

    if ($Mode -eq 'install') {
        Step 'Building ffwrap'
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
        $built = Join-Path $env:TEMP ('ffwrap-' + [guid]::NewGuid().ToString('N') + '.exe')
        $ErrorActionPreference = 'Continue'
        $out = & $csc /nologo /optimize /platform:x64 "/out:$built" $source 2>&1
        $rc = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($rc -ne 0 -or -not (Test-Path -LiteralPath $built)) {
            Say ($out | Out-String)
            Finish $false 'the C# compiler could not build ffwrap.cs (its messages are above).'
        }
        if (Test-Path -LiteralPath $wrapper) {
            # A transcode may still be using it: a running exe can be renamed, not overwritten.
            $old = Join-Path $targetDir ('ffmpeg.old-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.exe')
            Rename-Item -LiteralPath $wrapper -NewName (Split-Path $old -Leaf)
        }
        Move-Item -LiteralPath $built -Destination $wrapper
        $probeSrc = Join-Path (Split-Path $real) 'ffprobe.exe'
        if (Test-Path -LiteralPath $probeSrc) {
            Copy-Item -LiteralPath $probeSrc -Destination (Join-Path $targetDir 'ffprobe.exe') -Force
        } elseif (-not (Test-Path -LiteralPath (Join-Path $targetDir 'ffprobe.exe'))) {
            Finish $false "ffprobe.exe was not found next to $real. Nothing in Jellyfin's settings was changed."
        }
        $rate = IniValue 'readrate'
        if (-not $rate) { $rate = '1.75' }
        Set-Content -LiteralPath $ini -Encoding UTF8 -Value @("readrate=$rate", "ffmpeg=$real")
        Say "Installed to $targetDir (read rate ${rate}x)"

        Step 'Testing ffwrap'
        $ErrorActionPreference = 'Continue'
        $v = & $wrapper -version 2>&1 | Out-String
        $rc = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($rc -ne 0 -or $v -notmatch 'ffmpeg version') {
            Say $v
            Finish $false "ffwrap was built but could not run Jellyfin's ffmpeg. Jellyfin's settings were not changed."
        }
        Say 'ffwrap runs Jellyfin''s ffmpeg correctly.'

        Step 'Pointing Jellyfin at ffwrap'
        $backup = "$encoding.before-ffwrap"
        if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $encoding -Destination $backup }
        SaveSettings $wrapper
        $check = New-Object Xml.XmlDocument; $check.Load($encoding)
        if (([string]$check.DocumentElement.EncoderAppPath).Trim() -ne $wrapper) {
            Finish $false "Jellyfin's settings could not be updated ($encoding)."
        }
        Say "Saved (the old settings are in $backup)."
    } else {
        Step 'Pointing Jellyfin back at its own ffmpeg'
        SaveSettings $real
        $check = New-Object Xml.XmlDocument; $check.Load($encoding)
        if (([string]$check.DocumentElement.EncoderAppPath).Trim() -ne $real) {
            Finish $false "Jellyfin's settings could not be updated ($encoding)."
        }
        Say "Jellyfin uses $real again."
        Step 'Removing ffwrap'
        $left = @()
        foreach ($f in Get-ChildItem -LiteralPath $targetDir -File -ErrorAction SilentlyContinue) {
            if ($f.Name -match '^(ffmpeg(\.old-.*)?\.exe|ffprobe\.exe|ffwrap\.(ini|log))$') {
                try { Remove-Item -LiteralPath $f.FullName -Force } catch { $left += $f.Name }
            }
        }
        if ((Test-Path -LiteralPath $targetDir) -and -not (Get-ChildItem -LiteralPath $targetDir -Force)) {
            Remove-Item -LiteralPath $targetDir -Force
        }
        if ($left.Count) { Say ("Still in use, delete later: " + ($left -join ', ')) } else { Say "Removed $targetDir" }
    }

    if ($Mode -eq 'install') {
        Finish $true 'ffwrap is installed. Jellyfin now downloads debrid streams at a steady rate.'
    }
    Finish $true 'ffwrap is removed. Jellyfin uses its own ffmpeg again.'
}
catch {
    Finish $false ("unexpected error: " + $_.Exception.Message)
}
