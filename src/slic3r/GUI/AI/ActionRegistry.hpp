#ifndef slic3r_GUI_AI_ActionRegistry_hpp_
#define slic3r_GUI_AI_ActionRegistry_hpp_

#include <functional>
#include <map>
#include <string>

#include "ActionTypes.hpp"

namespace Slic3r {
namespace GUI {

class Plater;

namespace AI {

class ActionRegistry
{
public:
    explicit ActionRegistry(Plater& plater);

    nlohmann::json tool_definitions() const;
    ActionResult   execute(const ActionCall& call);

private:
    struct ActionDescriptor {
        std::string description;
        nlohmann::json schema;
        std::function<ActionResult(const nlohmann::json&)> executor;
    };

    Plater& m_plater;
    std::map<std::string, ActionDescriptor> m_actions;

    void register_actions();
};

} // namespace AI
} // namespace GUI
} // namespace Slic3r

#endif
