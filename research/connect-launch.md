# Direct-connect argument correction

Static analysis of executable SHA-256
`cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`
shows that an OS command line containing `-connect_steam_id` does not by itself
prove a connection attempt. The earlier server.md log observation
`Contraption::start -connect_steam_id ...` is insufficient evidence.

## Actual initialization path

At RVA `0x2E35CA..0x2E35F1`, game initialization obtains the Steam Apps context
and calls vtable offset `0xD0`, passing a 1024-byte output buffer. The result is
logged as `Contraption::SteamCommandLine ` (literal RVA `0x14C1660`). This is the
Steam `GetLaunchCommandLine` path. The raw application launch string is logged
earlier under `Contraption::start ` (literal `0x14C1648`) and is a separate input.

At `0x2E367E` the **Steam buffer** is passed to parser `0x2F1C90`. Register outputs
are initialized to zero: RDX points to connect Steam ID, R8 to friend Steam ID,
R9 to secret. On success, initialization stores connect Steam ID at `game+0x88`
and secret at `game+0x90` (`0x2E368C`, `0x2E3697`). Friend ID is used to query
Steam rich presence. The later bootstrap branch requires options mode
`[options+0x60] == 0` and nonzero `game+0x88`, then enters state/bootstrap helper
`0x474240` at `0x2E3AE0`. Other special application modes bypass this branch.

## Parser behavior

Function `0x2F1C90` tokenizes a whitespace-separated stream. The spelling is
checked through immediate constants, so the parser has no direct xref to the
standalone rich-presence literal `-connect_steam_id `.

At `0x2F1E97..0x2F1ED3`, the exact 17-byte token `-connect_steam_id` consumes the
following unsigned 64-bit numeric value. `-friend_steam_id` and `-secret` consume
their own values. Unknown tokens are skipped. Repeated recognized tokens write
the same outputs; the last successfully parsed value wins. A malformed numeric
argument follows a parse-error path and returns false. End-of-input returns true
only when the connect ID is nonzero (`0x2F2047..0x2F2051`, `0x2F2160`).

The literal `Failed to parse -connect_steam_id <id>` at `0x14C1B68` is referenced
from this parser at `0x2F2065`. The other main references to the standalone
`-connect_steam_id ` literal (`0x38DB84`, `0x467504`) build outgoing rich-presence
join strings; those references are not the startup parser.

## Correct launch experiment

Valve documents that `ISteamApps::GetLaunchCommandLine` supplies the launch
payload from Steam URLs, separately from OS arguments. A cold launch using a URI
of this form is therefore the appropriate next experiment:

```text
steam://run/387990//-connect_steam_id%20STEAMID64/
```

This URI is a proposed test, not a successful join claim. Inspect the
`Contraption::SteamCommandLine` log and verify that it contains the intended ID;
then correlate SNS callbacks and captured packets. OS launch options appended by
Steam may still be present in `Contraption::start` without appearing in this
Steam buffer. If duplicate connect arguments enter the Steam buffer itself,
the parser's last-value behavior applies.

For an already-running application, Steam emits `NewUrlLaunchParameters_t` for
such URL changes. Whether this build registers and processes that callback has
not been established; use a cold launch first. [Valve's ISteamApps reference](https://partner.steamgames.com/doc/api/ISteamApps#GetLaunchCommandLine)
documents the API and callback behavior.

The bounded E8 scan found two direct calls to `0x2F1C90`: initialization at
`0x2E367E` and callback code at `0x2F19A7` inside `0x2F1960`. The callback consumes
a string at callback payload `+8`, compares payload Steam ID `+0` with the parsed
friend ID, then dispatches according to application state. This shape matches
`GameRichPresenceJoinRequested_t`; it does not establish NewUrl handling. In
state 3 it calls `0x445740`; in state 2 it queues new connect/secret fields.
`connect-callback-disassembly.txt` retains this evidence. No remote callback
invocation was attempted: UI/state handlers may require the game's own thread.

## Reproduction

Run `tools/probe/inspect_binary.py` with `--disassembly-output`; the report now
includes the parser and initialization function ranges and the relevant log/error
string xrefs. `connect-parser-disassembly.txt` retains the focused disassembly
used for this finding; `connect-disassembly.txt` retains the rich-presence string
construction functions. These are ordinary offline shell observations, not
live execution evidence or REA Evidence records.
