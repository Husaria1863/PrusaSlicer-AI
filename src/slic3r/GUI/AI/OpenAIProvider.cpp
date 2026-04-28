#include "OpenAIProvider.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>

#include <boost/log/trivial.hpp>

#include "AISettings.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

namespace {

constexpr size_t kMaxResponseBodyBytes          = 1024 * 1024; // 1 MiB
constexpr size_t kMaxAssistantTextChars         = 8000;
constexpr size_t kMaxActionsPerResponse         = 24;
constexpr size_t kMaxActionNameChars            = 64;
constexpr size_t kMaxActionParamsSerializedSize = 32 * 1024;
constexpr int    kInitialMaxOutputTokens        = 1400;
constexpr int    kMaxOutputTokensCap            = 4096;
constexpr int    kMaxContinuationRetries        = 2;

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

std::string strip_control_chars(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::iscntrl(c) == 0 || c == '\n' || c == '\r' || c == '\t')
            out.push_back(static_cast<char>(c));
    }
    return out;
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

bool is_secure_provider_url(const std::string& url_raw)
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
    if (authority.empty() || authority.find('@') != std::string::npos)
        return false;

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
    return scheme == "http" && is_loopback_host(host);
}

bool is_valid_action_name(const std::string& name)
{
    if (name.empty() || name.size() > kMaxActionNameChars)
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_';
    });
}

std::string maybe_extract_json_payload(const std::string& content)
{
    const std::string trimmed = trim_copy(content);
    if (trimmed.rfind("```", 0) != 0)
        return trimmed;

    const size_t first_newline = trimmed.find('\n');
    if (first_newline == std::string::npos)
        return trimmed;

    const size_t closing = trimmed.rfind("```");
    if (closing == std::string::npos || closing <= first_newline)
        return trimmed;

    return trim_copy(trimmed.substr(first_newline + 1, closing - first_newline - 1));
}

ProviderReply parse_response_json(const std::string& body)
{
    ProviderReply out;

    if (body.size() > kMaxResponseBodyBytes) {
        out.error = "Provider response exceeded maximum allowed size.";
        return out;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        out.error = std::string("Invalid provider response JSON: ") + e.what();
        return out;
    }

    if (root.contains("error") && root["error"].is_object()) {
        out.error = root["error"].value("message", "Provider returned an error.");
        return out;
    }

    if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty()) {
        out.error = "Provider response did not contain choices.";
        return out;
    }

    const nlohmann::json& choice = root["choices"].front();
    if (!choice.contains("message") || !choice["message"].is_object()) {
        out.error = "Provider response did not contain a message.";
        return out;
    }

    const nlohmann::json& msg = choice["message"];
    std::string content;
    if (msg.contains("content") && msg["content"].is_string())
        content = msg["content"].get<std::string>();

    if (content.size() > kMaxResponseBodyBytes)
        content.resize(kMaxResponseBodyBytes);

    const std::string payload_text = maybe_extract_json_payload(content);

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(payload_text);
    } catch (...) {
        out.ok = true;
        out.assistant_text = trim_copy(strip_control_chars(content));
        if (out.assistant_text.size() > kMaxAssistantTextChars)
            out.assistant_text.resize(kMaxAssistantTextChars);
        return out;
    }

    if (payload.contains("assistant_text") && payload["assistant_text"].is_string())
        out.assistant_text = trim_copy(strip_control_chars(payload["assistant_text"].get<std::string>()));
    if (out.assistant_text.size() > kMaxAssistantTextChars)
        out.assistant_text.resize(kMaxAssistantTextChars);

    if (payload.contains("actions") && payload["actions"].is_array()) {
        for (const nlohmann::json& action : payload["actions"]) {
            if (out.actions.size() >= kMaxActionsPerResponse)
                break;
            if (!action.is_object())
                continue;
            const std::string name = action.value("name", "");
            if (!is_valid_action_name(name))
                continue;

            ActionCall call;
            call.name = name;
            if (action.contains("params") && action["params"].is_object())
                call.params = action["params"];
            else
                call.params = nlohmann::json::object();

            if (call.params.dump().size() > kMaxActionParamsSerializedSize)
                continue;

            out.actions.push_back(std::move(call));
        }
    }

    out.ok = true;
    return out;
}

