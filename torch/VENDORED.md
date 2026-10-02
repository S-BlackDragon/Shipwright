# Torch, vendored into this repository

This folder is a plain copy of Torch (the asset extractor that builds `oot.o2r` from the ROM), not a git submodule,
for the same reason as `libultraship/` (see `libultraship/VENDORED.md`): the project must keep building for years
without depending on somebody else's repository still being there.

- Origin: https://github.com/HarbourMasters/Torch (branch `main`)
- Commit copied: `2ab12fe9660aec04e02ee89fe81baed304a1a1d6` ("Remove redundant copies from the asset parse path (#252)", 2026-08-12)
- Git tree of that commit: `2611f0a521e6c60d18dc8b52316512db03e0f36c`. The commit that added this folder holds exactly that tree plus this file.
- License: see `LICENSE` in this folder (unchanged).

## Rules

- The repository's formatter (`run-clang-format.ps1`) only touches `soh/`; nothing reformats this folder.
- Every change is listed below with the commit that made it and marked in the code with a `// ZMP:` comment.

## Changes made here (newest last)

None: this is the unchanged copy.
