# Demo Event Pre-scan — `.fmjson` v6

The Follow camera's **Events** list (weapon/C4 drops + pickups) comes from a one-time demo
pre-scan, cached next to the demo as `<demo>.dem.fmjson`. CS2 demos have no random access
to game events, so the tool parses the whole demo once (in the Go helper) and reads the
cached JSON thereafter.

## Schema version

`schemaVersion` / `kSchemaVersion` = **6**. The writer and reader must agree:

- Go writer: [main.go:35](../../FilmmakerDemoInfoGo/main.go:35) (`const schemaVersion = 6`)
- C++ reader: [DemoInfoHelper.cpp:20](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp:20) (`constexpr int kSchemaVersion = 6;`)

A cached file with a different `v` is treated as stale and re-parsed
([DemoInfoHelper.cpp:281](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp:281)). **If you
change the schema, bump both constants together.**

> The top-level `"events"` array was added in v5; v6 switched the parser to
> demoinfocs-golang. A cache with an older `v` auto-regenerates on next read (no manual
> migration).

## Event JSON shape

The `.fmjson` carries a top-level array
([main.go:374](../../FilmmakerDemoInfoGo/main.go:374)):

```json
"events": [
  { "tick": 1234, "type": "weapon_drop",  "item": "",          "accountId": 123456 },
  { "tick": 1250, "type": "bomb_dropped",  "item": "c4",        "accountId": 234567 },
  { "tick": 1300, "type": "bomb_pickup",   "item": "c4",        "accountId": 345678 },
  { "tick": 1400, "type": "item_pickup",   "item": "weapon_ak47","accountId": 123456 }
]
```

| Field | Meaning |
|---|---|
| `tick` | the in-game tick when the event fired — the seek target. |
| `type` | `weapon_drop` · `bomb_dropped` · `bomb_pickup` · `bomb_planted` · `item_pickup`. |
| `item` | e.g. `weapon_ak47`, `c4`, or `""`. |
| `accountId` | Steam account id (32-bit) of the player involved; `0` = unknown. |

### Event sources (Go)

Registered with `p.RegisterEventHandler` during the full parse,
[main.go:231–307](../../FilmmakerDemoInfoGo/main.go:231):

| Recorded type | demoinfocs event | Notes |
|---|---|---|
| `weapon_drop` | `events.Kill` | CS2 has **no** `weapon_drop` game event, so the victim's death tick is used as the dominant drop moment. |
| `item_pickup` | `events.ItemPickup` | Skipped during freezetime (the automatic round-start weapon grants), so only real mid-round ground pickups are kept. |
| `bomb_dropped` | `events.BombDropped` | Player via `e.Player`. |
| `bomb_pickup` | `events.BombPickup` | Player via `e.Player`. |
| `bomb_planted` | `events.BombPlanted` | Player via `e.Player`. |

`addEvent` ([main.go:182](../../FilmmakerDemoInfoGo/main.go:182)) stamps each with
`p.GameState().IngameTick()` and the account id.

## C++ side

- `DemoEvent { int tick; std::string type; std::string item; uint32_t accountId; }`
  ([DemoEntry.h:38](../../AfxHookSource2/Filmmaker/Demo/DemoEntry.h:38)); `DemoEntry` /
  `DemoHelperResult` carry a `std::vector<DemoEvent> events`.
- Parsed from the `"events"` array in
  [DemoInfoHelper.cpp:229](../../AfxHookSource2/Filmmaker/Demo/DemoInfoHelper.cpp:229).
- The Follow camera loads them lazily on a background thread keyed on the engine's
  `GetDemoFilePath()`
  ([FollowCamera.cpp:1290 `EnsureEventsLoaded`](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1290)),
  resolving `accountId → ownerName` from the scoreboard. The state JSON ships the events
  nearest the playhead (capped at 60) so a long demo doesn't flood every frame
  ([FollowCamera.cpp:1232](../../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp:1232)).

## Regenerating a cache

Delete the cache file and re-open the demo (or select a Weapon target, which triggers the
background load):

```bat
del "<path-to-demo>.dem.fmjson"
```

The helper exe must be the freshly built one — it ships to
`build\staging-release\bin\x64\FilmmakerDemoInfo\FilmmakerDemoInfo.exe` via `build.bat`
(the `go build` step). The Go helper always does a full parse, so the event list is always
populated.

