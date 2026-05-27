# vtm-tile configuration loading

This document describes how `vtm-tile` loads its configuration, how it crosses
process boundaries, and what reaches the per-pane terminal children. For the
general settings file format and merge algorithm see
[`settings.md`](settings.md).

## Sources and precedence

Every `vtm-tile` process (the server fork, a `vtm-tile -r term` pane child,
`--list-config`, etc.) builds its configuration by calling
`app::shared::load::settings` at startup
(`src/netxs/desktopio/application.hpp:2312-2383`). Four input documents are
collected and overlaid on top of the compiled-in defaults:

| Order | Source                          | Where it comes from                                          |
| ----- | ------------------------------- | ------------------------------------------------------------ |
| 0     | `defcfg` (defaults)             | `src/vtm.xml` embedded at compile time.                      |
| 1     | files in `<include …/>` list    | Each source's `<include>` entries; the defaults already list `/etc/vtm/settings.xml` and `~/.config/vtm/settings.xml` (`src/vtm.xml:1-3`). |
| 2     | `envcfg`                        | `$VTM_CONFIG` — file path, inline `<…>` XML, or `:memhandle`. |
| 3     | `dvtcfg`                        | The DirectVT marker payload from a parent process (`os::dtvt::config`). |
| 4     | `clicfg` (`-c`)                 | The `-c`/`--config` argument — file path, inline `<…>`, or `:memhandle`. |

The merge is unconditional and last-write-wins:

```
overlay_config(defcfg, <each included file>);   // application.hpp:2369-2375
overlay_config(defcfg, envcfg);                 // application.hpp:2377
overlay_config(defcfg, dvtcfg);                 // application.hpp:2378
overlay_config(defcfg, clicfg);                 // application.hpp:2379
```

