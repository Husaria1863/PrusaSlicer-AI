#include "OpenAIProvider.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>
#include <utility>
#include <vector>

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

const std::string kOpenAIBaseUrl   = "https://api.openai.com/v1/chat/completions";
const std::string kClaudeBaseUrl   = "https://api.anthropic.com/v1/messages";
const std::string kGeminiBaseUrl   = "https://generativelanguage.googleapis.com/v1beta/models";
const std::string kAnthropicApiVer = "2023-06-01";

struct InlineImageData
{
    bool        available { false };
    std::string mime_type;
    std::string base64_data;
};

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

bool ends_with(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

void replace_all_inplace(std::string& value, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
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

void append_text_chunk(std::string& dst, const std::string& chunk)
{
    if (chunk.empty())
        return;
    if (!dst.empty())
        dst.push_back('\n');
    dst += chunk;
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

ProviderReply parse_payload_text(const std::string& content_raw)
{
    ProviderReply out;
    std::string content = content_raw;
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

std::string collect_openai_content_text(const nlohmann::json& msg)
{
    std::string content;
    if (!msg.contains("content"))
        return content;

    const nlohmann::json& node = msg["content"];
    if (node.is_string()) {
        append_text_chunk(content, node.get<std::string>());
        return content;
    }

    if (node.is_array()) {
        for (const nlohmann::json& part : node) {
            if (part.is_string()) {
                append_text_chunk(content, part.get<std::string>());
            } else if (part.is_object()) {
                if (part.value("type", "") == "text" && part.contains("text") && part["text"].is_string())
                    append_text_chunk(content, part["text"].get<std::string>());
                else if (part.contains("text") && part["text"].is_string())
                    append_text_chunk(content, part["text"].get<std::string>());
            }
        }
    }

    return content;
}

std::string collect_anthropic_content_text(const nlohmann::json& root)
{
    std::string content;
    if (!root.contains("content"))
        return content;

    const nlohmann::json& node = root["content"];
    if (node.is_string()) {
        append_text_chunk(content, node.get<std::string>());
        return content;
    }

    if (node.is_array()) {
        for (const nlohmann::json& part : node) {
            if (!part.is_object())
                continue;
            if (part.value("type", "") == "text" && part.contains("text") && part["text"].is_string())
                append_text_chunk(content, part["text"].get<std::string>());
        }
    }
    return content;
}

std::string collect_gemini_content_text(const nlohmann::json& root)
{
    std::string content;
    if (!root.contains("candidates") || !root["candidates"].is_array() || root["candidates"].empty() || !root["candidates"].front().is_object())
        return content;

    const nlohmann::json& candidate = root["candidates"].front();
    if (!candidate.contains("content") || !candidate["content"].is_object())
        return content;

    const nlohmann::json& content_node = candidate["content"];
    if (!content_node.contains("parts") || !content_node["parts"].is_array())
        return content;

    for (const nlohmann::json& part : content_node["parts"]) {
        if (!part.is_object())
            continue;
        if (part.contains("text") && part["text"].is_string())
            append_text_chunk(content, part["text"].get<std::string>());
    }

    return content;
}

ProviderReply parse_openai_response_json(const std::string& body)
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

    return parse_payload_text(collect_openai_content_text(choice["message"]));
}

ProviderReply parse_anthropic_response_json(const std::string& body)
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

    const std::string content = collect_anthropic_content_text(root);
    if (content.empty()) {
        out.error = "Provider response did not contain text content.";
        return out;
    }

    return parse_payload_text(content);
}

ProviderReply parse_gemini_response_json(const std::string& body)
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

    const std::string content = collect_gemini_content_text(root);
    if (content.empty()) {
        if (root.contains("promptFeedback") && root["promptFeedback"].is_object()) {
            const std::string block_reason = root["promptFeedback"].value("blockReason", "");
            if (!block_reason.empty()) {
                out.error = "Provider blocked the request: " + block_reason;
                return out;
            }
        }
        out.error = "Provider response did not contain text content.";
        return out;
    }

    return parse_payload_text(content);
}

bool response_indicates_length_truncation_openai(const std::string& body)
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

bool response_indicates_length_truncation_claude(const std::string& body)
{
    if (body.empty())
        return false;
    try {
        const nlohmann::json root = nlohmann::json::parse(body);
        std::string reason = lower_copy(trim_copy(root.value("stop_reason", "")));
        return reason == "max_tokens";
    } catch (...) {
        return false;
    }
}

bool response_indicates_length_truncation_gemini(const std::string& body)
{
    if (body.empty())
        return false;
    try {
        const nlohmann::json root = nlohmann::json::parse(body);
        if (!root.contains("candidates") || !root["candidates"].is_array() || root["candidates"].empty() || !root["candidates"].front().is_object())
            return false;
        const std::string reason = lower_copy(trim_copy(root["candidates"].front().value("finishReason", "")));
        return reason == "max_tokens";
    } catch (...) {
        return false;
    }
}

void set_output_token_limit_openai(nlohmann::json& body, const std::string& parameter_name, int token_limit)
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
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const nlohmann::json& body,
                           const HttpRetryOpt& retry_opts,
                           std::string& response_body,
                           std::string& error_message,
                           unsigned& http_status)
{
    response_body.clear();
    error_message.clear();
    http_status = 0;

    auto request = Http::post(url);
    for (const auto& header : headers)
        request.header(header.first, header.second);

    request
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

std::string url_encode_query_component(const std::string& value)
{
    static const char hex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string append_query_param(std::string url, const std::string& key, const std::string& value)
{
    if (url.find('?') == std::string::npos)
        url.push_back('?');
    else if (!url.empty() && url.back() != '?' && url.back() != '&')
        url.push_back('&');

    url += key;
    url.push_back('=');
    url += url_encode_query_component(value);
    return url;
}

std::string build_gemini_request_url(std::string base_url, const std::string& model, const std::string& api_key)
{
    std::string url = trim_copy(base_url);
    if (url.empty())
        url = kGeminiBaseUrl;

    if (url.find("{model}") != std::string::npos) {
        replace_all_inplace(url, "{model}", model);
        if (url.find(":generateContent") == std::string::npos)
            url += ":generateContent";
    } else if (url.find(":generateContent") == std::string::npos) {
        if (ends_with(url, "/models")) {
            url += "/" + model + ":generateContent";
        } else if (url.find("/models/") != std::string::npos) {
            if (ends_with(url, "/"))
                url += model;
            if (url.find(":generateContent") == std::string::npos)
                url += ":generateContent";
        } else {
            if (ends_with(url, "/"))
                url.pop_back();
            url += "/models/" + model + ":generateContent";
        }
    }

    return append_query_param(std::move(url), "key", api_key);
}

InlineImageData extract_inline_image(const nlohmann::json& runtime_context)
{
    InlineImageData out;
    if (!runtime_context.is_object())
        return out;
    if (!runtime_context.contains("viewport_image") || !runtime_context["viewport_image"].is_object())
        return out;

    const nlohmann::json& image = runtime_context["viewport_image"];
    if (!image.value("available", false))
        return out;
    if (!image.contains("data_url") || !image["data_url"].is_string())
        return out;

    const std::string data_url = image["data_url"].get<std::string>();
    const std::string prefix = "data:";
    if (data_url.rfind(prefix, 0) != 0)
        return out;

    const size_t base64_sep = data_url.find(";base64,");
    if (base64_sep == std::string::npos || base64_sep <= prefix.size())
        return out;

    out.mime_type = data_url.substr(prefix.size(), base64_sep - prefix.size());
    out.base64_data = data_url.substr(base64_sep + 8);
    if (out.mime_type.empty() || out.base64_data.empty())
        return InlineImageData{};

    out.available = true;
    return out;
}

std::string build_system_prompt(bool allow_actions)
{
    return allow_actions
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
            "For FFF support requests: 'auto-generated supports on' means set support_material=true and support_material_auto=true. "
            "'auto-generated supports off' means set support_material_auto=false (and optionally support_material=false when user asks to disable supports). "
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
}

nlohmann::json build_user_context_json(const std::string& user_prompt,
                                       const nlohmann::json& conversation_context,
                                       const nlohmann::json& scene_snapshot,
                                       const nlohmann::json& tools,
                                       const nlohmann::json& execution_history,
                                       const nlohmann::json& runtime_context)
{
    nlohmann::json user_context;
    user_context["user_prompt"] = user_prompt;
    user_context["conversation_context"] = conversation_context;
    user_context["scene_snapshot"] = scene_snapshot;
    user_context["tools"] = tools;
    user_context["execution_history"] = execution_history;
    user_context["runtime_context"] = runtime_context;
    return user_context;
}

class ClaudeProvider final : public IAIProvider
{
public:
    ProviderReply request_actions(const Settings& settings,
                                  const std::string& user_prompt,
                                  const nlohmann::json& conversation_context,
                                  const nlohmann::json& scene_snapshot,
                                  const nlohmann::json& tools,
                                  const nlohmann::json& execution_history,
                                  const nlohmann::json& runtime_context,
                                  bool allow_actions) override
    {
        ProviderReply out;

        const std::string model = trim_copy(settings.model.empty() ? "claude-3-7-sonnet-20250219" : settings.model);
        const std::string url   = trim_copy(settings.base_url.empty() ? kClaudeBaseUrl : settings.base_url);

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

        const std::string system_prompt = build_system_prompt(allow_actions);
        const nlohmann::json user_context = build_user_context_json(user_prompt, conversation_context, scene_snapshot, tools, execution_history, runtime_context);
        const std::string user_context_text = user_context.dump();

        nlohmann::json user_blocks = nlohmann::json::array({
            nlohmann::json{{"type", "text"}, {"text", user_context_text}}
        });

        const InlineImageData image = extract_inline_image(runtime_context);
        if (image.available) {
            user_blocks.push_back(nlohmann::json{
                {"type", "image"},
                {"source", nlohmann::json{
                    {"type", "base64"},
                    {"media_type", image.mime_type},
                    {"data", image.base64_data}
                }}
            });
        }

        nlohmann::json body;
        body["model"] = model;
        body["temperature"] = 0.0;
        body["max_tokens"] = kInitialMaxOutputTokens;
        body["system"] = system_prompt;
        body["messages"] = nlohmann::json::array({
            nlohmann::json{{"role", "user"}, {"content", user_blocks}}
        });

        std::string response_body;
        std::string error_message;
        unsigned    http_status = 0;
        const HttpRetryOpt retry_opts{
            std::chrono::milliseconds(700),
            std::chrono::milliseconds(5000),
            3
        };

        BOOST_LOG_TRIVIAL(info) << "Claude provider request started. prompt_len=" << user_prompt.size();

        int max_output_tokens = kInitialMaxOutputTokens;
        int truncation_retry_count = 0;
        for (;;) {
            body["max_tokens"] = max_output_tokens;
            perform_provider_post(
                url,
                {
                    {"Content-Type", "application/json"},
                    {"x-api-key", settings.api_key},
                    {"anthropic-version", kAnthropicApiVer}
                },
                body,
                retry_opts,
                response_body,
                error_message,
                http_status);

            if (!error_message.empty() || http_status >= 400)
                break;

            if (!response_indicates_length_truncation_claude(response_body))
                break;
            if (truncation_retry_count >= kMaxContinuationRetries || max_output_tokens >= kMaxOutputTokensCap)
                break;

            ++truncation_retry_count;
            max_output_tokens = std::min(kMaxOutputTokensCap, max_output_tokens * 2);
            BOOST_LOG_TRIVIAL(warning)
                << "Claude response truncated by length. Retrying with larger max_tokens="
                << max_output_tokens << " attempt=" << truncation_retry_count;
        }

        if (!error_message.empty()) {
            out.error = "AI request failed: " + error_message;
            BOOST_LOG_TRIVIAL(error) << "Claude provider request failed. http_status=" << http_status;
            return out;
        }

        if (http_status == 401 || http_status == 403) {
            out.error = "Authentication failed. Please verify your AI API key in Preferences > AI.";
            BOOST_LOG_TRIVIAL(error) << "Claude provider auth failure. http_status=" << http_status;
            return out;
        }

        if (http_status == 429) {
            const std::string provider_message = extract_error_message_from_body(response_body);
            out.error = "AI provider rate limit or quota exceeded (HTTP 429). Check your API usage/billing and retry.";
            if (!provider_message.empty())
                out.error += " Provider message: " + provider_message;
            BOOST_LOG_TRIVIAL(error) << "Claude provider rate limit/quota failure. http_status=429";
            return out;
        }

        if (http_status >= 400) {
            out.error = "AI provider returned HTTP " + std::to_string(http_status) + ".";
            const std::string provider_message = extract_error_message_from_body(response_body);
            if (!provider_message.empty())
                out.error += " " + provider_message;
            BOOST_LOG_TRIVIAL(error) << "Claude provider HTTP error. http_status=" << http_status;
            return out;
        }

        ProviderReply parsed = parse_anthropic_response_json(response_body);
        if (!parsed.ok && parsed.error.empty())
            parsed.error = "Failed to parse AI provider response.";

        if (parsed.ok)
            BOOST_LOG_TRIVIAL(info) << "Claude provider call succeeded. actions=" << parsed.actions.size();
        else
            BOOST_LOG_TRIVIAL(error) << "Claude provider parse failure.";

        return parsed;
    }
};

class GeminiProvider final : public IAIProvider
{
public:
    ProviderReply request_actions(const Settings& settings,
                                  const std::string& user_prompt,
                                  const nlohmann::json& conversation_context,
                                  const nlohmann::json& scene_snapshot,
                                  const nlohmann::json& tools,
                                  const nlohmann::json& execution_history,
                                  const nlohmann::json& runtime_context,
                                  bool allow_actions) override
    {
        ProviderReply out;

        const std::string model = trim_copy(settings.model.empty() ? "gemini-2.5-pro" : settings.model);
        const std::string base_url = trim_copy(settings.base_url.empty() ? kGeminiBaseUrl : settings.base_url);

        if (model.empty() || model.size() > 128 || has_control_chars(model)) {
            out.error = "Invalid AI model identifier configured in Preferences > AI.";
            return out;
        }
        if (!is_secure_provider_url(base_url)) {
            out.error = "AI base URL is invalid or insecure. Use HTTPS, or HTTP only for localhost/127.0.0.1.";
            return out;
        }
        if (settings.api_key.empty() || settings.api_key.size() > 2048 || has_control_chars(settings.api_key)) {
            out.error = "Invalid API key format configured in Preferences > AI.";
            return out;
        }

        const std::string url = build_gemini_request_url(base_url, model, settings.api_key);
        const std::string system_prompt = build_system_prompt(allow_actions);
        const nlohmann::json user_context = build_user_context_json(user_prompt, conversation_context, scene_snapshot, tools, execution_history, runtime_context);
        const std::string user_context_text = user_context.dump();

        nlohmann::json parts = nlohmann::json::array({
            nlohmann::json{{"text", user_context_text}}
        });

        const InlineImageData image = extract_inline_image(runtime_context);
        if (image.available) {
            parts.push_back(nlohmann::json{
                {"inline_data", nlohmann::json{
                    {"mime_type", image.mime_type},
                    {"data", image.base64_data}
                }}
            });
        }

        nlohmann::json body;
        body["systemInstruction"] = nlohmann::json{
            {"parts", nlohmann::json::array({ nlohmann::json{{"text", system_prompt}} })}
        };
        body["contents"] = nlohmann::json::array({
            nlohmann::json{
                {"role", "user"},
                {"parts", parts}
            }
        });
        body["generationConfig"] = nlohmann::json{
            {"temperature", 0.0},
            {"maxOutputTokens", kInitialMaxOutputTokens}
        };

        std::string response_body;
        std::string error_message;
        unsigned    http_status = 0;
        const HttpRetryOpt retry_opts{
            std::chrono::milliseconds(700),
            std::chrono::milliseconds(5000),
            3
        };

        BOOST_LOG_TRIVIAL(info) << "Gemini provider request started. prompt_len=" << user_prompt.size();

        int max_output_tokens = kInitialMaxOutputTokens;
        int truncation_retry_count = 0;
        for (;;) {
            body["generationConfig"]["maxOutputTokens"] = max_output_tokens;
            perform_provider_post(
                url,
                {
                    {"Content-Type", "application/json"}
                },
                body,
                retry_opts,
                response_body,
                error_message,
                http_status);

            if (!error_message.empty() || http_status >= 400)
                break;

            if (!response_indicates_length_truncation_gemini(response_body))
                break;
            if (truncation_retry_count >= kMaxContinuationRetries || max_output_tokens >= kMaxOutputTokensCap)
                break;

            ++truncation_retry_count;
            max_output_tokens = std::min(kMaxOutputTokensCap, max_output_tokens * 2);
            BOOST_LOG_TRIVIAL(warning)
                << "Gemini response truncated by length. Retrying with larger maxOutputTokens="
                << max_output_tokens << " attempt=" << truncation_retry_count;
        }

        if (!error_message.empty()) {
            out.error = "AI request failed: " + error_message;
            BOOST_LOG_TRIVIAL(error) << "Gemini provider request failed. http_status=" << http_status;
            return out;
        }

        if (http_status == 401 || http_status == 403) {
            out.error = "Authentication failed. Please verify your AI API key in Preferences > AI.";
            BOOST_LOG_TRIVIAL(error) << "Gemini provider auth failure. http_status=" << http_status;
            return out;
        }

        if (http_status == 429) {
            const std::string provider_message = extract_error_message_from_body(response_body);
            out.error = "AI provider rate limit or quota exceeded (HTTP 429). Check your API usage/billing and retry.";
            if (!provider_message.empty())
                out.error += " Provider message: " + provider_message;
            BOOST_LOG_TRIVIAL(error) << "Gemini provider rate limit/quota failure. http_status=429";
            return out;
        }

        if (http_status >= 400) {
            out.error = "AI provider returned HTTP " + std::to_string(http_status) + ".";
            const std::string provider_message = extract_error_message_from_body(response_body);
            if (!provider_message.empty())
                out.error += " " + provider_message;
            BOOST_LOG_TRIVIAL(error) << "Gemini provider HTTP error. http_status=" << http_status;
            return out;
        }

        ProviderReply parsed = parse_gemini_response_json(response_body);
        if (!parsed.ok && parsed.error.empty())
            parsed.error = "Failed to parse AI provider response.";

        if (parsed.ok)
            BOOST_LOG_TRIVIAL(info) << "Gemini provider call succeeded. actions=" << parsed.actions.size();
        else
            BOOST_LOG_TRIVIAL(error) << "Gemini provider parse failure.";

        return parsed;
    }
};

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

    const std::string model = trim_copy(settings.model.empty() ? "gpt-5.4" : settings.model);
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

    const std::string system_prompt = build_system_prompt(allow_actions);
    const nlohmann::json user_context = build_user_context_json(user_prompt, conversation_context, scene_snapshot, tools, execution_history, runtime_context);

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

    nlohmann::json body;
    body["model"] = model;
    body["temperature"] = 0.0;
    set_output_token_limit_openai(body, "max_tokens", kInitialMaxOutputTokens);
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

    BOOST_LOG_TRIVIAL(info) << "OpenAI-compatible provider request started. prompt_len=" << user_prompt.size();

    int max_output_tokens = kInitialMaxOutputTokens;
    std::string token_parameter_name = "max_tokens";
    bool switched_token_parameter = false;
    int truncation_retry_count = 0;
    for (;;) {
        set_output_token_limit_openai(body, token_parameter_name, max_output_tokens);
        perform_provider_post(
            url,
            {
                {"Content-Type", "application/json"},
                {"Authorization", "Bearer " + settings.api_key}
            },
            body,
            retry_opts,
            response_body,
            error_message,
            http_status);

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

        if (!response_indicates_length_truncation_openai(response_body))
            break;

        if (truncation_retry_count >= kMaxContinuationRetries || max_output_tokens >= kMaxOutputTokensCap)
            break;

        ++truncation_retry_count;
        max_output_tokens = std::min(kMaxOutputTokensCap, max_output_tokens * 2);
        BOOST_LOG_TRIVIAL(warning)
            << "OpenAI-compatible provider response truncated by length. Retrying with larger max_tokens="
            << max_output_tokens << " attempt=" << truncation_retry_count;
    }

    if (!error_message.empty()) {
        out.error = "AI request failed: " + error_message;
        BOOST_LOG_TRIVIAL(error) << "OpenAI-compatible provider request failed. http_status=" << http_status;
        return out;
    }

    if (http_status == 401 || http_status == 403) {
        out.error = "Authentication failed. Please verify your AI API key in Preferences > AI.";
        BOOST_LOG_TRIVIAL(error) << "OpenAI-compatible provider auth failure. http_status=" << http_status;
        return out;
    }

    if (http_status == 429) {
        const std::string provider_message = extract_error_message_from_body(response_body);
        out.error = "AI provider rate limit or quota exceeded (HTTP 429). Check your API usage/billing and retry.";
        if (!provider_message.empty())
            out.error += " Provider message: " + provider_message;
        BOOST_LOG_TRIVIAL(error) << "OpenAI-compatible provider rate limit/quota failure. http_status=429";
        return out;
    }

    if (http_status >= 400) {
        out.error = "AI provider returned HTTP " + std::to_string(http_status) + ".";
        const std::string provider_message = extract_error_message_from_body(response_body);
        if (!provider_message.empty())
            out.error += " " + provider_message;
        BOOST_LOG_TRIVIAL(error) << "OpenAI-compatible provider HTTP error. http_status=" << http_status;
        return out;
    }

    ProviderReply parsed = parse_openai_response_json(response_body);
    if (!parsed.ok && parsed.error.empty())
        parsed.error = "Failed to parse AI provider response.";

    if (parsed.ok)
        BOOST_LOG_TRIVIAL(info) << "OpenAI-compatible provider call succeeded. actions=" << parsed.actions.size();
    else
        BOOST_LOG_TRIVIAL(error) << "OpenAI-compatible provider parse failure.";

    return parsed;
}

std::string OpenAICompatibleProvider::default_base_url()
{
    return kOpenAIBaseUrl;
}

std::unique_ptr<IAIProvider> make_provider(const std::string& provider_id)
{
    if (provider_id == "claude")
        return std::make_unique<ClaudeProvider>();
    if (provider_id == "gemini")
        return std::make_unique<GeminiProvider>();
    if (provider_id == "openai" || provider_id == "openai_compatible" || provider_id.empty())
        return std::make_unique<OpenAICompatibleProvider>();

    return nullptr;
}

} // namespace AI
} // namespace GUI
} // namespace Slic3r
