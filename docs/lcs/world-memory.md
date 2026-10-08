# LCS world memory (a PSP-2000 partition)

Setting `WorldMemory` in `lcs.ini` (default off; the debugger's status line has a box for it, and
it takes a restart). The seam is `InitMemorySizeForGame` in `Core/PSPLoaders.cpp`, which hands
ULUS10041 v3.00 a 64 MB PSP-2000 partition instead of the 32 MB one. Read straight from the file
(`LCS::WantsExtraWorldMemory`), because the partition is sized while the ISO loads, before the layer
activates.

## Why it takes no game patch

VCS's draw distance turned out to be decided by its streaming heap and nothing else (four distance
levers in that game decide nothing; see the VCS fork's `docs/vcs/draw-distance.md`), and VCS needed
its reserve constant patched to grow the heap. LCS v3.00 already has the PSP-2000 path in its retail
code:

- `MainMemoryManager` (`0x08aa4af8`) takes `sceKernelMaxFreeMemSize()` less a reserve, the word at
  `0x08b5666c`: 5 MB as shipped, **12 MB when more than 22 MB is free**;
- `StreamingHeap` (`0x08a0b794`) is built from what is left, less 512 KB. Its object is at
  `0x08b89870`: the size at +0 (`0x08aa324c` stores it), and +0x14 moves with what is loaded.

So a bigger partition is all it takes; the game sizes its own heap from the answer.

## Measured (new game, the same 20 s walk down the first mission's park road, two boots)

| | reserve | streaming heap | in use while walking |
|---|---|---|---|
| retail 32 MB | 4.90 MB | 4.48 MB | 4.00 - 4.26 MB (90 - 95% full) |
| `WorldMemory` | 12.00 MB | 11.47 MB | 4.50 - 4.78 MB |

The retail heap runs as full as VCS's did (95%), and with room the streamer keeps about 0.5 MB (12%)
more resident on the same walk. The screenshots at the same moment of both walks are identical: the
same buildings, the same distant city. Whatever it changes is in what has to be evicted and
re-streamed (fast driving, turning round), which a still frame doesn't show and which isn't
measured yet.

## The cost

A savestate from one partition size will not load under the other, so it is opt-in: every state
made so far (and the test fixtures) is on 32 MB. Game saves are unaffected.

## Not measured

- Pop-in and streaming hitches while driving fast, with and without it (the instrument VCS used is
  the distribution of frame intervals while streaming; see its `streaming-stutter.md`).
- `CacheFullIsoInRam`, the host-side half of VCS's streaming-stutter fix, is a stock PPSSPP setting
  and wasn't measured here either.
