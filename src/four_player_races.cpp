// Races for 3 and 4 players. The game's split screen draws four views, and Stunt and Battle use them, but Select Mode
// offers Single Race and Practice to 1 or 2 players only: ovl1_func_80399394 looks a mode up in a table of 7 modes
// (0 Single Race, 1 Practice, 2 Ghost Race, 3 Circuit, 4 Stunt, 5 Obstacle, 6 Battle) for each player count, at
// 0x803B4679, and a hook where it returns (us.toml) offers those two to 3 and 4 players as well when the Race Players
// setting is at Up to 4. The race itself already lays out four views, places each car and, with 3 players, shows the
// track map and the clock in the fourth quarter.
//
// Computer cars. Setting up a race, the game takes the track's number of them into 0x80142724 and clears it for 3 or
// more players; the race then fills the field up to 6 cars. A second hook puts the number back, so 3 players race 3
// computer cars, as 1 and 2 players race 5 and 4. With 4 players, 2 computer cars (6 cars in each of 4 views) end a
// race some seconds in on a bad model pointer in func_80099BFC, so 4 players get 1 (5 cars), which ran clean.

#include "recomp.h"
#include "rush2049_game.h"
#include "rush2049_hooks.h"

namespace {

constexpr uint32_t player_count_addr = 0x8014A108;  // halfword: the PLAYERS choice on the main menu, 1 to 4
constexpr uint32_t computer_cars_addr = 0x80142724; // halfword: computer cars in the race about to start
constexpr gpr mode_single_race = 0;
constexpr gpr mode_practice = 1;
constexpr int most_computer_cars_with_four = 1;

int player_count(uint8_t* rdram) {
    return MEM_H(0, (gpr)(int32_t)player_count_addr);
}

} // namespace

extern "C" gpr rush2049_mode_offered(uint8_t* rdram, gpr mode, gpr offered) {
    if (player_count(rdram) > 2 && (mode == mode_single_race || mode == mode_practice) && rush2049::races_up_to_four()) {
        return 1;
    }
    return offered;
}

extern "C" void rush2049_race_computer_cars(uint8_t* rdram, gpr track_cars) {
    int players = player_count(rdram);
    if (players <= 2 || !rush2049::races_up_to_four()) {
        return;
    }
    int cars = (int)track_cars;
    if (players == 4 && cars > most_computer_cars_with_four) {
        cars = most_computer_cars_with_four;
    }
    MEM_H(0, (gpr)(int32_t)computer_cars_addr) = (int16_t)cars;
}
