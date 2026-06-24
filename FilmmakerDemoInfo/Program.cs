// FilmmakerDemoInfo - parse a CS2 .dem and print a JSON scoreboard summary.
//
// Usage:  FilmmakerDemoInfo <path-to-demo.dem> [accountId,accountId,...]
// Output: a single JSON object on stdout (see result below). On failure it still
//         prints JSON {"ok":false,"error":"..."} and returns non-zero so the C++
//         caller can cache the negative result and not retry forever.
//
// Stats are read straight off the CCSPlayerController each command (real CS2
// Score / MVPs / MatchStats), and the roster is accumulated DURING the parse -
// reading demo.Players only at the end misses everyone who disconnected in a GOTV
// demo's post-match wind-down (which is why only 2 players showed before).
//
// NAMES-FAST MODE: when the caller passes a comma-separated list of Steam account
// ids (it already has the full scoreboard from the .dem.info matchmaking sidecar
// and only needs player NAMES), the parse is cancelled the instant every wanted
// account id has a name. Names appear in the first second of a demo, so this turns
// a multi-second full decode into a sub-second one. In that mode duration/score
// are left at 0 because they would be truncated - the caller takes those from the
// sidecar / .dem header instead.

using System.Text.Json;
using DemoFile;
using DemoFile.Game.Cs;

if (args.Length < 1)
{
    Console.Out.Write("{\"ok\":false,\"error\":\"usage: FilmmakerDemoInfo <demo.dem> [accountIds]\"}");
    return 2;
}

string path = args[0];
if (!File.Exists(path))
{
    Console.Out.Write("{\"ok\":false,\"error\":\"file not found\"}");
    return 2;
}

