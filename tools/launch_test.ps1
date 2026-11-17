# Launch-survival test: copies config\SR3Population.ini to the game, applies key=value overrides, starts the game, waits up to $Seconds,
# then kills it if still alive. Prints ALIVE / CRASH (+ the plugin's CRASH lines) and how many 4-second monitor ticks were logged.
#   powershell -File tools\launch_test.ps1 -Seconds 60 -Set "AuditHooks=0","FixCapGetters=0"
param([int]$Seconds = 60, [string[]]$Set = @(), [switch]$Continue, [int]$MenuAt = 40)
$g = 'C:\Games\Saints Row 3'
if (Get-Process SaintsRow* -ea 0) { 'GAME ALREADY RUNNING - aborting'; exit 1 }
$ini = Get-Content 'D:\SR3-Population-Mod\config\SR3Population.ini'
foreach ($kv in $Set) {
    $k = $kv.Split('=')[0]
    if ($ini -match "^$k=") { $ini = $ini -replace "^$k=.*", $kv } else { $ini += $kv }
}
if ($Continue) { $ini = @($ini | ? { $_ -notmatch '^AutoContinueSeconds=' }) + "AutoContinueSeconds=$MenuAt" }   # plugin presses CONTINUE itself
$ini | Set-Content "$g\SR3Population.ini" -Encoding ascii
Remove-Item "$g\SR3Population.log", "$g\SR3Population.dmp" -ea 0
$p = Start-Process -FilePath "$g\SaintsRowTheThird_DX11.exe" -WorkingDirectory $g -PassThru
$t = 0
while ($t -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep 2; $t += 2
    # -Continue: once the main menu is up, press Enter on CONTINUE (loads the latest save) and again a bit later for any confirm prompt
}
$alive = -not $p.HasExited
if ($alive) {
    try {   # screenshot of the primary screen just before closing (D:\SR3-Population-Mod\tests\last.png)
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $gr = [System.Drawing.Graphics]::FromImage($bmp); $gr.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        New-Item -ItemType Directory -Force 'D:\SR3-Population-Mod\tests' | Out-Null
        $small = New-Object System.Drawing.Bitmap $bmp, ([int]($b.Width / 2)), ([int]($b.Height / 2))
        $small.Save('D:\SR3-Population-Mod\tests\last.png', [System.Drawing.Imaging.ImageFormat]::Png)
        $gr.Dispose(); $bmp.Dispose(); $small.Dispose()
    } catch { "screenshot failed: $_" }
    Stop-Process -Id $p.Id -Force; Start-Sleep 2
}
$log = if (Test-Path "$g\SR3Population.log") { Get-Content "$g\SR3Population.log" } else { @() }
$ticks = ($log | Select-String '^DIAG').Count
"{0} after {1}s  ticks={2}  set=[{3}]" -f ($(if ($alive) { 'ALIVE' } else { 'EXITED' })), $t, $ticks, ($Set -join ' ')
$log | Select-String '^CRASH|^  eax|stack return|^SKIP' | ForEach-Object { '  ' + $_.Line }
