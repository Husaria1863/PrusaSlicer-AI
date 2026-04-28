#include "ActionTypes.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

nlohmann::json ActionResult::to_json() const
{
    return nlohmann::json{
        { "name", name },
        { "success", success },
        { "destructive", destructive },
        { "message", message },
        { "data", data }
    };
}

} // namespace AI
} // namespace GUI
} // namespace Slic3r
