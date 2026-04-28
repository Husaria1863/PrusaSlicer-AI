#include "AIController.hpp"

#include <sstream>
#include <set>
#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/log/trivial.hpp>

#include "ActionRegistry.hpp"
#include "AIProvider.hpp"
#include "AISettings.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "../GUI_App.hpp"
#include "../GLCanvas3D.hpp"
#include "OpenAIProvider.hpp"
#include "../Plater.hpp"
#include "SceneSnapshot.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

namespace {

constexpr size_t kConversationContextTokenBudget = 1400;
constexpr size_t kConversationSummaryTokenBudget = 420;
constexpr size_t kConversationMaxRecentTurns     = 14;

std::string unavailable_message()
{
    return "To use AI features, enter your API key in Preferences > AI.";
}

std::string summarize_actions(const std::vector<ActionResult>& results)
{
    if (results.empty())
        return "No actions executed.";

    std::ostringstream os;
    os << "Executed " << results.size() << " action" << (results.size() == 1 ? "" : "s") << ": ";

    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0)
            os << " | ";
        os << results[i].name << " -> " << (results[i].success ? "ok" : "failed");
    }

    return os.str();
}

std::string summarize_actions_for_history(const std::vector<ActionResult>& results)
{
    if (results.empty())
        return "No actions.";

    std::ostringstream os;
    bool first = true;
    for (const ActionResult& result : results) {
        if (!first)
            os << " | ";
        first = false;
        os << result.name << " [" << (result.success ? "ok" : "failed") << "]";
        if (!result.message.empty())
            os << ": " << result.message;
    }
    return os.str();
}

size_t estimate_token_count(const std::string& text)
{
    // Fast heuristic to keep context bounded without external tokenizers.
    return (text.size() + 3) / 4;
}

std::string squash_whitespace(std::string text)
{
    std::string out;
    out.reserve(text.size());
    bool prev_space = false;
    for (unsigned char c : text) {
        const bool is_space = std::isspace(c) != 0;
        if (is_space) {
            if (!prev_space)
                out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(c));
        }
        prev_space = is_space;
    }
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

std::string truncate_for_history(const std::string& text, size_t max_chars = 320)
{
    const std::string compact = squash_whitespace(text);
    if (compact.size() <= max_chars)
        return compact;
    return compact.substr(0, max_chars) + "...";
}

std::string conversation_line(const std::string& role, const std::string& text)
{
    return role + ": " + text;
}

void append_summary_line(std::string& summary, const std::string& role, const std::string& text)
{
    if (!summary.empty())
        summary += "\n";
    summary += conversation_line(role, truncate_for_history(text, 180));
}

bool is_setting_write_action(const std::string& action_name)
{
    return action_name == "set_print_setting" ||
           action_name == "set_filament_setting" ||
           action_name == "set_printer_setting";
}

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool contains_ci(const std::string& haystack, const std::string& needle)
{
    const std::string h = lower_copy(haystack);
    const std::string n = lower_copy(needle);
    return !n.empty() && h.find(n) != std::string::npos;
}

