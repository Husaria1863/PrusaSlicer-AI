#ifndef slic3r_GUI_AI_SceneSnapshot_hpp_
#define slic3r_GUI_AI_SceneSnapshot_hpp_

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {

class Plater;

namespace AI {

nlohmann::json build_scene_snapshot(const Plater& plater);

} // namespace AI
} // namespace GUI
} // namespace Slic3r

#endif
