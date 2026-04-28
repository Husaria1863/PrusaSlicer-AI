#ifndef slic3r_GUI_AI_ActionTypes_hpp_
#define slic3r_GUI_AI_ActionTypes_hpp_

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

struct ActionCall
{
    std::string   name;
    nlohmann::json params = nlohmann::json::object();
};

struct ActionResult
{
    std::string   name;
    bool          success      { false };
    bool          destructive  { false };
    std::string   message;
    nlohmann::json data = nlohmann::json::object();

    nlohmann::json to_json() const;
};

struct ProviderReply
{
    bool                    ok { false };
    std::string             assistant_text;
    std::vector<ActionCall> actions;
    std::string             error;
};

} // namespace AI
} // namespace GUI
} // namespace Slic3r

#endif
