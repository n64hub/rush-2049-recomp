// The race HUD's position, at the Race Position setting. func_80107AF4 draws each player's "3/5" only while
// func_800CF604 says their car is active: not wrecked, and not in the 5 seconds after it is put back on the track.
// func_800D3B28 clears the player's flag at +0x7EB of their block (0x8014A250, 0x808 a player) when the car is put
// back, and func_800E56F8 sets it again once 5 seconds of the race have passed. Each crash took the number off the
// screen for 7-8 seconds, and in a 4-player race it went missing from one view or another now and then. At Always a
// hook (us.toml) has the HUD take every car as active; the rest of the game still asks func_800CF604 and sees the car
// as the game set it. A player who has finished still has no position drawn, which the HUD checks apart.

#include "recomp.h"
#include "rush2049_game.h"
#include "rush2049_hooks.h"

extern "C" gpr rush2049_race_position_shown(gpr car_active) {
    return rush2049::race_position_always() ? 1 : car_active;
}
