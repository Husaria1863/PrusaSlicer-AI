#ifndef slic3r_GUI_AI_AIProvider_hpp_
#define slic3r_GUI_AI_AIProvider_hpp_

#include <memory>
#include <string>

#include "nlohmann/json.hpp"

#include "ActionTypes.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

struct Settings;

class IAIProvider
{
public:
    virtual ~IAIProvider() = default;

    virtual ProviderReply request_actions(const Settings& settings,
                                          const std::string& user_prompt,
                                          const nlohmann::json& conversation_context,
                                          const nlohmann::json& scene_snapshot,
                                          const nlohmann::json& tools,
                                          const nlohmann::json& execution_history,
                                          const nlohmann::json& runtime_context,
                                          bool allow_actions) = 0;
};

std::unique_ptr<IAIProvider> make_provider(const std::string& provider_id);

} // namespace AI
} // namespace GUI
} // namespace Slic3r

#endif