std::string trim_copy_local(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

bool prompt_mentions_printables(const std::string& prompt)
{
    return contains_ci(prompt, "printables");
}

bool prompt_requests_import(const std::string& prompt)
{
    return contains_ci(prompt, "import");
}

bool prompt_requests_search(const std::string& prompt)
{
    return contains_ci(prompt, "search") || contains_ci(prompt, "find") || contains_ci(prompt, "look up");
}

std::string extract_printables_query(const std::string& prompt)
{
    std::string query = prompt;
    const std::string lower = lower_copy(prompt);

    auto from_after = [&](const std::string& marker) -> bool {
        const size_t pos = lower.find(marker);
        if (pos == std::string::npos)
            return false;
        query = prompt.substr(pos + marker.size());
        return true;
    };

    if (!from_after("for "))
        from_after("of ");

    std::string qlower = lower_copy(query);
    const std::vector<std::string> cut_markers = { " and import", " then import", " and open", " and load", " on printables", " from printables" };
    for (const std::string& marker : cut_markers) {
        const size_t cut = qlower.find(marker);
        if (cut != std::string::npos) {
            query = query.substr(0, cut);
            break;
        }
    }

    query = trim_copy_local(query);
    if (query.empty())
        query = "iphone 16 pro case";
    return query;
}

bool prompt_explicitly_requests_undo_or_redo(const std::string& prompt, const bool for_undo)
{
    const std::string p = lower_copy(prompt);
    if (for_undo) {
        return p.find("undo") != std::string::npos ||
               p.find("revert") != std::string::npos ||
               p.find("go back") != std::string::npos ||
               p.find("reverse that") != std::string::npos;
    }
    return p.find("redo") != std::string::npos;
}

bool prompt_contains_any_ci(const std::string& prompt, const std::vector<std::string>& needles)
{
    for (const std::string& needle : needles) {
        if (contains_ci(prompt, needle))
            return true;
    }
    return false;
}

bool prompt_explicitly_requests_delete_or_remove(const std::string& prompt)
{
    return prompt_contains_any_ci(prompt, {
        "delete", "remove", "clear all", "purge", "erase", "trash"
    });
}

bool prompt_explicitly_requests_new_project(const std::string& prompt)
{
    return prompt_contains_any_ci(prompt, {
        "new project", "start new project", "start over", "reset project", "clear project"
    });
}

bool prompt_explicitly_requests_open_or_import(const std::string& prompt)
{
    return prompt_contains_any_ci(prompt, {
        "open", "load", "import", "replace with stl", "reload from disk", "from disk"
    });
}

bool prompt_explicitly_requests_save_or_export(const std::string& prompt)
{
    return prompt_contains_any_ci(prompt, {
        "save", "export", "write file", "output file", "gcode file", "stl", "obj"
    });
}

bool prompt_explicitly_requests_send_or_connect(const std::string& prompt)
{
    return prompt_contains_any_ci(prompt, {
        "send", "upload", "connect", "print host", "start print"
    });
}

bool is_destructive_action(const std::string& action_name)
{
    return action_name == "delete_selected" ||
           action_name == "remove_object" ||
           action_name == "delete_all_objects";
}

bool is_project_reset_action(const std::string& action_name)
{
    return action_name == "new_project";
}

bool is_disk_read_action(const std::string& action_name)
{
    return action_name == "open_project" ||
           action_name == "load_gcode_file" ||
           action_name == "import_model" ||
           action_name == "reload_selected_from_disk" ||
           action_name == "reload_all_from_disk";
}

bool is_disk_write_action(const std::string& action_name)
{
    return action_name == "save_project" ||
           action_name == "export_gcode" ||
           action_name == "export_all_gcodes" ||
           action_name == "export_stl";
}

bool is_printer_send_action(const std::string& action_name)
{
    return action_name == "send_gcode" ||
           action_name == "connect_gcode" ||
           action_name == "connect_gcode_all";
}

std::unordered_set<std::string> collect_allowed_action_names(const nlohmann::json& tools)
{
    std::unordered_set<std::string> out;
    if (!tools.is_array())
        return out;
    for (const nlohmann::json& tool : tools) {
        if (tool.is_object() && tool.contains("name") && tool["name"].is_string())
            out.insert(tool["name"].get<std::string>());
    }
    return out;
}

bool action_should_run_once_per_prompt(const std::string& action_name)
{
    // Printables import is heavy and side-effectful; executing it twice in one prompt cycle
    // can duplicate objects/downloads and destabilize the UI.
    return action_name == "import_printables_model";
}

std::string extract_setting_write_target(const ActionCall& call)
{
    if (!is_setting_write_action(call.name))
        return {};
    if (!call.params.is_object() || !call.params.contains("key") || !call.params["key"].is_string())
        return {};
    std::string key = call.params["key"].get<std::string>();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        if (c == '-' || c == ' ')
            return static_cast<char>('_');
        return static_cast<char>(std::tolower(c));
    });
    return call.name + ":" + key;
}

nlohmann::json capture_runtime_context(Plater& plater, const Settings& settings)
{
    nlohmann::json runtime = nlohmann::json::object();
    if (!settings.use_viewport_image_context)
        return runtime;

    const int image_size = std::max(128, std::min(1024, settings.viewport_image_size_px));

    GLCanvas3D* canvas = plater.canvas3D();
    if (canvas == nullptr) {
        runtime["viewport_image"] = nlohmann::json{{"available", false}, {"error", "3D canvas unavailable."}};
        return runtime;
    }

    ThumbnailData thumbnail;
    ThumbnailsParams thumbnail_params;
    thumbnail_params.printable_only = false;
    thumbnail_params.parts_only = false;
    thumbnail_params.show_bed = true;
    thumbnail_params.transparent_background = false;

    canvas->render_thumbnail(thumbnail,
                             static_cast<unsigned int>(image_size),
                             static_cast<unsigned int>(image_size),
                             thumbnail_params,
                             Camera::EType::Perspective);

    if (!thumbnail.is_valid()) {
        runtime["viewport_image"] = nlohmann::json{{"available", false}, {"error", "Thumbnail capture failed."}};
        return runtime;
    }

    std::unique_ptr<GCodeThumbnails::CompressedImageBuffer> compressed =
        GCodeThumbnails::compress_thumbnail(thumbnail, GCodeThumbnailsFormat::JPG);
    if (!compressed || compressed->data == nullptr || compressed->size == 0) {
        runtime["viewport_image"] = nlohmann::json{{"available", false}, {"error", "Image compression failed."}};
        return runtime;
    }

    std::string encoded;
    encoded.resize(boost::beast::detail::base64::encoded_size(compressed->size));
    encoded.resize(boost::beast::detail::base64::encode((void*)encoded.data(), (const void*)compressed->data, compressed->size));

    runtime["viewport_image"] = nlohmann::json{
        {"available", true},
        {"mime_type", "image/jpeg"},
        {"width", static_cast<int>(thumbnail.width)},
        {"height", static_cast<int>(thumbnail.height)},
        {"capture_mode", "current_viewport_as_is"},
        {"data_url", std::string("data:image/jpeg;base64,") + encoded}
    };

    return runtime;
}

} // namespace

