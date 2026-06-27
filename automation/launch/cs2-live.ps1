#requires -Version 5
<#
============================================================
  CS2 netcon LIVE dashboard  (browser)
============================================================
  Bridges the running CS2's netcon console (TCP -netconport) to
  a local web page it opens in your browser. You see:
    * the live console stream (colourised, auto-scroll),
    * parsed camera-marker readouts that update in real time -
      marker COUNT, selected #, speed mode, constant speed,
      path mode, timing, playback state, current SEGMENT +
      progress bar, and the last reported path duration,
    * quick buttons + a command box to drive it from the browser.

  So you can press K/J/L/F in-game (or click the buttons here)
  and watch the numbers change live.

  PREREQ: CS2 running with netcon (automation\launch\launch-cs2-netcon.ps1 -Port 29010)
          and, to place/play markers, a demo loaded + playing.

  RUN:
    powershell -ExecutionPolicy Bypass -File automation\launch\cs2-live.ps1
    powershell -ExecutionPolicy Bypass -File automation\launch\cs2-live.ps1 -Port 29010 -WebPort 8765
    powershell -ExecutionPolicy Bypass -File automation\launch\cs2-live.ps1 -Demo "F:\path\to\demo.dem"

  Stop with Ctrl+C in this window (or the Stop button on the page).
============================================================
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,     # CS2 netcon port
    [int]$WebPort = 8765,    # local web dashboard port
    [string]$Demo = ''       # optional demo to load when the dashboard connects
)
$ErrorActionPreference = 'Stop'

function Quote-ConsoleArg([string]$value) {
    if ($null -eq $value) { return '""' }
    return '"' + ($value -replace '"', '\"') + '"'
}

# --- preflight ---------------------------------------------------------------
$cs2 = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
if (-not $cs2) { Write-Host '[WARN] cs2.exe not running yet - start it with launch-cs2-netcon.ps1; the dashboard will keep retrying.' -ForegroundColor Yellow }

# Shared, thread-safe state between the netcon reader runspace and the web loop.
$shared = [hashtable]::Synchronized(@{})
$shared.lines = [System.Collections.ArrayList]::Synchronized((New-Object System.Collections.ArrayList))
$shared.cmds  = [System.Collections.Queue]::Synchronized((New-Object System.Collections.Queue))
$shared.connected = $false
$shared.stop = $false

# --- background netcon reader (its own runspace, shares $shared) -------------
$readerScript = {
    $enc = [System.Text.Encoding]::ASCII
    while (-not $shared.stop) {
        $client = $null
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $client.Connect('127.0.0.1', $Port)
            $client.NoDelay = $true
            $stream = $client.GetStream()
            $shared.connected = $true
            [void]$shared.lines.Add(@{ t = (Get-Date).ToString('HH:mm:ss'); s = '*** connected to netcon 127.0.0.1:' + $Port + ' ***' })
            $buf = New-Object byte[] 8192
            $partial = ''
            while (-not $shared.stop) {
                while ($shared.cmds.Count -gt 0) {
                    $c = $shared.cmds.Dequeue()
                    $b = $enc.GetBytes([string]$c + "`n")
                    $stream.Write($b, 0, $b.Length); $stream.Flush()
                    [void]$shared.lines.Add(@{ t = (Get-Date).ToString('HH:mm:ss'); s = '> ' + $c })
                }
                if ($stream.DataAvailable) {
                    $n = $stream.Read($buf, 0, $buf.Length)
                    if ($n -gt 0) {
                        $partial += $enc.GetString($buf, 0, $n)
                        while (($i = $partial.IndexOf("`n")) -ge 0) {
                            $line = $partial.Substring(0, $i).TrimEnd("`r")
                            $partial = $partial.Substring($i + 1)
                            if ($line.Length -gt 0) { [void]$shared.lines.Add(@{ t = (Get-Date).ToString('HH:mm:ss'); s = $line }) }
                        }
                    }
                } else { Start-Sleep -Milliseconds 40 }
            }
        } catch {
            $shared.connected = $false
            [void]$shared.lines.Add(@{ t = (Get-Date).ToString('HH:mm:ss'); s = '*** netcon not reachable, retrying... ***' })
            Start-Sleep -Seconds 2
        } finally {
            if ($client) { $client.Close() }
        }
    }
}
$rs = [runspacefactory]::CreateRunspace(); $rs.Open()
$rs.SessionStateProxy.SetVariable('shared', $shared)
$rs.SessionStateProxy.SetVariable('Port', $Port)
$psReader = [powershell]::Create(); $psReader.Runspace = $rs
[void]$psReader.AddScript($readerScript)
$readerHandle = $psReader.BeginInvoke()

