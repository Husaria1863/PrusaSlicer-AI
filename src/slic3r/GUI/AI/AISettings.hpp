#ifndef slic3r_GUI_AI_AISettings_hpp_
#define slic3r_GUI_AI_AISettings_hpp_

#include <string>

namespace Slic3r {
class AppConfig;

namespace GUI {
namespace AI {

struct Settings
{
    std::string provider;
    std::string model;
    std::string base_url;
    std::string api_key;
    bool        use_viewport_image_context { false };
    bool        agent_mode_enabled { true };
    bool        agent_mode_warning_acknowledged { false };
    int         viewport_image_size_px { 448 };

    bool has_api_key() const;
};

Settings default_settings();
Settings load_settings(const AppConfig& app_config);
void     save_settings(AppConfig& app_config, const Settings& settings);

const std::string& settings_section();
const std::string& key_provider();
const std::string& key_model();
const std::string& key_base_url();
const std::string& key_api_key();
const std::string& key_use_viewport_image_context();
const std::string& key_agent_mode_enabled();
const std::string& key_agent_mode_warning_acknowledged();
const std::string& key_viewport_image_size_px();

} // namespace AI
} // namespace GUI
} // namespace Slic3r

#endif