AIController::AIController(Plater& plater)
    : m_plater(plater)
{
}

void AIController::reset_chat()
{
    m_chat_turns.clear();
    m_chat_summary.clear();
    m_cached_viewport_image = nlohmann::json::object();
}

void AIController::add_chat_turn(const std::string& role, const std::string& text)
{
    const std::string trimmed = truncate_for_history(text);
    if (trimmed.empty())
        return;
    m_chat_turns.push_back(ChatTurn{ role, trimmed });
}

void AIController::compact_chat_context()
{
    auto compute_turns_tokens = [this]() {
        size_t tokens = 0;
        for (const ChatTurn& turn : m_chat_turns)
            tokens += estimate_token_count(conversation_line(turn.role, turn.text));
        return tokens;
    };

    while (m_chat_turns.size() > kConversationMaxRecentTurns) {
        const ChatTurn oldest = m_chat_turns.front();
        append_summary_line(m_chat_summary, oldest.role, oldest.text);
        m_chat_turns.erase(m_chat_turns.begin());
    }

    while (!m_chat_turns.empty() &&
           estimate_token_count(m_chat_summary) + compute_turns_tokens() > kConversationContextTokenBudget) {
        const ChatTurn oldest = m_chat_turns.front();
        append_summary_line(m_chat_summary, oldest.role, oldest.text);
        m_chat_turns.erase(m_chat_turns.begin());
    }

    while (!m_chat_summary.empty() && estimate_token_count(m_chat_summary) > kConversationSummaryTokenBudget) {
        const size_t newline = m_chat_summary.find('\n');
        if (newline == std::string::npos) {
            m_chat_summary = truncate_for_history(m_chat_summary, 700);
            break;
        }
        m_chat_summary.erase(0, newline + 1);
    }
}

nlohmann::json AIController::build_conversation_context() const
{
    nlohmann::json out = nlohmann::json::object();
    out["summary"] = m_chat_summary;

    nlohmann::json recent = nlohmann::json::array();
    size_t used_tokens = estimate_token_count(m_chat_summary);

    // Include newest turns first, then reverse to chronological order.
    for (auto it = m_chat_turns.rbegin(); it != m_chat_turns.rend(); ++it) {
        const std::string line = conversation_line(it->role, it->text);
        const size_t line_tokens = estimate_token_count(line);
        if (!recent.empty() && used_tokens + line_tokens > kConversationContextTokenBudget)
            break;
        recent.push_back(nlohmann::json{{"role", it->role}, {"text", it->text}});
        used_tokens += line_tokens;
    }
    std::reverse(recent.begin(), recent.end());

    out["recent_turns"] = std::move(recent);
    out["token_budget"] = nlohmann::json{
        {"target_max_tokens", static_cast<int>(kConversationContextTokenBudget)},
        {"estimated_used_tokens", static_cast<int>(used_tokens)}
    };
    return out;
}

nlohmann::json AIController::build_runtime_context(const Settings& settings)
{
    if (!settings.use_viewport_image_context) {
        m_cached_viewport_image = nlohmann::json::object();
        return nlohmann::json::object();
    }

    nlohmann::json runtime = nlohmann::json::object();
    if (m_cached_viewport_image.is_object() &&
        m_cached_viewport_image.value("available", false) &&
        m_cached_viewport_image.contains("data_url") &&
        m_cached_viewport_image["data_url"].is_string()) {
        runtime["viewport_image"] = m_cached_viewport_image;
        runtime["viewport_image"]["source"] = "chat_cache";
        return runtime;
    }

    runtime = capture_runtime_context(m_plater, settings);
    if (runtime.contains("viewport_image") &&
        runtime["viewport_image"].is_object() &&
        runtime["viewport_image"].value("available", false)) {
        m_cached_viewport_image = runtime["viewport_image"];
        runtime["viewport_image"]["source"] = "captured_now";
    }

    return runtime;
}

