# Profile favorites regression tests

Configure and build the normal native Linux `Throne` target with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run with the same compiler/Qt environment:

```sh
python3 script/tests/profile_favorites.py /path/to/build
```

The runner reuses the application's actual object files and replaces only its
startup `main`. It builds and runs two executables:

- `profile_favorites_persistence.cpp`: fresh and legacy schema, cold reads,
  transactional toggle failures, ordinary single/batch saves, stale captured
  rows, chunk boundaries, deletion, and selective/legacy backup compatibility.
- `profile_favorites.cpp`: the actual profile table, menu actions and star
  filter, multi-selection, duplicate slots, combined text filters, failed
  mutations, edit/move/clone/delete semantics, outbound export invariants, and
  real subscription reconciliation against a loopback-only synthetic fixture.

All database, application-home and output state is temporary. Qt uses its
`offscreen` platform. The GUI fixture constructs `EngineHost` without calling
`init`; no core, VPN, system proxy or external subscription is started.
The runner writes logs under `profile-favorites-tests` in the supplied build.

## Metadata contract

Favorites belong to a stored profile ID. Only `ProfilesRepo::SetFavorites`
changes them. Generic saves preserve the existing SQL value, including a batch
whose other fields were captured before a toggle. New imports, clones and IDs
recreated by clear-before-refresh start unfavorited; ordinary edits, moves and
identity-matched subscription updates retain the ID and its flag.

Favorites are excluded from outbound JSON, share links and identity keys. The
current-group filter composes with text filters and does not modify group order.
The filter starts off on each launch. A favorite does not prevent deletion.

Desktop database backups include the optional column. Older backups restore it
as false. Older desktop versions and the current Android schema do not preserve
this unsupported column through all writes or backup round trips.
