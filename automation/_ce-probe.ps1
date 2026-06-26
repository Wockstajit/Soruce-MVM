$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 1000
  $buf=New-Object byte[] 16384; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
function CE($js){ Send ('mirv_filmmaker editor eval "'+$js+'"') }
# recursively dump FmTypeDrop tree: id/type/text
Write-Host (CE "var o='';(function w(p,d){o+=d+p.paneltype+':'+(p.id||'-')+(p.paneltype=='Label'?'=\""'+p.text+'\""':'')+'|';for(var i=0;i<p.GetChildCount();i++)w(p.GetChild(i),d+'.');})(`$('#FmTypeDrop'),'');`$.Msg('[TREE] '+o.substr(0,200)+'\n')")
$c.Close()