std::string extract_error_message_from_body(const std::string& body)
{
    if (body.empty())
        return {};

    try {
        const nlohmann::json root = nlohmann::json::parse(body);
        if (root.contains("error") && root["error"].is_object()) {
            const std::string message = root["error"].value("message", "");
            if (!message.empty())
                return message;
        }
    } catch (...) {
        // Ignore body parse failures for error reporting fallback.
    }

    return {};
}

bool response_indicates_length_truncation(const std::string& body)
{
    if (body.empty())
        return false;

    try {
        const nlohmann::json root = nlohmann::json::parse(body);
        if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty() || !root["choices"].front().is_object())
            return false;

        const nlohmann::json& choice = root["choices"].front();
        std::string reason = choice.value("finish_reason", "");
        if (reason.empty())
            reason = choice.value("stop_reason", "");
        reason = lower_copy(trim_copy(reason));
        return reason == "length" || reason == "max_tokens";
    } catch (...) {
        return false;
    }
}

void set_output_token_limit(nlohmann::json& body, const std::string& parameter_name, int token_limit)
{
    body.erase("max_tokens");
    body.erase("max_completion_tokens");
    body[parameter_name] = token_limit;
}

bool should_switch_token_parameter(unsigned http_status,
                                   const std::string& body,
                                   const std::string& current_parameter_name,
                                   const std::string& target_parameter_name)
{
    if (http_status != 400 || body.empty() || current_parameter_name.empty() ||
        target_parameter_name.empty() || current_parameter_name == target_parameter_name)
        return false;

    const std::string lower = lower_copy(body);
    return lower.find("unsupported parameter") != std::string::npos &&
           lower.find(lower_copy(current_parameter_name)) != std::string::npos &&
           lower.find(lower_copy(target_parameter_name)) != std::string::npos;
}

void perform_provider_post(const std::string& url,
                          const std::string& api_key,
                          const nlohmann::json& body,
                          const HttpRetryOpt& retry_opts,
                          std::string& response_body,
                          std::string& error_message,
                          unsigned& http_status)
{
    response_body.clear();
    error_message.clear();
    http_status = 0;

    Http::post(url)
        .header("Content-Type", "application/json")
        .header("Authorization", "Bearer " + api_key)
        .size_limit(kMaxResponseBodyBytes)
        .timeout_connect(20)
        .timeout_max(120)
        .set_post_body(body.dump())
        .on_complete([&response_body, &http_status](std::string body_text, unsigned status) {
            response_body = std::move(body_text);
            http_status = status;
        })
        .on_error([&response_body, &error_message, &http_status](std::string body_text, std::string error, unsigned status) {
            response_body = std::move(body_text);
            error_message = std::move(error);
            http_status = status;
        })
        .perform_sync(retry_opts);
}

} // namespace

