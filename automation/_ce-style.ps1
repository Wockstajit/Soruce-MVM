$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 600
  $buf=New-Object byte[] 16384; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
function CE($js){ Send ('mirv_filmmaker editor eval "'+$js+'"') | Out-Null }
CE "`$.CamEditor.setInspectorMode('follow')"
Send 'mirv_filmmaker follow nearest' | Out-Null
Start-Sleep -Milliseconds 400
# define panel + label stylers (split to fit the 256-byte netcon limit)
CE "SDP=function(id){var d=`$('#'+id);if(!d)return;d.style.borderRadius='4px';d.style.border='1px solid #ffffff2e';d.style.backgroundColor='#1b2230';d.style.backgroundPosition='right 9px 50%';d.style.backgroundSize='12px 12px';}"
CE "SDL=function(id){var d=`$('#'+id);if(!d)return;(function w(p){for(var i=0;i<p.GetChildCount();i++){var c=p.GetChild(i);if(c.paneltype=='Label'){c.style.fontSize='13px';c.style.textTransform='none';c.style.fontWeight='normal';c.style.marginRight='22px';c.style.color='#e6eaef';}w(c);}})(d);}"
foreach($id in 'FmTypeDrop','FmTargetDrop','FmSrcDrop'){ CE "SDP('$id')"; CE "SDL('$id')" }
$c.Close()
Start-Sleep -Milliseconds 500
& "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\capture-game-window.ps1" -Out "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_ce_styled.png" | Out-Null
& "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_crop.ps1" -In "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_ce_styled.png" -Out "C:\Users\ayden\Documents\Github Projects\cs2 filmaker\automation\_ce_styled_crop.png" -X 1175 -Y 300 -W 425 -H 260 | Out-Null
Write-Host done
