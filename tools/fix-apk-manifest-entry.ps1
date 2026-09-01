# 修复 AGP zipflinger 在 Windows 上打包 full 变体时 AndroidManifest.xml
# 条目名损坏的 bug（名字被写成 "AndroidMan" + PK\x01\x02 + 5 个零字节）。
# 坏名字与正确名字 "AndroidManifest.xml" 恰好同为 19 字节，可原地补丁，
# 之后必须用 apksigner 重新签名（v2/v3 签名覆盖整个文件）。
#
# 用法: powershell -File fix-apk-manifest-entry.ps1 <in.apk> <out.apk>
param(
    [Parameter(Mandatory = $true)][string]$InApk,
    [Parameter(Mandatory = $true)][string]$OutApk
)

$ErrorActionPreference = "Stop"
$correctName = "AndroidManifest.xml"
$nameBytes = [Text.Encoding]::ASCII.GetBytes($correctName)
if ($nameBytes.Length -ne 19) { throw "internal error: name length != 19" }

Copy-Item $InApk $OutApk -Force
$fs = [IO.File]::Open($OutApk, 'Open', 'ReadWrite')

try {
    # --- 解析 EOCD / 中央目录 ---
    $len = $fs.Length
    $tailLen = [Math]::Min(65557, $len)
    [void]$fs.Seek(-$tailLen, [IO.SeekOrigin]::End)
    $tail = New-Object byte[] $tailLen
    [void]$fs.Read($tail, 0, $tailLen)
    $eocd = -1
    for ($i = $tailLen - 22; $i -ge 0; $i--) {
        if ($tail[$i] -eq 0x50 -and $tail[$i+1] -eq 0x4B -and $tail[$i+2] -eq 0x05 -and $tail[$i+3] -eq 0x06) { $eocd = $i; break }
    }
    if ($eocd -lt 0) { throw "EOCD not found" }
    $cdOff  = [BitConverter]::ToUInt32($tail, $eocd + 16)
    $cdSize = [BitConverter]::ToUInt32($tail, $eocd + 12)
    [void]$fs.Seek($cdOff, [IO.SeekOrigin]::Begin)
    $cd = New-Object byte[] $cdSize
    [void]$fs.Read($cd, 0, $cdSize)
    Write-Host "EOCD ok: cdOff=$cdOff cdSize=$cdSize"

    # --- 遍历中央目录，定位损坏的 manifest 条目 ---
    $pos = 0
    $patched = 0
    while ($pos + 46 -le $cdSize) {
        if (!($cd[$pos] -eq 0x50 -and $cd[$pos+1] -eq 0x4B -and $cd[$pos+2] -eq 0x01 -and $cd[$pos+3] -eq 0x02)) { break }
        $nameLen  = [BitConverter]::ToUInt16($cd, $pos + 28)
        $extraLen = [BitConverter]::ToUInt16($cd, $pos + 30)
        $name = [Text.Encoding]::ASCII.GetString($cd, $pos + 46, $nameLen)
        $lho  = [BitConverter]::ToUInt32($cd, $pos + 42)

        if ($nameLen -eq 19 -and $name.StartsWith("AndroidMan") -and $name -ne $correctName) {
            Write-Host "found corrupted entry at cdPos=$pos lho=$lho name='$name'"

            # 1) 补丁中央目录名字
            [void]$fs.Seek($cdOff + $pos + 46, [IO.SeekOrigin]::Begin)
            $fs.Write($nameBytes, 0, 19)
            $fs.Flush()
            Write-Host "CD name patched at $($cdOff + $pos + 46)"

            # 2) 校验并补丁本地头名字
            $localNamePos = [int64]$lho + 26
            [void]$fs.Seek($localNamePos, [IO.SeekOrigin]::Begin)
            Write-Host "seeked to $($fs.Position) (expect $localNamePos)"
            $lhLens = New-Object byte[] 4
            $nread = $fs.Read($lhLens, 0, 4)
            $lNameLen = [BitConverter]::ToUInt16($lhLens, 0)
            Write-Host "local nameLen read=$lNameLen (bytes: $(($lhLens | ForEach-Object { $_.ToString('X2') }) -join ' '))"
            if ($lNameLen -ne 19) { throw "local header nameLen=$lNameLen != 19 at $lho" }
            [void]$fs.Seek([int64]$lho + 30, [IO.SeekOrigin]::Begin)
            $fs.Write($nameBytes, 0, 19)
            $fs.Flush()
            $patched++
            Write-Host "local header name patched at $($lho + 30)"
        }
        $pos += 46 + $nameLen + $extraLen
    }
} finally {
    $fs.Dispose()
}

if ($patched -eq 0) { throw "no corrupted manifest entry found (already fixed?)" }
Write-Host "OK: $patched entry patched. Now re-sign with apksigner."