var wanted = new HashSet<uint>();
if (args.Length >= 2)
{
    foreach (var tok in args[1].Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        if (uint.TryParse(tok, out var id) && id != 0)
            wanted.Add(id);
}

try
{
    string json = await DemoInfo.ParseAsync(path, wanted);
    Console.Out.Write(json);
    return 0;
}
catch (Exception ex)
{
    Console.Out.Write(JsonSerializer.Serialize(new { ok = false, error = ex.Message }));
    return 1;
}

static class DemoInfo
{
    const ulong SteamId64Base = 76561197960265728UL;

    // Bumped whenever the JSON shape or parse semantics change; the C++ caller
    // discards any cached "<demo>.fmjson" whose "v" does not match this.
    // v3: added per-player "perRound" (the round-performance timeline).
    // v4: "team" now reports the side held at the last scored round (the side the
    //     player ended the match on), not the last side seen in the GOTV wind-down.
    const int SchemaVersion = 4;

    // In names-fast mode, give up waiting for a straggler name after this many
    // commands so a player who connects very late never forces a full decode.
    // Names normally resolve in the first few hundred commands.
    const int NamesFastCommandCap = 40000;

    sealed class Rec
    {
        public ulong SteamId;
        public string Name = "";
        public int Team = -1; // 0 = CT, 1 = T, -1 = unknown/spectator (last seen, any phase)
        public int FinalTeam = -1; // side held as of the last scored round (the side "ended on")
        public int Kills, Deaths, Assists, Score, Mvps;

        // Per-round timeline (full parse only). Each entry: this player's result
        // for one scored round. curr* accumulate the in-progress round.
        public int curKills, curHeadshots, curDied, curMvp;
        public readonly List<int[]> Rounds = new(); // [kills, headshots, died, won, side, mvp]
    }

    public static async Task<string> ParseAsync(string path, HashSet<uint> wantedAccountIds)
    {
        var demo = new CsDemoParser();
        await using var stream = File.OpenRead(path);
        var reader = DemoFileReader.Create(demo, stream);

        var roster = new Dictionary<ulong, Rec>();

        bool namesFast = wantedAccountIds.Count > 0;
        var namedAccounts = namesFast ? new HashSet<uint>() : null;
        using var cts = new CancellationTokenSource();
        int commandCount = 0;

        // Snapshot every connected controller after each command. MatchStats are
        // cumulative, so the last snapshot before a player leaves holds their final
        // totals; players present to the end keep updating to the true final value.
        demo.OnCommandFinishPersistent += () =>
        {
            foreach (var c in demo.Players)
            {
                ulong sid = c.SteamID;
                if (sid == 0) continue; // bots / unassigned

                if (!roster.TryGetValue(sid, out var r))
                {
                    r = new Rec { SteamId = sid };
                    roster[sid] = r;
                }

                if (!string.IsNullOrEmpty(c.PlayerName))
                {
                    r.Name = c.PlayerName;
                    if (namedAccounts != null && sid > SteamId64Base)
                        namedAccounts.Add((uint)(sid - SteamId64Base));
                }

                int team = c.CSTeamNum switch
                {
                    CSTeamNumber.CounterTerrorist => 0,
                    CSTeamNumber.Terrorist => 1,
                    _ => -1
                };
                if (team >= 0)
                    r.Team = team; // keep last CT/T side (ignore brief spectator)

                var ms = c.ActionTrackingServices?.MatchStats;
                if (ms != null)
                {
                    r.Kills = ms.Kills;
                    r.Deaths = ms.Deaths;
                    r.Assists = ms.Assists;
                }
                r.Score = c.Score;
                r.Mvps = c.MVPs;
            }

            // Names-fast: stop as soon as every wanted account id has a name (or we
            // hit the safety cap), since the caller only needs name <-> id mapping.
            if (namesFast)
            {
                ++commandCount;
                if (namedAccounts!.Count >= wantedAccountIds.Count
                    && wantedAccountIds.IsSubsetOf(namedAccounts))
                {
                    cts.Cancel();
                }
                else if (commandCount >= NamesFastCommandCap)
                {
                    cts.Cancel();
                }
            }
        };

        // Per-round timeline: only in full-parse mode (names-fast cancels long
        // before any round happens). Kills/headshots/deaths come from player_death,
        // the round winner from round_end, and we commit one entry per scored round
        // on round_officially_ended. Warmup rounds are dropped at match start.
        if (!namesFast)
        {
            Rec? RecFor(CCSPlayerController? c)
            {
                if (c == null) return null;
                ulong sid = c.SteamID;
                if (sid == 0) return null;
                if (!roster.TryGetValue(sid, out var r))
                {
                    r = new Rec { SteamId = sid };
                    roster[sid] = r;
                }
                return r;
            }

            // round_announce_match_start fires when the live match begins; clear
            // anything captured during warmup. (May not fire if recording started
            // mid-match, in which case we just keep every scored round - still right.)
            demo.Source1GameEvents.RoundAnnounceMatchStart += _ =>
            {
                foreach (var r in roster.Values)
                {
                    r.Rounds.Clear();
                    r.curKills = r.curHeadshots = r.curDied = r.curMvp = 0;
                }
            };
            demo.Source1GameEvents.PlayerDeath += e =>
            {
                var victim = RecFor(e.Player);
                if (victim != null) victim.curDied = 1;
                var attacker = RecFor(e.Attacker);
                if (attacker != null && attacker != victim)
                {
                    attacker.curKills++;
                    if (e.Headshot) attacker.curHeadshots++;
                }
            };
            // round_mvp (when present - GOTV demos often omit it) fires right after
            // round_end, so patch the round we just committed.
            demo.Source1GameEvents.RoundMvp += e =>
            {
                var r = RecFor(e.Player);
                if (r != null && r.Rounds.Count > 0) r.Rounds[^1][5] = 1;
            };
            // round_officially_ended is not emitted in many GOTV demos, so commit on
            // round_end (which always fires once per scored round and carries winner).
            demo.Source1GameEvents.RoundEnd += e =>
            {
                int w = (int)e.Winner;                  // CS team numbers: 3 = CT, 2 = T
                int winnerTeam = w == 3 ? 0 : (w == 2 ? 1 : -1);
                foreach (var r in roster.Values)
                {
                    int side = r.Team; // 0 CT / 1 T (side held this round)
                    if (side >= 0) r.FinalTeam = side; // remember the last scored-round side
                    int won = (side >= 0 && side == winnerTeam) ? 1 : 0;
                    r.Rounds.Add(new[] { r.curKills, r.curHeadshots, r.curDied, won, side, r.curMvp });
                    r.curKills = r.curHeadshots = r.curDied = r.curMvp = 0;
                }
            };
        }

        try
        {
            await reader.ReadAllAsync(cts.Token);
        }
        catch (OperationCanceledException)
        {
            // Expected in names-fast mode: we have what we came for.
        }

        // In names-fast mode the parse stopped early, so duration/scores would be
        // truncated - report 0 and let the caller use the sidecar / .dem header.
        int durationSeconds = 0;
        int ctScore = 0, tScore = 0;
        if (!namesFast)
        {
            float tickInterval = demo.ServerInfo?.TickInterval ?? (1f / 64f);
            if (tickInterval <= 0f) tickInterval = 1f / 64f;
            durationSeconds = (int)Math.Round(demo.TickCount.Value * tickInterval);
            ctScore = demo.TeamCounterTerrorist?.Score ?? 0;
            tScore = demo.TeamTerrorist?.Score ?? 0;
        }

        string map = demo.ServerInfo?.MapName ?? "";

        // Only real players, sorted by score desc so the scoreboard reads like CS2.
        var recs = new List<Rec>();
        foreach (var r in roster.Values)
            if (r.Team == 0 || r.Team == 1)
                recs.Add(r);
        recs.Sort((a, b) => b.Score.CompareTo(a.Score));

        var rows = new List<object>();
        foreach (var r in recs)
        {
            uint accountId = r.SteamId > SteamId64Base ? (uint)(r.SteamId - SteamId64Base) : 0;
            // Report the side the player FINISHED the match on (last scored round),
            // not whatever side they were on in the post-match GOTV wind-down.
            int reportTeam = r.FinalTeam >= 0 ? r.FinalTeam : r.Team;
            var perRound = new List<object>(r.Rounds.Count);
            foreach (var rr in r.Rounds)
                perRound.Add(new { k = rr[0], hs = rr[1], d = rr[2], w = rr[3], side = rr[4], mvp = rr[5] });
            rows.Add(new
            {
                name = string.IsNullOrEmpty(r.Name) ? "[unknown]" : r.Name,
                steamId = r.SteamId.ToString(),
                accountId,
                team = reportTeam,
                k = r.Kills,
                a = r.Assists,
                d = r.Deaths,
                score = r.Score,
                mvps = r.Mvps,
                perRound
            });
        }

        var result = new
        {
            ok = true,
            v = SchemaVersion,
            namesOnly = namesFast,
            map,
            durationSeconds,
            rounds = ctScore + tScore,
            teamScore0 = ctScore,
            teamScore1 = tScore,
            hasScoreboard = rows.Count > 0,
            players = rows
        };

        return JsonSerializer.Serialize(result);
    }
}
