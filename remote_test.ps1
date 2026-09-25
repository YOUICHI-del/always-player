# ============================================================
#  Always Player v9 リモコン(Always Link) テスト用スクリプト
#  スマホの代わりに、このPCから Always Player を操作します。
#  使い方: Always.exe を起動しておき、このファイルを右クリック →
#          「PowerShell で実行」
# ============================================================
$ErrorActionPreference = 'Stop'
try {
    $client = New-Object Net.Sockets.TcpClient('127.0.0.1', 50505)
} catch {
    Write-Host "Always Player に接続できません。Always.exe が起動しているか確認してください。" -ForegroundColor Red
    Read-Host "Enterで終了"
    exit
}
$stream = $client.GetStream()
# 受信データは自前で改行ごとに区切る（StreamReaderは先読みしてしまい、
# 届いている行がすぐ表示されないことがあるため）
$script:pending = New-Object System.Collections.Generic.List[byte]
$writer = New-Object IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)))
$writer.AutoFlush = $true
$volume = 100

function Show-Incoming {
    Start-Sleep -Milliseconds 400
    $buf = New-Object byte[] 65536
    while ($stream.DataAvailable) {
        $n = $stream.Read($buf, 0, $buf.Length)
        for ($i = 0; $i -lt $n; $i++) { $script:pending.Add($buf[$i]) }
        if (-not $stream.DataAvailable) { Start-Sleep -Milliseconds 100 }
    }
    $lines = @()
    while (($idx = $script:pending.IndexOf([byte]10)) -ge 0) {
        $lines += [Text.Encoding]::UTF8.GetString($script:pending.GetRange(0, $idx).ToArray())
        $script:pending.RemoveRange(0, $idx + 1)
    }
    if ($lines.Count -eq 0 -and $script:showLast -and $script:lastStatus) {
        $lines = @(($script:lastStatus | ConvertTo-Json -Compress))
    }
    $script:showLast = $false
    foreach ($line in $lines) {
        try { $o = $line | ConvertFrom-Json } catch { continue }
        switch ($o.type) {
            'hello'  { Write-Host "接続OK: $($o.app) v$($o.version)" -ForegroundColor Green }
            'status' {
                $script:lastStatus = $o
                $script:volume = [int]$o.volume
                $t = [TimeSpan]::FromSeconds([double]$o.pos).ToString('m\:ss')
                $d = [TimeSpan]::FromSeconds([double]$o.dur).ToString('m\:ss')
                Write-Host ("[{0}] {1} / {2}  ({3}/{4})  {5}/{6}  音量{7}" -f `
                    $o.state, $o.title, $o.artist, ($o.index + 1), $o.total, $t, $d, $o.volume) -ForegroundColor Cyan
            }
            'art'    {
                $kb = [math]::Round(([string]$o.data).Length * 3 / 4 / 1024, 1)
                Write-Host "ジャケット画像を受信: 番号=$($o.id)  約${kb}KB" -ForegroundColor Yellow
            }
        }
    }
}

function Send-Cmd($cmd, $value = $null) {
    $obj = @{ cmd = $cmd }
    if ($null -ne $value) { $obj.value = $value }
    $writer.WriteLine(($obj | ConvertTo-Json -Compress))
}

Show-Incoming
while ($true) {
    Write-Host ""
    Write-Host "1:再生/一時停止  2:次の曲  3:前の曲  4:停止  5:音量+10  6:音量-10  7:30秒へシーク  Enter:状態表示  q:終了"
    $k = Read-Host "操作"
    switch ($k) {
        '1' { Send-Cmd 'toggle' }
        '2' { Send-Cmd 'next' }
        '3' { Send-Cmd 'prev' }
        '4' { Send-Cmd 'stop' }
        '5' { Send-Cmd 'volume' ([math]::Min(100, $volume + 10)) }
        '6' { Send-Cmd 'volume' ([math]::Max(0, $volume - 10)) }
        '7' { Send-Cmd 'seek' 30 }
        ''  { $script:showLast = $true }   # 変化が無くても最後の状態を表示し直す
        'q' { $client.Close(); exit }
    }
    Show-Incoming
}
