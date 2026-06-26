$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 600
  $buf=New-Object byte[] 16384; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
function CE($js){ Send ('mirv_filmmaker editor eval "'+$js+'"') | Out-Null }
CE "`$.CamEditor.setInspectorMode('follow')"
Start-Sleep -Milliseconds 300
CE "var l=`$('#FmTypeDrop').GetChild(0);l.style.fontSize='14px';l.style.textAlign='center';l.style.width='100%';l.style.textTransform='none';l.style.paddingLeft='28px';l.style.paddingRight='0px';l.style.marginRight='28px'"
$c.Close()
Start-Sleep -Milliseconds 400
& "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\capture-game-window.ps1" -Out "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_ce_center.png" | Out-Null
& "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_crop.ps1" -In "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_ce_center.png" -Out "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_ce_center_crop.png" -X 1175 -Y 375 -W 425 -H 95 | Out-Null
Write-Host done
