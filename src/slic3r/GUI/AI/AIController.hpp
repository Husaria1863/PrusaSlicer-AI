#ifndef slic3r_GUI_AI_AIController_hpp_
#define slic3r_GUI_AI_AIController_hpp_

#include <string>
#include <vector>

#include "ActionTypes.hpp"
#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {

class Plater;

namespace AI {

struct Settings;

struct ControllerResult
{
    bool                    success { false };
    std::string             assistant_text;
    std::string             error;
    std::vector<ActionResult> action_results;
};

class AIController
{
public:
    explicit AIController(Plater& plater);

    bool            is_available(std::string& reason) const;
    ControllerResult process_prompt(const std::string& prompt, bool allow_actions = true);
    void             reset_chat();

private:
    struct ChatTurn
    {
        std::string role;
        std::string text;
    };

    void add_chat_turn(const std::string& role, const std::string& text);
    void compact_chat_context();
    nlohmann::json build_conversation_context() const;
    nlohmann::json build_runtime_context(const Settings& settings);

    Plater& m_plater;
    std::vector<ChatTurn> m_chat_turns;
    std::string           m_chat_summary;
    nlohmann::json        m_cached_viewport_image = nlohmann::json::object();
};

} // namespace AI
} // namespace GUI
} // namespace Slic3r

#endif
