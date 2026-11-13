// Frame interpolation of the scene's moving objects: the matrix group their draws get from the scene-node walk.
//
// The game gives RT64 no matrix IDs, so every draw is in RT64's automatic group, and the rt64 fork moves the vertices
// of an automatic draw between two of the game's frames when both have as many of them and none moved further than
// the box they span (rt64_game_frame.cpp, the end of matchTransform). A keyframed animation (func_800C0AC0) plays by
// stepping a scene node's model through one model per keyframe, the checkered flags over the start line among them:
// the same matrix, as many vertices, but each keyframe a mesh of its own whose vertices aren't in the same order, so
// the flags' separate panels were pulled toward one another's corners in the frames drawn between, and broke into
// stretched shards about one drawn frame in six. The 8-vertex culling box loaded under the same matrix makes the box
// they span 800 units or more, and the flags alone span over 600, so no box test told those moves from real ones.
//
// The scene-node walk (func_8009DD88) draws a node flagged 0x10 or 0x20 (a car, or an object that moves or animates)
// as its matrix, then a list of the culling box (0x8011EF10 or 0x8011EF90), a G_CULLDL of it, and the model. Before
// the box's G_VTX, RT64 gets a matrix group with an ID of its own and otherwise RT64's automatic group's settings, and
// after it the group is popped: RT64 takes a matrix's group from the first vertices loaded under it, so the node's
// matrix keeps the group for the model's vertices that follow, and the pop is in before the G_CULLDL, which ends the
// list when the box is off screen. A draw with an ID is one the fork leaves alone, and with the automatic ordering,
// which matches draws by what they draw and their group's ID, the node's matrix is still matched and interpolated as
// before. Only the moving objects' vertices go from one keyframe to the next on the game's frames.
//
// The list pointer is the walk's local at 0x114($sp), and the commands go where the game would write next; when a node
// draws nothing the walk moves the pointer back to before the node's list, these with it.

#include <cstdint>

#include "recomp.h"
#include "rush2049_hooks.h"

#define F3DEX_GBI_2
#include "../lib/rt64/include/rt64_extended_gbi.h"

namespace {

constexpr uint32_t node_group_id = 0x52320001;  // any ID but G_EX_ID_AUTO and G_EX_ID_IGNORE; nothing else uses one
constexpr int modelview = 0;  // the group is for the modelview matrix stack, not the projection

void append(uint8_t* rdram, recomp_context* ctx, const GfxCommand* cmds, int count) {
    gpr list = (gpr)MEM_W(0x114, ctx->r29);
    for (int i = 0; i < count; i++) {
        MEM_W(i * 8, list) = (int32_t)cmds[i].values.word0;
        MEM_W(i * 8 + 4, list) = (int32_t)cmds[i].values.word1;
    }
    MEM_W(0x114, ctx->r29) = (int32_t)((uint32_t)list + count * 8);
}

}  // namespace

extern "C" void rush2049_node_group_push(uint8_t* rdram, recomp_context* ctx) {
    GfxCommand cmds[2]{};
    gEXMatrixGroup(cmds, node_group_id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, modelview, G_EX_COMPONENT_AUTO,
                   G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO,
                   G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_AUTO, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
                   G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
    append(rdram, ctx, cmds, 2);
}

extern "C" void rush2049_node_group_pop(uint8_t* rdram, recomp_context* ctx) {
    GfxCommand cmds[1]{};
    gEXPopMatrixGroup(cmds, modelview);
    append(rdram, ctx, cmds, 1);
}