So `-c` wins over everything; `$VTM_CONFIG` wins over included files; included
files (including the user's `~/.config/vtm/settings.xml`) win over the embedded
defaults.

## Process lifecycle

```
  shell user
     │
     │  $ vtm-tile [-c …]                   (Client)
     ▼
  ┌──────────────────────────────┐
  │ Client process               │
  │  load::settings(config,      │
  │                 cliopt)      │
  │  Connects to server pipe.    │
  │  Forks the server if absent. │
  └──────────────┬───────────────┘
                 │ fork() + serialised config
                 ▼
  ┌──────────────────────────────┐
  │ Server process (Tile applet) │
  │  indexer.config = merged cfg │
  │  app::tile::hall created.    │
  └──────────────┬───────────────┘
                 │  per pane:
                 │   app_type="dtvt", cmd="$0 -r term"
                 │   build_dtvt → run_dtvt_app
                 │   sends DirectVT marker (appcfg.cfg is empty)
                 ▼
  ┌──────────────────────────────┐
  │ Pane child: vtm-tile -r term │
  │  os::dtvt::initialize() reads│
  │   marker → dtvt::config      │
  │  load::settings(indexer.config, "")
  │  build_terminal in-process.  │
  └──────────────┬───────────────┘
                 │ pty fork+exec
                 ▼
              shell program
```

### Client → server fork

The first `vtm-tile` invocation acts as a client (`src/vtm-tile.cpp:565-606`).
It loads its config, then if no tile server is listening on the user's pipe it
forks one via `os::process::fork(system, prefix_base, config.settings::utf8())`
(`src/vtm-tile.cpp:575`, fork body at `src/netxs/desktopio/system.hpp:2841`).

The server thus has the same merged config the client computed:

* **Linux/macOS** — the grandchild inherits the parent's `config` via COW; the
  serialised `config_utf8` argument is unused (`system.hpp:2876-2900`). After
  the fork the server thread calls `indexer.config.swap(config)`
  (`vtm-tile.cpp:644`).
* **Windows** — a fresh `vtm-tile -s -p <prefix> -c :<memhandle>` process is
  spawned (`system.hpp:2862-2870`). It re-enters `main`, parses `-c :…`, and
  reloads the merged config from shared memory; `clicfg.load` is the
  `:memhandle` branch in `application.hpp:2354-2359`.

Either way the tile server ends up with the merged client-side config.

### Tile server → pane child

When a tile pane is created with no explicit `app_type`, the default selection
is `app_type="dtvt"` and `cmd="$0 -r term"`
(`src/netxs/apps/tile.hpp:2087-2088`). The server builds a `ui::dtvt` host
(`src/netxs/apps.hpp:399-426`), which spawns `vtm-tile -r term` as a child via
`vtty::run_dtvt_app` (`src/netxs/desktopio/system.hpp:4484-4543`). On the way
it sends a `directvt::binary::marker{ appcfg.cfg.size(), initsize }` followed
by `appcfg.cfg`.

The child reads the marker in `os::dtvt::initialize`
(`system.hpp:3986-4055`), stashes the payload in `os::dtvt::config`, and on the
next `load::settings` call it appears as `dvtcfg`.

**However:** in vtm-tile the menu-item parser at `tile.hpp:2070-2093` only
reads `id`, `type`, `cmd`, `title` from each item. It never sets
`appcfg.cfg` — unlike the full-vtm path at `src/vtm.hpp:1089-1126` which reads
both `cfg="…"` attributes and nested `<config>…</config>` patches. So the
DirectVT marker the child receives carries a **zero-length** config blob, and
the child's `dvtcfg` is empty.

The child therefore reloads its configuration from scratch:

```
defcfg ← embedded vtm.xml                         (always)
includes ← /etc/vtm/settings.xml,                 (via defcfg <include>)
           ~/.config/vtm/settings.xml
envcfg ← $VTM_CONFIG                              (inherited via env)
dvtcfg ← (empty)                                  (tile leaves appcfg.cfg empty)
clicfg ← cliopt                                   (only what's in the cmd line)
```

### Pane child → shell

The terminal widget itself is in-process inside the pane child
(`build_terminal` at `src/netxs/apps/term.hpp:130-…`). The shell program is
spawned beneath the widget by `ipccon.runapp` →
`vtty::create`/`runapp`/`create_dtvt_process`
(`system.hpp:4633` and `system.hpp:4354`). The shell receives `appcfg.cmd /
env / cwd` only — no XML config.

## Propagation truth table

What reaches each process started by `vtm-tile`:

| Source                                  | Client | Server (fork) | Pane child (`vtm-tile -r term`) | Shell |
| --------------------------------------- | ------ | ------------- | ------------------------------- | ----- |
| Embedded `vtm.xml` defaults             | yes    | yes           | yes                             | no    |
| `/etc/vtm/settings.xml` (`<include>`)   | yes    | yes           | yes                             | no    |
| `~/.config/vtm/settings.xml` (`<include>`) | yes | yes           | yes                             | no    |
| `$VTM_CONFIG`                           | yes    | yes           | yes (env inherited)             | no    |
| `-c` on client cmd line                 | yes    | yes (via fork)| **no**                          | no    |
| Per-menu-item `<config>` patch          | n/a    | n/a (ignored) | **no** (tile.hpp doesn't wire it) | no  |
| DirectVT marker payload (`appcfg.cfg`)  | n/a    | n/a           | yes if non-empty                | no    |

The two surprising rows are the `-c` and per-menu-item ones. Both reach the
server but stop there.

## Practical implications

`/config/tile/*` is read by the server; `/config/terminal/*`,
`/config/colors/*`, `/config/timings/*`, etc. are read in the pane child where
the term widget actually lives. So if you start the server with
`vtm-tile -c "<config><terminal><confirm_close=false/></terminal></config>"`,
the server itself reads (and ignores) that subtree; the spawned terminals
re-load from defaults and never see your override.

Ways to make terminal-side settings reach the panes:

1. **`$VTM_CONFIG`** — inherited via the process environment across every
   spawn, so it lands in both the server and the pane children. This is the
   simplest knob for ad-hoc overrides:
   ```sh
   VTM_CONFIG='<config><terminal><confirm_close=false/></terminal></config>' vtm-tile
   ```
2. **`~/.config/vtm/settings.xml`** — included by every `vtm-tile` process via
   the default `<include>` list. Right place for persistent user settings.
3. **Encode it in the menu item's `cmd`** — make each pane spawn carry its own
   `-c`:
   ```xml
   <item id="term" type="dtvt"
         cmd="$0 -c '<config><terminal><confirm_close=false/></terminal></config>' -r term"/>
   ```
   This works because `utf::tokenize` (`src/netxs/desktopio/utf.hpp:2456-2467`)
   respects single quotes, so the inline XML survives as a single argv element.

## Reference: key source locations

* `src/vtm-tile.cpp:234-706` — main entry, role dispatch, fork sites.
* `src/netxs/desktopio/application.hpp:2312-2383` — `load::settings` merge
  algorithm.
* `src/vtm.xml:1-3` — default `<include>` list.
* `src/netxs/desktopio/system.hpp:2841-2903` — `os::process::fork` (client →
  server).
* `src/netxs/desktopio/system.hpp:3986-4055` — `os::dtvt::initialize` (reads
  the marker and populates `os::dtvt::config`).
* `src/netxs/desktopio/system.hpp:4484-4543` — `vtty::run_dtvt_app` (sends the
  marker + `appcfg.cfg` to a dtvt child).
* `src/netxs/desktopio/terminal/dtvt.hpp:330-379` — `ui::dtvt::start_dtvt`.
* `src/netxs/apps/tile.hpp:2070-2093` — tile pane menu-item lookup (does not
  populate `appcfg.cfg`).
* `src/netxs/apps/tile.hpp:86-104` — `expand_appcfg` (currently only rewrites
  `$0` in `cmd`/`env`; the `appcfg.cfg` patch is commented out).
* `src/netxs/apps.hpp:399-562` — `build_dtvt`, `build_term`, `build_vtty`.
* `src/netxs/apps/term.hpp:86-…` — `build_teletype` / `build_terminal`
  (in-process term widget builders).
* `src/vtm.hpp:1089-1126` — for contrast, the full-vtm menu-item path that
  *does* read per-item `cfg=`/`<config>` and stuffs it into `appcfg.cfg`.
