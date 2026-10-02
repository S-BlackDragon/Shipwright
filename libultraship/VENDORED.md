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

None yet: this commit is the unchanged copy.
