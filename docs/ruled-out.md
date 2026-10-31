# Ruled out, so don't retry

What has been tried or measured and dropped, with why.

- AI upsampling (AudioSR) of the MusyX mix: inaudible in a headphone A/B. The mix is close to mono (L/R correlate
  0.99).
- A headphone fold of the surround channel: the game only offers Stereo and Mono, and the channel reads 0.
- Sorting every see-through draw by depth, the effects' too (each call by its triangles' mean screen depth, and the
  triangles inside a call): it turned the layers of an effect over, the exhaust flame's glow went under its core.
  Moving the see-through draws that the root display list calls itself (the player's car, its flame and the flame's
  trail, smoke and the other effects) as well draws the orange flame over its nozzle's glow, so only those two lists
  deep (scene nodes) move, and only those are sorted.
- Drawing the moved see-through scenery far to near call by call (each call by its triangles' mean depth, or each scene
  node's calls together by the node's): it changes nothing seen. A call holds many pictures (one scenery list draws a
  row of street lamps in one call and the sail boats in the next, all in render mode C8104A50, Z compare without Z
  update), so a far sail drawn after a nearer lamp or sail still covers it. In the first 12 seconds of a Track 1
  race, of the overlapping see-through pictures drawn near before far, about 2,200 pairs were inside one call and
  3,900 across two, and each call holds pictures near and far, so no order of whole calls puts them right. They are
  sorted picture by picture instead (runtime-patches/rt64-0011).
- Making Scenery Culling's Off draw less where it loses frames. At the foot of Track 1's downhill, where it meets the
  loop, looking back the way the track comes (GitHub #267), Off draws 1366 calls a frame, Reduced 1108 and Original
  639 (1600x900), all at about the same GPU time: the cost is RT64's CPU work per call. 78 of the track's 117 scenery
  objects are in view there, and hiding them one at a time, about 18 of those change no pixel, being behind closer
  buildings. The view test (runtime-patches/rt64-0008) can't tell them, and nor can the game's table: a reach long
  enough to leave them out (taking every stretch within 1500 units instead of all of them) leaves out a tower in sight
  too. Skipping them needs an occlusion test before the call is sent, which is where the cost is. Reduced is the
  cheaper setting (`src/draw_distance.cpp`).
- Caching texture hashes while TMEM is untouched: only 8% of hashes repeat without a TMEM change.
- Merging or batching RT64's draws to cut the 240p floor (`start_gpu_profiler`'s table in `src/main.cpp`). Only ~5% of
  draws repeat the one before exactly; merging every run that shares a pipeline (wrong textures, timing only) halves
  the ~500 calls and the draws part moves less than ±4%. With a 1x1 viewport (no pixels) ~90k cycles stay, and the
  ubershader, which cuts pipeline binds from 80% of draws to 7%, doesn't lower them; it doubles the draws part with
  pixels on. Draw calls and pipeline binds are not the cost. Only the upload barriers were overhead.
- A cheaper VI pass. Its ~71k in a 1920x1080 window are ~10k of render-pass start and clear (a driver fast clear;
  clearing only the bars saves 1-2k), ~50k filling the window once (the same triangle in a flat colour, about 40 pixels
  a clock) and ~10k for its one bilinear tap of the frame (320x240 at 240p). The gamma `pow` measures nothing. Only a
  smaller window shrinks it.
- Inserting `gEXSetRectAlign` before HUD rects: callers keep their own display list pointer and overwrite it. Rewrite
  commands in place instead.
- Stretching the menu background: the emblem goes oval and the circuit lines smear. It is mirrored instead.
- Lowering RT64's swapchain frame latency (2 on Vulkan, 1 on D3D12) for input lag: in a window a presented frame
  reached the compositor 0.6 ms later with either value, so nothing queues there. Fullscreen wasn't measured. What did
  queue was the game's own scheduler (`frame_rate.cpp`, the Input Lag option).
- SDL 2's resamplers for the output: `SDL_ConvertAudio` pads buffers, `SDL_AudioStream` drops fractional frames.