bool AIController::is_available(std::string& reason) const
{
    if (wxGetApp().app_config == nullptr) {
        reason = "Application configuration is not available.";
        return false;
    }

    const Settings settings = load_settings(*wxGetApp().app_config);
    if (!settings.has_api_key()) {
        reason = unavailable_message();
        return false;
    }

    reason.clear();
    return true;
}

ControllerResult AIController::process_prompt(const std::string& prompt, bool allow_actions)
{
    ControllerResult out;

    if (prompt.empty()) {
        out.error = "Prompt is empty.";
        return out;
    }
    if (prompt.size() > 12000) {
        out.error = "Prompt is too long. Please shorten it and try again.";
        return out;
    }

    if (wxGetApp().app_config == nullptr) {
        out.error = "Application configuration is not available.";
        return out;
    }

    const Settings settings = load_settings(*wxGetApp().app_config);
    if (!settings.has_api_key()) {
        out.error = unavailable_message();
        return out;
    }

    std::unique_ptr<IAIProvider> provider = make_provider(settings.provider);
    if (!provider) {
        out.error = "Unsupported AI provider: " + settings.provider;
        return out;
    }

    add_chat_turn("user", prompt);
    compact_chat_context();
    const nlohmann::json conversation_context = build_conversation_context();

    ActionRegistry registry(m_plater);
    const nlohmann::json tools = allow_actions ? registry.tool_definitions() : nlohmann::json::array();
    const std::unordered_set<std::string> allowed_action_names = collect_allowed_action_names(tools);

    const int max_rounds = allow_actions ? 10 : 1;
    const int max_actions_total = allow_actions ? 40 : 0;
    std::string last_nonempty_assistant_text;
    std::set<std::string> seen_action_signatures;
    std::unordered_set<std::string> written_setting_targets;
    std::unordered_set<std::string> successful_single_run_actions;
    nlohmann::json execution_history = nlohmann::json::array();

    for (int round = 0; round < max_rounds; ++round) {
        const nlohmann::json scene_snapshot = build_scene_snapshot(m_plater);
        const nlohmann::json runtime_context = build_runtime_context(settings);
        ProviderReply reply = provider->request_actions(settings, prompt, conversation_context, scene_snapshot, tools, execution_history, runtime_context, allow_actions);
        if (!reply.ok) {
            out.error = reply.error.empty() ? "AI provider request failed." : reply.error;
            add_chat_turn("assistant", out.error);
            compact_chat_context();
            return out;
        }

        if (!reply.assistant_text.empty())
            last_nonempty_assistant_text = reply.assistant_text;

        if (allow_actions &&
            reply.actions.empty() && round == 0 && prompt_mentions_printables(prompt) &&
            (prompt_requests_search(prompt) || prompt_requests_import(prompt))) {
            ActionCall fallback_call;
            if (prompt_requests_import(prompt)) {
                fallback_call.name = "import_printables_model";
                fallback_call.params = nlohmann::json::object({
                    {"query", extract_printables_query(prompt)},
                    {"selection_mode", "best_match"},
                    {"file_type", "stl"},
                    {"auto_load", true}
                });
            } else {
                fallback_call.name = "search_printables_models";
                fallback_call.params = nlohmann::json::object({
                    {"query", extract_printables_query(prompt)},
                    {"selection_mode", "best_match"},
                    {"limit", 5}
                });
            }
            reply.actions.push_back(std::move(fallback_call));
            if (last_nonempty_assistant_text.empty())
                last_nonempty_assistant_text = "Using Printables search and import now.";
        }

        if (!allow_actions)
            break;

        if (reply.actions.empty())
            break;

        bool round_executed_any_action = false;
        int  round_skipped_repeated_action_calls = 0;
        int  round_skipped_repeated_setting_updates = 0;
        int  round_skipped_blocked_undo_redo = 0;
        int  round_skipped_single_run_actions = 0;
        int  round_skipped_unlisted_actions = 0;
        int  round_skipped_missing_user_intent = 0;
        nlohmann::json round_record = nlohmann::json::object();
        round_record["round"] = round;
        round_record["actions"] = nlohmann::json::array();

        for (const ActionCall& call : reply.actions) {
            if (static_cast<int>(out.action_results.size()) >= max_actions_total)
                break;

            if (!allowed_action_names.empty() && allowed_action_names.find(call.name) == allowed_action_names.end()) {
                ++round_skipped_unlisted_actions;
                continue;
            }

            if (action_should_run_once_per_prompt(call.name) &&
                successful_single_run_actions.find(call.name) != successful_single_run_actions.end()) {
                ++round_skipped_single_run_actions;
                continue;
            }

            const std::string action_signature = call.name + ":" + call.params.dump();
            if (!seen_action_signatures.insert(action_signature).second) {
                ++round_skipped_repeated_action_calls;
                continue;
            }

            const std::string setting_target = extract_setting_write_target(call);
            if (!setting_target.empty() && written_setting_targets.find(setting_target) != written_setting_targets.end()) {
                ++round_skipped_repeated_setting_updates;
                continue;
            }

            if (call.name == "undo" && !prompt_explicitly_requests_undo_or_redo(prompt, true)) {
                ++round_skipped_blocked_undo_redo;
                continue;
            }
            if (call.name == "redo" && !prompt_explicitly_requests_undo_or_redo(prompt, false)) {
                ++round_skipped_blocked_undo_redo;
                continue;
            }

            if (is_destructive_action(call.name) && !prompt_explicitly_requests_delete_or_remove(prompt)) {
                ++round_skipped_missing_user_intent;
                continue;
            }
            if (is_project_reset_action(call.name) && !prompt_explicitly_requests_new_project(prompt)) {
                ++round_skipped_missing_user_intent;
                continue;
            }
            if (is_disk_read_action(call.name) && !prompt_explicitly_requests_open_or_import(prompt)) {
                ++round_skipped_missing_user_intent;
                continue;
            }
            if (is_disk_write_action(call.name) && !prompt_explicitly_requests_save_or_export(prompt)) {
                ++round_skipped_missing_user_intent;
                continue;
            }
            if (is_printer_send_action(call.name) && !prompt_explicitly_requests_send_or_connect(prompt)) {
                ++round_skipped_missing_user_intent;
                continue;
            }

            ActionResult action_result = registry.execute(call);
            BOOST_LOG_TRIVIAL(info) << "AI action executed name=" << action_result.name << " success=" << action_result.success;
            out.action_results.push_back(action_result);
            round_record["actions"].push_back(action_result.to_json());
            if (action_result.success && !setting_target.empty())
                written_setting_targets.insert(setting_target);
            if (action_result.success && action_should_run_once_per_prompt(call.name))
                successful_single_run_actions.insert(call.name);
            round_executed_any_action = true;
        }

        execution_history.push_back(round_record);

        if (!round_executed_any_action &&
            (round_skipped_repeated_action_calls > 0 || round_skipped_repeated_setting_updates > 0 ||
             round_skipped_blocked_undo_redo > 0 || round_skipped_single_run_actions > 0 ||
             round_skipped_unlisted_actions > 0 || round_skipped_missing_user_intent > 0)) {
            BOOST_LOG_TRIVIAL(info)
                << "AI round produced only guarded/skipped actions. repeated_calls="
                << round_skipped_repeated_action_calls
                << " repeated_setting_updates=" << round_skipped_repeated_setting_updates
                << " blocked_undo_redo=" << round_skipped_blocked_undo_redo
                << " single_run_actions=" << round_skipped_single_run_actions
                << " unlisted_actions=" << round_skipped_unlisted_actions
                << " missing_user_intent=" << round_skipped_missing_user_intent;
        }

        if (static_cast<int>(out.action_results.size()) >= max_actions_total) {
            last_nonempty_assistant_text = "Stopped after reaching the AI action safety limit.";
            break;
        }

        if (!round_executed_any_action)
            break;
    }

    if (!last_nonempty_assistant_text.empty())
        out.assistant_text = last_nonempty_assistant_text;
    else if (allow_actions)
        out.assistant_text = summarize_actions(out.action_results);
    else
        out.assistant_text = "I couldn't generate a response.";

    if (!out.action_results.empty())
        add_chat_turn("actions", summarize_actions_for_history(out.action_results));
    if (!out.assistant_text.empty())
        add_chat_turn("assistant", out.assistant_text);
    if (!out.error.empty())
        add_chat_turn("assistant", out.error);
    compact_chat_context();

    if (!out.action_results.empty()) {
        bool any_success = false;
        for (const ActionResult& action : out.action_results)
            any_success = any_success || action.success;
        out.success = any_success;
    } else {
        out.success = true;
    }

    return out;
}

} // namespace AI
} // namespace GUI
} // namespace Slic3r