# Prime the dashboard with current state, optionally loading a specific demo first.
if (-not [string]::IsNullOrWhiteSpace($Demo)) {
    [void]$shared.lines.Add(@{ t = (Get-Date).ToString('HH:mm:ss'); s = '*** queued demo load: ' + $Demo + ' ***' })
    $shared.cmds.Enqueue('playdemo ' + (Quote-ConsoleArg $Demo))
}
$shared.cmds.Enqueue('mirv_filmmaker marker list')

# --- the dashboard page ------------------------------------------------------
$html = @'
<!DOCTYPE html><html><head><meta charset="utf-8"><title>CS2 Camera-Path Live</title>
<style>
  :root{--bg:#0e1116;--panel:#161b22;--line:#222b36;--gold:#f0b323;--blue:#4aa3ff;--red:#ff5d5d;--cyan:#54d6c4;--label:#9aa4b0;--val:#eef2f6}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--val);font:14px/1.4 "Segoe UI",Arial,sans-serif}
  header{padding:10px 16px;background:#0a0d12;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:12px}
  header h1{font-size:15px;margin:0;letter-spacing:1px;color:var(--gold)}
  #conn{font-size:12px;padding:2px 8px;border-radius:10px;background:#333;color:#bbb}
  #conn.ok{background:#13361f;color:#6fe39a}#conn.bad{background:#3a1414;color:#ff8a8a}
  .wrap{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:12px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px}
  .cards{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
  .stat{background:#0d1117;border:1px solid var(--line);border-radius:6px;padding:10px}
  .stat .k{font-size:11px;color:var(--label);text-transform:uppercase;letter-spacing:.5px}
  .stat .v{font-size:22px;font-weight:700;margin-top:3px}
  .v.gold{color:var(--gold)}.v.blue{color:var(--blue)}.v.cyan{color:var(--cyan)}
  .bar{height:8px;background:#0d1117;border:1px solid var(--line);border-radius:4px;margin-top:8px;overflow:hidden}
  .bar>i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--gold),var(--cyan));transition:width .15s}
  h2{font-size:12px;color:var(--label);text-transform:uppercase;letter-spacing:1px;margin:0 0 8px}
  #log{height:60vh;overflow:auto;background:#0a0d12;border:1px solid var(--line);border-radius:6px;padding:8px;font:12px/1.45 Consolas,monospace;white-space:pre-wrap}
  #log .t{color:#5a6675}#log .campath{color:var(--gold)}#log .play{color:var(--cyan)}#log .warn{color:var(--red)}#log .cmd{color:#7fb3ff}#log .sys{color:#7d8a99;font-style:italic}
  .btns{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}
  button{background:#1d2733;color:var(--val);border:1px solid var(--line);border-radius:5px;padding:6px 10px;cursor:pointer;font-size:12px}
  button:hover{border-color:var(--gold)}
  button.stop{border-color:#5a2222;color:#ff9b9b}
  #cmd{flex:1;background:#0d1117;color:var(--val);border:1px solid var(--line);border-radius:5px;padding:7px 9px;font:12px Consolas,monospace}
  .row{display:flex;gap:6px}
  label.chk{font-size:12px;color:var(--label);display:flex;align-items:center;gap:5px;margin-left:auto}
</style></head><body>
<header><h1>CS2 CAMERA-PATH &mdash; LIVE</h1><span id="conn">connecting...</span>
  <label class="chk"><input type="checkbox" id="auto" checked> auto-refresh state (2s)</label>
</header>
<div class="wrap">
  <div>
    <div class="card">
      <h2>Camera markers</h2>
      <div class="cards">
        <div class="stat"><div class="k">Markers</div><div class="v gold" id="s_count">-</div></div>
        <div class="stat"><div class="k">Selected #</div><div class="v" id="s_sel">-</div></div>
        <div class="stat"><div class="k">Playback</div><div class="v" id="s_state">editing</div></div>
        <div class="stat"><div class="k">Speed mode</div><div class="v gold" id="s_speed">-</div></div>
        <div class="stat"><div class="k">Const speed</div><div class="v" id="s_const">-</div></div>
        <div class="stat"><div class="k">Path mode</div><div class="v blue" id="s_interp">-</div></div>
      </div>
      <div style="margin-top:10px" class="cards">
        <div class="stat"><div class="k">Timing</div><div class="v" id="s_timing">-</div></div>
        <div class="stat"><div class="k">Segment</div><div class="v cyan" id="s_seg">-</div></div>
        <div class="stat"><div class="k">Last duration</div><div class="v" id="s_dur">-</div></div>
      </div>
      <div class="bar"><i id="s_prog"></i></div>
    </div>
    <div class="card" style="margin-top:12px">
      <h2>Drive it</h2>
      <div class="btns">
        <button data-c="mirv_filmmaker marker list">list</button>
        <button data-c="mirv_filmmaker marker place">place (K)</button>
        <button data-c="mirv_filmmaker marker preview">arm (J)</button>
        <button data-c="mirv_filmmaker marker previewplay">play (Space)</button>
        <button data-c="mirv_filmmaker marker previewstop">stop (X)</button>
        <button data-c="mirv_filmmaker marker speedmode cycle">speedmode</button>
        <button data-c="mirv_filmmaker marker interp cycle">interp</button>
        <button data-c="mirv_filmmaker marker constspeed cycle">constspeed</button>
        <button data-c="mirv_filmmaker marker autosnap toggle">autosnap</button>
        <button data-c="mirv_filmmaker marker deleteall confirm">delete all</button>
      </div>
      <div class="row">
        <input id="cmd" placeholder="type any console command, Enter to send (e.g. mirv_filmmaker marker list)">
        <button id="send">Send</button>
        <button id="stopsrv" class="stop">Stop server</button>
      </div>
    </div>
  </div>
  <div class="card"><h2>Console stream</h2><div id="log"></div></div>
</div>
<script>
var next=0, st={count:'-',sel:'-',speed:'-',cnst:'-',interp:'-',timing:'-',state:'editing',seg:'-',prog:0,dur:'-'};
function $(id){return document.getElementById(id)}
function setConn(ok){var c=$('conn');c.className=ok?'ok':'bad';c.textContent=ok?'connected':'no netcon';}
function render(){
  $('s_count').textContent=st.count;$('s_sel').textContent=st.sel;$('s_speed').textContent=st.speed;
  $('s_const').textContent=st.cnst;$('s_interp').textContent=st.interp;$('s_timing').textContent=st.timing;
  $('s_state').textContent=st.state;$('s_state').className='v '+(st.state==='PLAYING'?'cyan':'');
  $('s_seg').textContent=st.seg;$('s_dur').textContent=st.dur;$('s_prog').style.width=(st.prog||0)+'%';
}
function parse(s){
  var m;
  if(m=s.match(/\[campath\]\s+(\d+)\s+marker\(s\),\s+selected #(-?\d+),\s+speed=([\w-]+),\s+timing=(\w+),\s+interp=(\w+)/)){
    st.count=m[1];st.sel=m[2];st.speed=m[3];st.timing=m[4];st.interp=m[5];}
  if(m=s.match(/\[campath\]\s+marker #\d+ placed/)){ /* count updates via list */ }
  if(m=s.match(/\[campath\]\s+marker #(\d+) deleted \((\d+) left\)/)){st.count=m[2];}
  if(m=s.match(/\[campath\]\s+speed mode -> ([\w-]+)/)){st.speed=m[1];}
  if(m=s.match(/\[campath\]\s+constant speed x([\d.]+)/)){st.cnst=m[1]+'x';}
  if(m=s.match(/\[campath\]\s+interpolation -> (\w+)/)){st.interp=m[1];}
  if(m=s.match(/\[campath\]\s+timing -> (\w+)/)){st.timing=m[1];}
  if(m=s.match(/PLAY:\s+(\d+) markers, speedMode=([\w-]+), interp=(\w+), timing=(\w+), duration=([\d.]+)s/)){
    st.count=m[1];st.speed=m[2];st.interp=m[3];st.timing=m[4];st.dur=m[5]+'s';st.state='PLAYING';st.prog=0;}
  if(m=s.match(/play state=(\w+) seg=(\d+)\/(\d+) prog=([\d.]+)% .*speed=([\w-]+) t=([\d.]+)\/([\d.]+)/)){
    st.state='PLAYING';st.seg=m[2]+'/'+m[3];st.prog=parseFloat(m[4]);st.speed=m[5];}
  if(/reached end of path|preview stopped/.test(s)){st.state='editing';st.prog=0;st.seg='-';}
  if(/PLAY refused: need >=2/.test(s)){st.state='need 2+ markers';}
}
function cls(s){
  if(s.indexOf('***')>=0||s.charAt(0)==='*')return'sys';
  if(s.charAt(0)==='>')return'cmd';
  if(/refused|Warning|cannot|could not|no outgoing/i.test(s))return'warn';
  if(/play state=/.test(s))return'play';
  if(/\[campath\]/.test(s))return'campath';
  return'';
}
function poll(){
  fetch('/poll?from='+next).then(function(r){return r.json()}).then(function(d){
    setConn(d.connected);
    if(d.lines&&d.lines.length){
      var log=$('log');var atBottom=log.scrollTop+log.clientHeight>=log.scrollHeight-30;
      for(var i=0;i<d.lines.length;i++){
        var ln=d.lines[i];parse(ln.s);
        var div=document.createElement('div');
        div.innerHTML='<span class="t">'+ln.t+'</span> <span class="'+cls(ln.s)+'">'+ln.s.replace(/</g,'&lt;')+'</span>';
        log.appendChild(div);
      }
      while(log.childNodes.length>1200)log.removeChild(log.firstChild);
      if(atBottom)log.scrollTop=log.scrollHeight;
      render();
    }
    next=d.next;
  }).catch(function(){setConn(false)});
}
function send(c){fetch('/cmd?c='+encodeURIComponent(c));}
var btns=document.getElementsByTagName('button');
for(var i=0;i<btns.length;i++){(function(b){if(b.dataset.c)b.onclick=function(){send(b.dataset.c)}})(btns[i]);}
$('send').onclick=function(){var v=$('cmd').value.trim();if(v){send(v);$('cmd').value=''}};
$('cmd').addEventListener('keydown',function(e){if(e.key==='Enter')$('send').click()});
$('stopsrv').onclick=function(){if(confirm('Stop the dashboard server?'))fetch('/stop')};
setInterval(poll,250);
setInterval(function(){if($('auto').checked)send('mirv_filmmaker marker list')},2000);
poll();
</script></body></html>
'@

# --- HTTP server -------------------------------------------------------------
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$WebPort/")
try { $listener.Start() }
catch { Write-Host "[FAIL] Could not bind http://localhost:$WebPort/ : $($_.Exception.Message)" -ForegroundColor Red; $shared.stop = $true; exit 1 }
$sawCs2Process = [bool](Get-Process -Name 'cs2' -ErrorAction SilentlyContinue)

Write-Host "=== CS2 live dashboard ===" -ForegroundColor Cyan
Write-Host ("  netcon : 127.0.0.1:{0}" -f $Port)
Write-Host ("  browser: http://localhost:{0}/   (opening...)" -f $WebPort) -ForegroundColor Green
Write-Host "  Stop with Ctrl+C in this window." -ForegroundColor DarkGray
Start-Process ("http://localhost:$WebPort/") | Out-Null

function Write-Text($resp, $text, $type) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $resp.ContentType = $type
    $resp.ContentLength64 = $bytes.Length
    $resp.OutputStream.Write($bytes, 0, $bytes.Length)
    $resp.OutputStream.Close()
}

try {
    while ($listener.IsListening) {
        $pending = $listener.BeginGetContext($null, $null)
        while (-not $pending.AsyncWaitHandle.WaitOne(500)) {
            $cs2Now = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
            if ($cs2Now) { $sawCs2Process = $true }
            if ($sawCs2Process -and -not $cs2Now) {
                [void]$shared.lines.Add(@{ t = (Get-Date).ToString('HH:mm:ss'); s = '*** cs2.exe closed; stopping dashboard server ***' })
                Write-Host "`ncs2.exe closed; stopping dashboard server..." -ForegroundColor Yellow
                break
            }
        }
        $cs2Now = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
        if ($cs2Now) { $sawCs2Process = $true }
        if ($sawCs2Process -and -not $cs2Now) { break }
        $ctx = $listener.EndGetContext($pending)
        $req = $ctx.Request; $resp = $ctx.Response
        $path = $req.Url.AbsolutePath
        if ($path -eq '/') {
            Write-Text $resp $html 'text/html; charset=utf-8'
        }
        elseif ($path -eq '/poll') {
            $from = 0; [void][int]::TryParse([string]$req.QueryString['from'], [ref]$from)
            $snap = $shared.lines.ToArray()
            $total = $snap.Length
            $slice = @()
            if ($from -lt $total) { for ($k = $from; $k -lt $total; $k++) { $slice += $snap[$k] } }
            $obj = @{ next = $total; connected = [bool]$shared.connected; lines = $slice }
            Write-Text $resp ($obj | ConvertTo-Json -Depth 5 -Compress) 'application/json'
        }
        elseif ($path -eq '/cmd') {
            $c = [string]$req.QueryString['c']
            if ($c) { $shared.cmds.Enqueue($c) }
            Write-Text $resp '{"ok":true}' 'application/json'
        }
        elseif ($path -eq '/stop') {
            Write-Text $resp '{"ok":true,"stopping":true}' 'application/json'
            break
        }
        else {
            $resp.StatusCode = 404; $resp.OutputStream.Close()
        }
    }
}
finally {
    Write-Host "`nShutting down..." -ForegroundColor Yellow
    $shared.stop = $true
    try { $listener.Stop() } catch {}
    try { $psReader.Stop() } catch {}
}
