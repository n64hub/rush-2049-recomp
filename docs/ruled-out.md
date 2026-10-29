# Ruled out, so don't retry

What has been tried or measured and dropped, with why.

- AI upsampling (AudioSR) of the MusyX mix: inaudible in a headphone A/B. The mix is close to mono (L/R correlate
  0.99).
- A headphone fold of the surround channel: the game only offers Stereo and Mono, and the channel reads 0.
- Fixing the street lamp drawn behind the sailboats' sails at Track 1's start. One scenery display list draws the lamps
  (a textured quad each) and then the boats, all in render mode C8104A50 (Z compare, no Z update), so a sail drawn
  after a lamp passes its depth test and covers the pole. That is the game's own order, and an N64 does the same.
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
