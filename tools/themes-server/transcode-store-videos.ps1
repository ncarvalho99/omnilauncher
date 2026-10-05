<#
.SYNOPSIS
  Re-encode non-compliant store wallpapers into the console-playable profile and
  stage them on the themes server (never touches live files).

.DESCRIPTION
  Console limits mirror VideoPlayer.cpp wallpaperStreamRejection(): H.264,
  8-bit 4:2:0, <= 1920x1080, <= 60 fps. Each name in -ListFile is downloaded
  from /srv/themes/videos, encoded, re-probed locally, and uploaded to
  /home/ubuntu/vt/out. Existing outputs are skipped, so the script is re-runnable.
  Windows PowerShell 5.1 compatible. Audio is dropped (wallpapers are silent).
#>
param(
    [Parameter(Mandatory)] [string] $ListFile,
    [string] $Key = 'C:\Users\nata.carvalho\Documents\oracle.key',
    [string] $Remote = 'ubuntu@147.15.71.60',
    [string] $Work = 'D:\vt'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force "$Work\in", "$Work\out" | Out-Null
$names = Get-Content $ListFile | Where-Object { $_ }
function Log($f, $m) { "[{0:HH:mm:ss}] {1} {2}" -f (Get-Date), $f, $m | Tee-Object -FilePath "$Work\run.log" -Append | Write-Host }

foreach ($f in $names) {
    $in = Join-Path "$Work\in" $f; $out = Join-Path "$Work\out" $f
    try {
        if (-not (Test-Path $out)) {
            if (-not (Test-Path $in)) {
                Log $f 'download'
                scp -i $Key -o BatchMode=yes "${Remote}:/srv/themes/videos/$f" "$in.part"
                if ($LASTEXITCODE) { throw 'scp down failed' }
                Move-Item "$in.part" $in
            }
            Log $f 'encode'
            ffmpeg -nostdin -hide_banner -loglevel error -y -i $in -map 0:v:0 -an `
                -vf "scale=w='min(iw,1920)':h='min(ih,1080)':force_original_aspect_ratio=decrease:force_divisible_by=2,format=yuv420p" `
                -c:v libx264 -preset medium -crf 22 -profile:v high -level 4.2 `
                -maxrate 14M -bufsize 28M -map_metadata -1 -map_chapters -1 -write_tmcd 0 `
                -movflags +faststart "$out.part.mp4"
            if ($LASTEXITCODE) { throw 'ffmpeg failed' }
            Move-Item "$out.part.mp4" $out
        }
        # Verify the product, not the exit code.
        $p = ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,pix_fmt,width,height,r_frame_rate -of csv=p=0 $out
        # ffprobe csv emits fields in its own fixed order, not the requested one:
        # codec_name, width, height, pix_fmt, r_frame_rate.
        $c = ([string](@($p)[0])).Trim() -split ','
        $q = ($c[4] -split '/'); $r = [double]$q[0] / [double]$q[1]
        if ($c[0] -ne 'h264' -or [int]$c[1] -gt 1920 -or [int]$c[2] -gt 1080 -or $c[3] -ne 'yuv420p' -or $r -gt 60.01) { throw "output fails limits: $p" }
        Log $f "upload ($p)"
        scp -i $Key -o BatchMode=yes $out "${Remote}:/home/ubuntu/vt/out/$f"
        if ($LASTEXITCODE) { throw 'scp up failed' }
        Log $f 'OK'
        Remove-Item $in -ErrorAction SilentlyContinue
    } catch {
        Log $f "FAIL $_"
    }
}
