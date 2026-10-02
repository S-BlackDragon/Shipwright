# libultraship, vendored into this repository

This folder is a plain copy of libultraship, not a git submodule (decision of the project owner, 2026-10-02:
`docs/PETICIONES_ALEX.md` in the ZMP repository). The project must keep building for years without depending on
somebody else's repository still being there, and its fixes to this library live here, in the open, as ordinary commits.

- Origin: https://github.com/kenix3/libultraship (branch `port-maintenance`)
- Commit copied: `62e973aeb4a53ad4d22bb91e2d9373ecdfcd246c` ("Unify the pyramidlike test (#1239)", 2026-08-15)
- Git tree of that commit: `400e3eb8feab752c83cea6d32644520db55fd819`. The commit that added this folder holds exactly
  that tree plus this file: `git rev-parse <that commit>:libultraship` minus `VENDORED.md` gives the same tree.
- License: MIT, see `LICENSE` in this folder (unchanged).

## Rules

- The repository's formatter (`run-clang-format.ps1`) only touches `soh/`; nothing reformats this folder. A change here
  is as small as the fix needs, and is marked with a `// ZMP:` comment.
- Every change is listed below with the commit that made it, so that the difference from upstream is always
  `git diff <copy commit> -- libultraship` and this list.
- To take a newer upstream version: replace the folder with the new upstream tree in one commit, then re-apply the
  changes of the list (each is one small commit) and run the whole test suite.

## Changes made here (newest last)

1. **Resource table written and read without its lock** (finding T of the ZMP suite; `docs/DECISIONES.md` D-096).
   - `src/ship/resource/ResourceManager.cpp`, `LoadResourceProcess`: the write of "not found" into the resource
     cache takes the lock that guards every other access.
   - `src/ship/resource/ResourceManager.cpp`, `UnloadResource`: the lookup is made under the lock, and the entry is
     taken out of the table under the lock and destroyed after it (the code's own comment asked for that).
   - `src/ship/resource/archive/ArchiveManager.cpp`, `LoadFile` and `GetArchiveFromFile`: a lookup instead of
     `operator[]`, which added an empty entry for every file asked for and not found, on any thread and with no lock
     (after it `HasFile` said "yes" for a file that does not exist).
   - `include/ship/resource/archive/ArchiveManager.h`: `gZmpTestResourceFaults`, three bits that bring those three
     accesses back one by one, for the mutation test (`tabla_sin_cerrojo`, `descarga_sin_cerrojo`,
     `archivo_fantasma`). Nothing sets it outside a test build.
   - Test: `test_f0_first_loads_from_several_threads_write_nothing_unguarded` (ZMP harness).
2. **`Fast3dGui::mImpl` value-initialized** (finding R; D-096). `include/fast/Fast3dGui.h`. The window procedure read
   `mImpl.Backend` before anybody had set it. Test: `test_f0_start_does_not_depend_on_what_memory_held`, mutant
   `gui_sin_iniciar`.

Not changed, and known: `ArchiveManager::AddArchive`, `RemoveArchive` and `SetArchives` rewrite the file table with no
lock while other threads may be reading it. They run at start and when the player changes mods from the menu.
