#ifndef slic3r_GUI_AI_OpenAIProvider_hpp_
#define slic3r_GUI_AI_OpenAIProvider_hpp_

#include "AIProvider.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

class OpenAICompatibleProvider final : public IAIProvider
{
public:
    ProviderReply request_actions(const Settings& settings,
                                  const std::string& user_prompt,
                                  const nlohmann::json& conversation_context,
                                  const nlohmann::json& scene_snapshot,
                                  const nlohmann::json& tools,
                                  const nlohmann::json& execution_history,
                                  const nlohmann::json& runtime_context,
                                  bool allow_actions) override;

private:
    static std::string default_base_url();
};

} // namespace AI
} // namespace GUI
} // namespace Slic3r

#endif
