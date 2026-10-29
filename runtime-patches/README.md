# runtime-patches

This game's changes to the submodules, applied by `tools/apply_runtime_patches.sh` on top of N64Recomp-browser's
patches, in order of name. A new change is a new file per submodule, numbered after that submodule's highest, with a
line here.

```
N64ModernRuntime-0001  one AI event per buffer played
N64ModernRuntime-0002  set_vi_rate for frame rates above 60; high-resolution VI timer on Windows
RecompFrontend-0001    built-in texture packs (the hi-res font and title)
RecompFrontend-0002    the Rewind input
RecompFrontend-0003    multiplayer mode: a controller joins as the next player on its first button press, the keyboard
                       stays with player 1, and a new controller's profile copies the single-player bindings
RecompFrontend-0004    a controller that disconnects and comes back is its player's again; the log names each controller
RecompFrontend-0005    a saved controller is matched to the connected one again (loaded with no hash, none ever was), so
                       its profile's bindings come back each session; the log says if a controller is known or new
plume-0005             a query pool read of only the queries written (Vulkan calls a read with an unwritten one not ready)
rt64-0001              viewports with left and right edge origins (split screen); fill-rect fix
rt64-0003              GPU timings for --gpu-profile, split into the parts of each pass (none read on the web)
rt64-0004              one barrier call around all of a frame's buffer uploads, not one per uploader
rt64-0005              every slot of the texture set holds a live texture: the blank tile texture in a new set's slots
                       and an evicted texture's, which eviction marks changed (a tile whose texture missed the cache
                       read slot 0, never written or left on a deleted texture)
rt64-0006              a fill rect's clear clamped to its target (a rect ending on the scissor's edge is a pixel wider)
rt64-0007              the workload queue's size set by the game (WORKLOAD_QUEUE_SIZE on the rt64 target); the page's is 16
```
