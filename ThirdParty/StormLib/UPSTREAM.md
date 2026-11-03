Vendored from https://github.com/ladislav-zezula/StormLib
Release v9.31, commit 2aa6216c65ce68b88c46c0e4f12a38dc94c36b7a.
Local patch: bzip2/blocksort.c uses unsigned 1U bit masks to avoid signed-shift undefined behavior under UBSan. Only CMake build inputs are included.
Used by the Android port; the Windows client retains its existing MPQ code.
See LICENSE and individual codec copyright/license notices.
