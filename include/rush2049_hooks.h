#ifndef RUSH2049_HOOKS_H
#define RUSH2049_HOOKS_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hooks inserted into recompiled code by us.toml ([[patches.hook]]). */
void rush2049_inflate_begin(uint8_t* rdram, recomp_context* ctx);
void rush2049_inflate_end(uint8_t* rdram, recomp_context* ctx);

/* Before the scheduler's swap throttle (us.toml): retraces since the last swap, as the game should see them. */
gpr rush2049_frame_pace(gpr retraces);
/* On every retrace, before the scheduler forwards it to its clients (us.toml): sets the VI rate and keeps the
   clients at 60 retraces a second. */
void rush2049_retrace_clients(uint8_t* rdram, recomp_context* ctx);
/* Input lag (us.toml): after the main thread queues a graphics task, and in the scheduler's check of the VI's next
   framebuffer ($t7) against the current one ($s0) before it starts one. */
void rush2049_task_queued(uint8_t* rdram);
gpr rush2049_task_framebuffer(gpr next_framebuffer, gpr current_framebuffer);

/* Widescreen race HUD (src/widescreen.cpp): around every texture rectangle, when the race timer is drawn, and
   before a graphics task starts (turns on RT64's extended GBI for that display list). */
void rush2049_hud_rect_begin(uint8_t* rdram, recomp_context* ctx);
void rush2049_hud_rect_end(uint8_t* rdram);
void rush2049_hud_race_mark(uint8_t* rdram);
/* At the start of the music thread's retrace tick (us.toml): nonzero while another retrace is already queued for the
   thread, and the tick should return (src/music_thread.cpp). */
int rush2049_music_tick_behind(uint8_t* rdram);
void rush2049_enable_extended_gbi(uint8_t* rdram, uint32_t task_addr);

/* Before the scheduler starts a task (us.toml): upscale any font texture the display list draws that the hi-res
   font pack doesn't have yet (src/hires_font.cpp). */
void rush2049_hires_font_scan(uint8_t* rdram, uint32_t task_addr);

/* Before the scheduler starts a task (us.toml); writes it out when --dump-dl is due. */
void rush2049_dump_task(uint8_t* rdram, uint32_t task_addr);

/* Draw Distance and Level of Detail (src/draw_distance.cpp, us.toml): the LOD cutoff and level choice in func_8009C8F0,
   the far plane in func_8009F058 and the prim depth that follows it in func_8008A644, the split-screen cull distance in func_8009DD88, and the 2048-unit test of the node
   matrix builders func_8009D99C and func_8009D45C, and the 1600-unit depth clamp of the far one, func_8009D708. */
float rush2049_draw_distance_lod(float limit);
float rush2049_draw_distance_level(float switch_distance);
float rush2049_draw_distance_far(float far, float near);
gpr rush2049_draw_distance_prim_depth(gpr z);
float rush2049_draw_distance_cull(float cull);
void rush2049_far_matrix_begin(recomp_context* ctx);
void rush2049_far_matrix_end(uint8_t* rdram, gpr matrix);
float rush2049_far_clamp_distance(float d);
void rush2049_far_clamp_fit(recomp_context* ctx);

/* In busy-wait loops (us.toml): let the next ready game thread run, whatever its priority (src/spin_yield.cpp). */
void rush2049_spin_yield(uint8_t* rdram);

/* Rewind (src/rewind.cpp): around each 60 Hz race tick in thread 5, and in the frame loop (thread 7) after its wait
   for a frame and before it draws a race frame. */
void rush2049_rewind_tick_begin(uint8_t* rdram);
void rush2049_rewind_tick_end(uint8_t* rdram);
void rush2049_rewind_frame_begin(uint8_t* rdram);
void rush2049_rewind_frame_draw(uint8_t* rdram);

/* Save Ghosts (src/controller_pak.cpp): at the end of a Ghost Race, before the game checks whether to offer
   SAVE GHOST. */
void rush2049_ghost_save_check(uint8_t* rdram);
/* Save Ghosts: whether the SAVE GHOST prompt's per-pak check takes a ghost as this profile's, in place of the game's
   answer (v0, 0 for a match). */
void rush2049_ghost_prompt_match(uint8_t* rdram, recomp_context* ctx);

/* Controller Accessory at Both (src/controller_pak.cpp): whether the game's motor routines see a Rumble Pak in a port,
   given the game's own answer. */
gpr rush2049_rumble_pak_in(gpr rumble_pak);

/* Debug tracing for bring-up: prints "<ms> <tag> <value>" to stderr. */
void rush2049_trace(const char* tag, uint32_t value);
void rush2049_probe_sched_msg(uint8_t* rdram, uint32_t msg);

#ifdef __cplusplus
}
#endif

#endif