ProviderReply OpenAICompatibleProvider::request_actions(const Settings& settings,
                                                        const std::string& user_prompt,
                                                        const nlohmann::json& conversation_context,
                                                        const nlohmann::json& scene_snapshot,
                                                        const nlohmann::json& tools,
                                                        const nlohmann::json& execution_history,
                                                        const nlohmann::json& runtime_context,
                                                        bool allow_actions)
{
    ProviderReply out;

    const std::string model = trim_copy(settings.model.empty() ? default_settings().model : settings.model);
    const std::string url   = trim_copy(settings.base_url.empty() ? default_base_url() : settings.base_url);

    if (model.empty() || model.size() > 128 || has_control_chars(model)) {
        out.error = "Invalid AI model identifier configured in Preferences > AI.";
        return out;
    }
    if (!is_secure_provider_url(url)) {
        out.error = "AI base URL is invalid or insecure. Use HTTPS, or HTTP only for localhost/127.0.0.1.";
        return out;
    }
    if (settings.api_key.empty() || settings.api_key.size() > 2048 || has_control_chars(settings.api_key)) {
        out.error = "Invalid API key format configured in Preferences > AI.";
        return out;
    }

    nlohmann::json body;
    body["model"] = model;
    body["temperature"] = 0.0;
    set_output_token_limit(body, "max_tokens", kInitialMaxOutputTokens);

    const std::string system_prompt = allow_actions
        ? std::string(
            "You are an AI assistant integrated into PrusaSlicer. "
            "Conversation context includes summary and recent_turns from prior messages; use it to resolve follow-ups. "
            "Interpret short references like 'other way', 'undo that direction', or 'same as before' using conversation context. "
            "Decide whether to call tools from the provided tool list. "
            "Use scene state and execution history to continue unfinished tasks across rounds. "
            "If runtime_context includes viewport_image, use it as visual truth for shape/layout understanding. "
            "The viewport image reflects the user's current viewport exactly as shown at capture time. "
            "When the user asks for multiple operations, execute the full sequence; do not stop after the first successful action. "
            "Return multiple actions in order in a single response whenever possible. "
            "Use geometry digests (bounding boxes, sizes, centers, ratios) instead of raw mesh triangles. "
            "For geometric edits, prefer deterministic tools like move_selected_to, scale_selected_to_size, "
            "set_selected_rotation, cut_selected_by_ratio, place_selected_on_largest_face, get_selection_geometry, and get_object_geometry. "
            "For renaming and metadata updates, prefer rename_object / rename_volume instead of describing manual UI edits. "
            "For deep app options beyond presets, use get_arrange_settings/set_arrange_settings and backend settings tools. "
            "Do not guess numeric offsets when an exact tool call can compute them. "
            "Do not call undo/redo unless the current user prompt explicitly asks for undo/revert/redo. "
            "If work remains, return the next actions to run now. "
            "If the request is complete, return actions as an empty array. "
            "Keep assistant_text concise (one short sentence). "
            "Never repeat the same setting write multiple times in one turn. "
            "For percentage settings, stay within the setting range (for infill use 0-100). "
            "Respond with strict JSON only, no markdown. "
            "Output schema: {\"assistant_text\": string, \"actions\": [{\"name\": string, \"params\": object}]}. "
            "If no action is required, set actions to []. "
            "Only use provided action names and parameter names.")
        : std::string(
            "You are an AI assistant integrated into PrusaSlicer. "
            "Agent mode is OFF. You must answer in plain English and you must not request or imply any app actions. "
            "Use conversation context for follow-ups. If runtime_context includes viewport_image, use it for visual understanding only. "
            "Keep assistant_text concise and helpful. "
            "Respond with strict JSON only, no markdown. "
            "Output schema: {\"assistant_text\": string, \"actions\": []}. "
            "actions must always be an empty array.");

    nlohmann::json user_context;
    user_context["user_prompt"] = user_prompt;
    user_context["conversation_context"] = conversation_context;
    user_context["scene_snapshot"] = scene_snapshot;
    user_context["tools"] = tools;
    user_context["execution_history"] = execution_history;
    user_context["runtime_context"] = runtime_context;

    nlohmann::json user_message_content = user_context.dump();
    if (runtime_context.contains("viewport_image") &&
        runtime_context["viewport_image"].is_object() &&
        runtime_context["viewport_image"].value("available", false) &&
        runtime_context["viewport_image"].contains("data_url") &&
        runtime_context["viewport_image"]["data_url"].is_string()) {
        user_message_content = nlohmann::json::array({
            nlohmann::json{{"type", "text"}, {"text", user_context.dump()}},
            nlohmann::json{
                {"type", "image_url"},
                {"image_url", nlohmann::json{
                    {"url", runtime_context["viewport_image"]["data_url"]},
                    {"detail", "low"}
                }}
            }
        });
    }

    body["messages"] = nlohmann::json::array({
        nlohmann::json{{"role", "system"}, {"content", system_prompt}},
        nlohmann::json{{"role", "user"}, {"content", user_message_content}}
    });

    std::string response_body;
    std::string error_message;
    unsigned    http_status = 0;
    const HttpRetryOpt retry_opts{
        std::chrono::milliseconds(700),
        std::chrono::milliseconds(5000),
        3
    };

    BOOST_LOG_TRIVIAL(info) << "AI provider request started. prompt_len=" << user_prompt.size();

    int max_output_tokens = kInitialMaxOutputTokens;
    std::string token_parameter_name = "max_tokens";
    bool switched_token_parameter = false;
    int truncation_retry_count = 0;
    for (;;) {
        set_output_token_limit(body, token_parameter_name, max_output_tokens);
        perform_provider_post(url, settings.api_key, body, retry_opts, response_body, error_message, http_status);

        if (!error_message.empty())
            break;
        if (http_status >= 400) {
            const std::string target_token_parameter =
                token_parameter_name == "max_tokens" ? "max_completion_tokens" : "max_tokens";
            if (!switched_token_parameter &&
                should_switch_token_parameter(http_status, response_body, token_parameter_name, target_token_parameter)) {
                token_parameter_name = target_token_parameter;
                switched_token_parameter = true;
                BOOST_LOG_TRIVIAL(info)
                    << "AI provider rejected " << (token_parameter_name == "max_tokens" ? "max_completion_tokens" : "max_tokens")
                    << "; retrying with " << token_parameter_name << ".";
                continue;
            }
            break;
        }

        if (!response_indicates_length_truncation(response_body))
            break;

        if (truncation_retry_count >= kMaxContinuationRetries || max_output_tokens >= kMaxOutputTokensCap)
            break;

        ++truncation_retry_count;
        max_output_tokens = std::min(kMaxOutputTokensCap, max_output_tokens * 2);
        BOOST_LOG_TRIVIAL(warning)
            << "AI provider response truncated by length. Retrying with larger max_tokens="
            << max_output_tokens << " attempt=" << truncation_retry_count;
    }

    if (!error_message.empty()) {
        out.error = "AI request failed: " + error_message;
        BOOST_LOG_TRIVIAL(error) << "AI provider request failed. http_status=" << http_status;
        return out;
    }

    if (http_status == 401 || http_status == 403) {
        out.error = "Authentication failed. Please verify your AI API key in Preferences > AI.";
        BOOST_LOG_TRIVIAL(error) << "AI provider auth failure. http_status=" << http_status;
        return out;
    }

    if (http_status == 429) {
        const std::string provider_message = extract_error_message_from_body(response_body);
        out.error = "AI provider rate limit or quota exceeded (HTTP 429). Check your API usage/billing and retry.";
        if (!provider_message.empty())
            out.error += " Provider message: " + provider_message;
        BOOST_LOG_TRIVIAL(error) << "AI provider rate limit/quota failure. http_status=429";
        return out;
    }

    if (http_status >= 400) {
        out.error = "AI provider returned HTTP " + std::to_string(http_status) + ".";
        const std::string provider_message = extract_error_message_from_body(response_body);
        if (!provider_message.empty())
            out.error += " " + provider_message;
        BOOST_LOG_TRIVIAL(error) << "AI provider HTTP error. http_status=" << http_status;
        return out;
    }

    ProviderReply parsed = parse_response_json(response_body);
    if (!parsed.ok && parsed.error.empty())
        parsed.error = "Failed to parse AI provider response.";

    if (parsed.ok)
        BOOST_LOG_TRIVIAL(info) << "AI provider call succeeded. actions=" << parsed.actions.size();
    else
        BOOST_LOG_TRIVIAL(error) << "AI provider parse failure.";

    return parsed;
}

std::string OpenAICompatibleProvider::default_base_url()
{
    return default_settings().base_url;
}

std::unique_ptr<IAIProvider> make_provider(const std::string& provider_id)
{
    if (provider_id == "openai" || provider_id == "openai_compatible" || provider_id.empty())
        return std::make_unique<OpenAICompatibleProvider>();

    return nullptr;
}

} // namespace AI
} // namespace GUI
} // namespace Slic3r
