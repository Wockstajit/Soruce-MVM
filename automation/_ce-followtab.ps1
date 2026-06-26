$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 800
  $buf=New-Object byte[] 16384; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
function CE($js){ Send ('mirv_filmmaker editor eval "'+$js+'"') | Out-Null }
CE "`$.CamEditor.setInspectorMode('follow')"
$c.Close()
Start-Sleep -Milliseconds 600
& "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\capture-game-window.ps1" -Out "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_ce_follow.png" | Out-Null
Write-Host done
