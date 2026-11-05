# Server data importer

`tools/importer/import_data.py` selects candidate server assets from a local
licensed Scrap Mechanic installation. It uses only Python 3.12 standard-library
modules. Inventory is the default and does not write files:

```powershell
python tools/importer/import_data.py --game 'D:\Steam\steamapps\common\Scrap Mechanic'
python tools/importer/import_data.py --game 'D:\Steam\steamapps\common\Scrap Mechanic' --output artifacts/data-pack --copy
python -m unittest discover -s tests -p test_importer.py
```

The importer requires the executable SHA-256 recorded in `server.md`:
`cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`.
`--allow-unknown-build` explicitly permits another build and records
`known_build: false`; it does not establish compatibility.

Selection scans `Data`, `Survival`, `ChallengeData`, `Challenges`, and
`Cache/Bundle`. Lua, JSON, collision OBJ, tile/tileson, shapeset and physics
materials are server-data candidates. Mesh, export and rend files are included
as uncertain dependencies. Only `*_physics.cbo` bundles are selected from cache.
The importer excludes binaries, saves, textures, audio and all other extensions.
It does not parse assets or establish dependency closure, and this pack is not
yet sufficient evidence of a working headless world.

Every copied file is recorded in `manifest.json` with its installation-relative
path, byte size, SHA-256 and classification. The manifest binds the pack to the
game executable hash. Source files are never changed. Symlinks and junctions
are rejected for roots/output paths and skipped inside the scanned tree;
case-insensitive collisions are rejected. Output cannot overlap the installation.

Copy stages files beside the output, checks their hashes, and renames the
complete directory into place. Existing packs are accepted only if their
manifest and every file match exactly; modified, missing or untracked files
cause an error. There is no implicit overwrite/update mode: choose a new output
directory for changed game data. The default inventory also hashes files, so a
run may take time even without copying.

Keep imported game data local. The distributable project contains the importer
and server source; it does not need to bundle the installation's proprietary
assets. Actual license permissions must be assessed separately.

## Local measurement, 2026-11-02

The importer ran successfully against the installed build with the expected
executable hash. It copied and verified 11,653 candidate files totaling
452,324,696 asset bytes (452.3 MB decimal, excluding the manifest) into
`artifacts/data-pack`. No links were encountered. Classification totals:

| Classification | Bytes |
|---|---:|
| Server-data candidates | 314,104,449 |
| Physics cache candidates | 9,566,833 |
| Uncertain mesh/export/rend dependencies | 128,653,414 |

This confirms that the selected candidate pack fits below 1 GB. Whether it
contains every dependency needed by a native server remains unverified.
