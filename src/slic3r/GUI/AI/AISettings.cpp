#include "AISettings.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

#include "libslic3r/AppConfig.hpp"

#if wxUSE_SECRETSTORE
#include <wx/secretstore.h>
#endif

namespace Slic3r {
namespace GUI {
namespace AI {

namespace {

const std::string kSettingsSection = "ai";
const std::string kProviderKey     = "provider";
const std::string kModelKey        = "model";
const std::string kBaseUrlKey      = "base_url";
const std::string kApiKeyKey       = "api_key";
const std::string kApiKeyStoredSentinel = "__stored_in_secret_store__";
const std::string kUseViewportImageContextKey = "use_viewport_image_context";
const std::string kAgentModeEnabledKey        = "agent_mode_enabled";
const std::string kAgentModeWarningAcknowledgedKey = "agent_mode_warning_acknowledged";
const std::string kViewportImageSizePxKey     = "viewport_image_size_px";

std::string default_provider()
{
    return "openai_compatible";
}

std::string default_model_for_provider(const std::string& provider_id)
{
    if (provider_id == "claude")
        return "claude-3-7-sonnet-20250219";
    if (provider_id == "gemini")
        return "gemini-2.5-pro";
    return "gpt-5.4";
}

std::string default_base_url_for_provider(const std::string& provider_id)
{
    if (provider_id == "claude")
        return "https://api.anthropic.com/v1/messages";
    if (provider_id == "gemini")
        return "https://generativelanguage.googleapis.com/v1beta/models";
    return "https://api.openai.com/v1/chat/completions";
}

std::string trim_copy(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool has_control_chars(const std::string& value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::iscntrl(c) != 0;
    });
}

std::string sanitize_provider(const std::string& provider_raw)
{
    const std::string provider = lower_copy(trim_copy(provider_raw));
    if (provider == "openai" || provider == "openai_compatible" || provider == "claude" || provider == "gemini")
        return provider;
    return default_provider();
}

bool model_name_chars_allowed(const std::string& value)
{
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == ':' || c == '/';
    });
}

std::string sanitize_model(const std::string& model_raw, const std::string& provider)
{
    std::string model = trim_copy(model_raw);
    if (model.empty() || model.size() > 128 || has_control_chars(model) || !model_name_chars_allowed(model))
        return default_model_for_provider(provider);
    return model;
}

bool is_loopback_host(std::string host)
{
    host = lower_copy(trim_copy(host));
    if (host.empty())
        return false;
    if (!host.empty() && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);

    if (host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1")
        return true;
    if (host == "127.0.0.1")
        return true;
    return host.rfind("127.", 0) == 0;
}

bool is_secure_base_url(const std::string& url_raw)
{
    const std::string url = trim_copy(url_raw);
    if (url.empty() || url.size() > 2048 || has_control_chars(url))
        return false;

    static const std::regex re(R"(^(https?)://([^/?#]+)(?:[/?#].*)?$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(url, m, re))
        return false;

    const std::string scheme = lower_copy(m[1].str());
    std::string authority = m[2].str();
    if (authority.empty())
        return false;
    if (authority.find('@') != std::string::npos)
        return false; // Disallow userinfo in URL.

    std::string host = authority;
    if (!host.empty() && host.front() == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos)
            return false;
        host = host.substr(0, close + 1);
    } else {
        const size_t colon = host.find(':');
        if (colon != std::string::npos)
            host = host.substr(0, colon);
    }

    if (scheme == "https")
        return true;

    // Allow plain HTTP only for local loopback endpoints.
    return scheme == "http" && is_loopback_host(host);
}

std::string sanitize_base_url(const std::string& base_url_raw, const std::string& provider)
{
    const std::string url = trim_copy(base_url_raw);
    if (is_secure_base_url(url))
        return url;
    return default_base_url_for_provider(provider);
}

std::string sanitize_api_key(const std::string& api_key_raw)
{
    std::string key = trim_copy(api_key_raw);
    if (key.size() > 2048 || has_control_chars(key))
        return {};
    return key;
}

#if wxUSE_SECRETSTORE
wxString ai_secret_service()
{
    return wxString::FromUTF8("PrusaSlicer/AI/api_key");
}

bool secret_store_is_ok()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    return store.IsOk();
}

bool save_api_key_secret(const std::string& api_key)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return false;

    const wxString service  = ai_secret_service();
    const wxString username = wxString::FromUTF8("default");
    const wxSecretValue secret(wxString::FromUTF8(api_key.c_str()));
    return store.Save(service, username, secret);
}

bool load_api_key_secret(std::string& out_api_key)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return false;

    wxString username;
    wxSecretValue secret;
    if (!store.Load(ai_secret_service(), username, secret))
        return false;

    const wxString value = secret.GetAsString();
    const wxCharBuffer utf8 = value.utf8_str();
    out_api_key = utf8 ? std::string(utf8.data()) : std::string();
    return true;
}

void clear_api_key_secret()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return;
    store.Delete(ai_secret_service());
}
#endif // wxUSE_SECRETSTORE

} // namespace

bool Settings::has_api_key() const
{
    return !sanitize_api_key(api_key).empty();
}

Settings default_settings()
{
    Settings settings;
    settings.provider = default_provider();
    settings.model    = default_model_for_provider(settings.provider);
    settings.base_url = default_base_url_for_provider(settings.provider);
    settings.api_key  = "";
    settings.use_viewport_image_context = false;
    settings.agent_mode_enabled = true;
    settings.agent_mode_warning_acknowledged = false;
    settings.viewport_image_size_px = 448;
    return settings;
}

