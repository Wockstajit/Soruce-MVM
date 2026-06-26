$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 1000
  $buf=New-Object byte[] 16384; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
function CE($js){ $r = Send ('mirv_filmmaker editor eval "'+$js+'"'); ($r -split "`n" | Where-Object { $_ -match '^\[' }) | ForEach-Object { Write-Host $_ } }
# dump the DropDown's child structure (id/class/tag) so we know what to style
CE "var d=`$('#FmTargetDrop');var o='';for(var i=0;i<d.GetChildCount();i++){var ch=d.GetChild(i);o+=ch.id+'('+ch.paneltype+') ';}`$.Msg('[KIDS] '+o+'\n')"
CE "var d=`$('#FmTargetDrop');`$.Msg('[DD] h='+d.actuallayoutheight+' cls='+d.GetAttributeString('class','')+'\n')"
$c.Close()