Settings load_settings(const AppConfig& app_config)
{
    Settings settings = default_settings();

    const std::string provider = app_config.get(kSettingsSection, kProviderKey);
    if (!provider.empty())
        settings.provider = sanitize_provider(provider);

    const std::string model = app_config.get(kSettingsSection, kModelKey);
    if (!model.empty())
        settings.model = sanitize_model(model, settings.provider);
    else
        settings.model = default_model_for_provider(settings.provider);

    // Migrate historical default to the current default model automatically.
    if ((settings.provider == "openai" || settings.provider == "openai_compatible") && settings.model == "gpt-4.1-mini")
        settings.model = "gpt-5.4";
    if ((settings.provider == "claude" || settings.provider == "gemini") &&
        (settings.model == "gpt-5.4" || settings.model == "gpt-4.1-mini")) {
        settings.model = default_model_for_provider(settings.provider);
    }

    const std::string base_url = app_config.get(kSettingsSection, kBaseUrlKey);
    if (!base_url.empty())
        settings.base_url = sanitize_base_url(base_url, settings.provider);
    else
        settings.base_url = default_base_url_for_provider(settings.provider);
    if ((settings.provider == "claude" || settings.provider == "gemini") &&
        settings.base_url == "https://api.openai.com/v1/chat/completions") {
        settings.base_url = default_base_url_for_provider(settings.provider);
    }

    const std::string persisted_api_key = app_config.get(kSettingsSection, kApiKeyKey);
    if (persisted_api_key == kApiKeyStoredSentinel || persisted_api_key == "stored") {
#if wxUSE_SECRETSTORE
        if (!load_api_key_secret(settings.api_key))
            settings.api_key.clear();
#else
        settings.api_key.clear();
#endif
    } else {
        // Backward compatibility for legacy plaintext values. This is sanitized and
        // moved to secure storage on next save when available.
        settings.api_key = sanitize_api_key(persisted_api_key);
    }

    const std::string use_viewport_image = app_config.get(kSettingsSection, kUseViewportImageContextKey);
    if (!use_viewport_image.empty())
        settings.use_viewport_image_context = use_viewport_image == "1" || use_viewport_image == "true";

    const std::string agent_mode_enabled = app_config.get(kSettingsSection, kAgentModeEnabledKey);
    if (!agent_mode_enabled.empty())
        settings.agent_mode_enabled = agent_mode_enabled == "1" || agent_mode_enabled == "true";

    const std::string agent_mode_warning_acknowledged = app_config.get(kSettingsSection, kAgentModeWarningAcknowledgedKey);
    if (!agent_mode_warning_acknowledged.empty())
        settings.agent_mode_warning_acknowledged = agent_mode_warning_acknowledged == "1" || agent_mode_warning_acknowledged == "true";

    const std::string viewport_image_size_px = app_config.get(kSettingsSection, kViewportImageSizePxKey);
    if (!viewport_image_size_px.empty()) {
        try {
            settings.viewport_image_size_px = std::stoi(viewport_image_size_px);
        } catch (...) {
            // Keep defaults on invalid persisted values.
        }
    }
    return settings;
}

void save_settings(AppConfig& app_config, const Settings& settings)
{
    const std::string provider = sanitize_provider(settings.provider);
    const std::string model = sanitize_model(settings.model, provider);
    const std::string base_url = sanitize_base_url(settings.base_url, provider);
    const std::string api_key = sanitize_api_key(settings.api_key);

    app_config.set(kSettingsSection, kProviderKey, provider);
    app_config.set(kSettingsSection, kModelKey, model);
    app_config.set(kSettingsSection, kBaseUrlKey, base_url);

#if wxUSE_SECRETSTORE
    if (!api_key.empty() && secret_store_is_ok() && save_api_key_secret(api_key)) {
        app_config.set(kSettingsSection, kApiKeyKey, kApiKeyStoredSentinel);
    } else {
        if (api_key.empty())
            clear_api_key_secret();
        // Never persist plaintext API keys.
        app_config.set(kSettingsSection, kApiKeyKey, "");
    }
#else
    // Never persist plaintext API keys when secure store is unavailable.
    app_config.set(kSettingsSection, kApiKeyKey, "");
#endif

    app_config.set(kSettingsSection, kUseViewportImageContextKey, settings.use_viewport_image_context ? "1" : "0");
    app_config.set(kSettingsSection, kAgentModeEnabledKey, settings.agent_mode_enabled ? "1" : "0");
    app_config.set(kSettingsSection, kAgentModeWarningAcknowledgedKey, settings.agent_mode_warning_acknowledged ? "1" : "0");
    app_config.set(kSettingsSection, kViewportImageSizePxKey, std::to_string(settings.viewport_image_size_px));
}

const std::string& settings_section() { return kSettingsSection; }
const std::string& key_provider()     { return kProviderKey; }
const std::string& key_model()        { return kModelKey; }
const std::string& key_base_url()     { return kBaseUrlKey; }
const std::string& key_api_key()      { return kApiKeyKey; }
const std::string& key_use_viewport_image_context() { return kUseViewportImageContextKey; }
const std::string& key_agent_mode_enabled()         { return kAgentModeEnabledKey; }
const std::string& key_agent_mode_warning_acknowledged() { return kAgentModeWarningAcknowledgedKey; }
const std::string& key_viewport_image_size_px()     { return kViewportImageSizePxKey; }

} // namespace AI
} // namespace GUI
} // namespace Slic3r
