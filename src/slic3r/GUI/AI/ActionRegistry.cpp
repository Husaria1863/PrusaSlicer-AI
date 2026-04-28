#include "ActionRegistry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "../GUI_App.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI_ObjectList.hpp"
#include "../GUI_ObjectManipulation.hpp"
#include "../OptionsGroup.hpp"
#include "../Plater.hpp"
#include "SceneSnapshot.hpp"
#include "../Selection.hpp"
#include "../Tab.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ModelProcessing.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/ServiceConfig.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

double round_coord(const double value, const int decimals = 4)
{
    const double scale = std::pow(10.0, static_cast<double>(decimals));
    return std::round(value * scale) / scale;
}

enum class AnchorMode {
    Min,
    Center,
    Max
};

ActionResult make_error(const std::string& action_name, const std::string& message)
{
    ActionResult result;
    result.name = action_name;
    result.success = false;
    result.message = message;
    return result;
}

ActionResult make_success(const std::string& action_name, const std::string& message, const nlohmann::json& data = nlohmann::json::object())
{
    ActionResult result;
    result.name = action_name;
    result.success = true;
    result.message = message;
    result.data = data;
    return result;
}

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_setting_key_token(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        if (c == '-' || c == ' ')
            return static_cast<char>('_');
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string alias_print_setting_key(const std::string& normalized_key)
{
    if (normalized_key == "infill" || normalized_key == "infill_percent" || normalized_key == "infill_percentage")
        return "fill_density";
    if (normalized_key == "infill_pattern")
        return "fill_pattern";
    if (normalized_key == "print_speed" || normalized_key == "speed")
        return "perimeter_speed";
    if (normalized_key == "support" || normalized_key == "supports")
        return "support_material";
    if (normalized_key == "auto_support" ||
        normalized_key == "auto_supports" ||
        normalized_key == "auto_generated_support" ||
        normalized_key == "auto_generated_supports" ||
        normalized_key == "auto_support_material" ||
        normalized_key == "auto_generated_support_material")
        return "support_material_auto";
    if (normalized_key == "brim")
        return "brim_width";
    return {};
}

std::string alias_filament_setting_key(const std::string& normalized_key)
{
    if (normalized_key == "filament_temperature" ||
        normalized_key == "nozzle_temperature" ||
        normalized_key == "extruder_temperature")
        return "temperature";

    if (normalized_key == "filament_temperature_first_layer" ||
        normalized_key == "filament_first_layer_temperature" ||
        normalized_key == "nozzle_temperature_first_layer" ||
        normalized_key == "nozzle_first_layer_temperature" ||
        normalized_key == "extruder_temperature_first_layer" ||
        normalized_key == "extruder_first_layer_temperature" ||
        normalized_key == "first_layer_filament_temperature")
        return "first_layer_temperature";

    if (normalized_key == "filament_bed_temperature")
        return "bed_temperature";

    if (normalized_key == "filament_bed_temperature_first_layer" ||
        normalized_key == "filament_first_layer_bed_temperature" ||
        normalized_key == "bed_temperature_first_layer")
        return "first_layer_bed_temperature";

    return {};
}

bool resolve_setting_key_generic(const DynamicPrintConfig& config, const std::string& requested_key, std::string& resolved_key)
{
    if (config.def()->has(requested_key)) {
        resolved_key = requested_key;
        return true;
    }

    const std::string normalized = normalize_setting_key_token(requested_key);
    if (config.def()->has(normalized)) {
        resolved_key = normalized;
        return true;
    }

    const std::string aliased = alias_print_setting_key(normalized);
    if (!aliased.empty() && config.def()->has(aliased)) {
        resolved_key = aliased;
        return true;
    }

    const std::string filament_aliased = alias_filament_setting_key(normalized);
    if (!filament_aliased.empty() && config.def()->has(filament_aliased)) {
        resolved_key = filament_aliased;
        return true;
    }

    return false;
}

bool resolve_print_setting_key(const DynamicPrintConfig& config, const std::string& requested_key, std::string& resolved_key)
{
    return resolve_setting_key_generic(config, requested_key, resolved_key);
}

std::string object_display_name(const ModelObject& object)
{
    if (!object.name.empty())
        return object.name;
    if (!object.input_file.empty())
        return boost::filesystem::path(object.input_file).filename().string();
    return "Object";
}

bool parse_double(const nlohmann::json& value, double& out)
{
    if (value.is_number()) {
        out = value.get<double>();
        return true;
    }

    if (value.is_string()) {
        std::string text = value.get<std::string>();
        text.erase(std::remove(text.begin(), text.end(), '%'), text.end());
        try {
            out = std::stod(text);
            return true;
        } catch (...) {
            return false;
        }
    }

    return false;
}

bool parse_int(const nlohmann::json& value, int& out)
{
    if (value.is_number_integer()) {
        out = value.get<int>();
        return true;
    }

    if (value.is_number()) {
        out = static_cast<int>(std::llround(value.get<double>()));
        return true;
    }

    if (value.is_string()) {
        try {
            out = std::stoi(value.get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }

    return false;
}

bool parse_bool(const nlohmann::json& value, bool& out)
{
    if (value.is_boolean()) {
        out = value.get<bool>();
        return true;
    }

    if (value.is_number()) {
        out = value.get<double>() != 0.0;
        return true;
    }

    if (value.is_string()) {
        const std::string text = lower_copy(value.get<std::string>());
        if (text == "true" || text == "1" || text == "yes") {
            out = true;
            return true;
        }
        if (text == "false" || text == "0" || text == "no") {
            out = false;
            return true;
        }
    }

    return false;
}

constexpr const char* kPrintablesGraphqlEndpoint = "https://api.printables.com/graphql/";

struct PrintablesFileCandidate
{
    std::string id;
    std::string name;
    std::string download_type; // stl | gcode | sla | other | pack
    std::string group;
    std::string pack_type;
    long long   file_size{ 0 };
};

std::string json_to_string_id(const nlohmann::json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    return {};
}

long long json_to_size_bytes(const nlohmann::json& value)
{
    if (value.is_number_integer())
        return value.get<long long>();
    if (value.is_number_unsigned())
        return static_cast<long long>(value.get<unsigned long long>());
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

std::string summarize_graphql_errors(const nlohmann::json& errors)
{
    if (!errors.is_array() || errors.empty())
        return {};

    std::ostringstream os;
    bool first = true;
    for (const nlohmann::json& err : errors) {
        std::string message;
        if (err.is_object() && err.contains("message") && err["message"].is_string()) {
            message = err["message"].get<std::string>();
        } else if (err.is_object() && err.contains("messages") && err["messages"].is_array() && !err["messages"].empty() && err["messages"].front().is_string()) {
            message = err["messages"].front().get<std::string>();
        }

        if (message.empty())
            continue;

        if (!first)
            os << " | ";
        first = false;
        os << message;
    }

    return os.str();
}

bool post_printables_graphql(const nlohmann::json& payload, nlohmann::json& response_json, std::string& out_error)
{
    std::string response_body;
    std::string transport_error;
    unsigned    http_status = 0;

    const HttpRetryOpt retry_opts{
        std::chrono::milliseconds(400),
        std::chrono::milliseconds(3000),
        2
    };

    Http::post(kPrintablesGraphqlEndpoint)
        .header("Content-Type", "application/json")
        .timeout_connect(15)
        .timeout_max(45)
        .set_post_body(payload.dump())
        .on_complete([&response_body, &http_status](std::string body, unsigned status) {
            response_body = std::move(body);
            http_status = status;
        })
        .on_error([&response_body, &transport_error, &http_status](std::string body, std::string error, unsigned status) {
            response_body = std::move(body);
            transport_error = std::move(error);
            http_status = status;
        })
        .perform_sync(retry_opts);

    if (!transport_error.empty()) {
        out_error = "Network error while contacting Printables: " + transport_error;
        return false;
    }

    if (http_status >= 400) {
        out_error = "Printables API returned HTTP " + std::to_string(http_status) + ".";
        try {
            const nlohmann::json parsed = nlohmann::json::parse(response_body);
            const std::string gql_error = summarize_graphql_errors(parsed.value("errors", nlohmann::json::array()));
            if (!gql_error.empty())
                out_error += " " + gql_error;
        } catch (...) {
            // Keep base message on parse failure.
        }
        return false;
    }

    try {
        response_json = nlohmann::json::parse(response_body);
    } catch (const std::exception& e) {
        out_error = std::string("Invalid Printables API response JSON: ") + e.what();
        return false;
    }

    if (response_json.contains("errors") && response_json["errors"].is_array() && !response_json["errors"].empty()) {
        const std::string gql_error = summarize_graphql_errors(response_json["errors"]);
        out_error = gql_error.empty() ? "Printables API returned GraphQL errors." : gql_error;
        return false;
    }

    return true;
}

std::string normalize_printables_selection_mode(std::string token)
{
    token = lower_copy(token);
    if (token.empty() || token == "auto" || token == "default" || token == "best_match" || token == "relevant" || token == "most_relevant")
        return "best_match";
    if (token == "most_downloaded" || token == "downloads" || token == "downloaded")
        return "most_downloaded";
    if (token == "top_rated" || token == "rated" || token == "highest_rated")
        return "top_rated";
    return {};
}

std::string printables_selection_mode_to_graphql_ordering(const std::string& mode)
{
    if (mode == "best_match")
        return "best_match";
    if (mode == "most_downloaded")
        return "downloads_count";
    if (mode == "top_rated")
        return "rating_avg";
    return "best_match";
}

bool query_printables_search_models(const std::string& query,
                                    int limit,
                                    const std::string& selection_mode,
                                    nlohmann::json& out_items,
                                    int& out_total_count,
                                    std::string& out_error)
{
    static const char* kSearchQuery = R"graphql(
query SearchModels($query: String!, $limit: Int, $cursor: Int, $ordering: SearchChoicesEnum) {
  result: searchPrints2(
    query: $query
    printType: print
    limit: $limit
    offset: $cursor
    ordering: $ordering
  ) {
    items {
      id
      name
      slug
      ratingAvg
      likesCount
      downloadCount
      price
      club: premium
      user {
        handle
        publicUsername
      }
    }
    totalCount
  }
}
)graphql";

    const std::string ordering = printables_selection_mode_to_graphql_ordering(selection_mode);
    nlohmann::json payload{
        { "query", kSearchQuery },
        { "variables", nlohmann::json{
            { "query", query },
            { "limit", limit },
            { "cursor", 0 },
            { "ordering", ordering }
        }}
    };

    nlohmann::json root;
    if (!post_printables_graphql(payload, root, out_error))
        return false;

    const nlohmann::json data = root.value("data", nlohmann::json::object());
    const nlohmann::json result = data.value("result", nlohmann::json::object());
    if (!result.contains("items") || !result["items"].is_array()) {
        out_error = "Printables search response did not include model items.";
        return false;
    }

    out_items = result["items"];
    out_total_count = 0;
    if (result.contains("totalCount")) {
        if (result["totalCount"].is_number_integer())
            out_total_count = result["totalCount"].get<int>();
        else if (result["totalCount"].is_string()) {
            try {
                out_total_count = std::stoi(result["totalCount"].get<std::string>());
            } catch (...) {
                out_total_count = static_cast<int>(out_items.size());
            }
        }
    }
    if (out_total_count <= 0)
        out_total_count = static_cast<int>(out_items.size());

    return true;
}

bool query_printables_model_files(const std::string& model_id, nlohmann::json& out_model, std::string& out_error)
{
    static const char* kModelFilesQuery = R"graphql(
query ModelFiles($id: ID!) {
  model: print(id: $id) {
    id
    stls {
      id
      name
      fileSize
    }
    gcodes {
      id
      name
      fileSize
    }
    slas {
      id
      name
      fileSize
    }
    otherFiles {
      id
      name
      fileSize
    }
    downloadPacks {
      id
      name
      fileSize
      fileType
    }
  }
}
)graphql";

    nlohmann::json payload{
        { "query", kModelFilesQuery },
        { "variables", nlohmann::json{{"id", model_id}} }
    };

    nlohmann::json root;
    if (!post_printables_graphql(payload, root, out_error))
        return false;

    const nlohmann::json data = root.value("data", nlohmann::json::object());
    if (!data.contains("model") || !data["model"].is_object()) {
        out_error = "Printables did not return file metadata for the selected model.";
        return false;
    }

    out_model = data["model"];
    return true;
}

std::string summarize_download_link_errors(const nlohmann::json& result_node)
{
    if (!result_node.is_object() || !result_node.contains("errors") || !result_node["errors"].is_array())
        return {};
    return summarize_graphql_errors(result_node["errors"]);
}

bool query_printables_download_link(const std::string& file_id,
                                    const std::string& model_id,
                                    const std::string& file_type,
                                    std::string& out_link,
                                    std::string& out_error)
{
    static const char* kGetDownloadLinkMutation = R"graphql(
mutation GetDownloadLink($id: ID!, $modelId: ID!, $fileType: DownloadFileTypeEnum!, $source: DownloadSourceEnum!) {
  getDownloadLink(
    id: $id
    printId: $modelId
    fileType: $fileType
    source: $source
  ) {
    ok
    errors {
      field
      messages
    }
    output {
      link
      ttl
      count
    }
  }
}
)graphql";

    nlohmann::json payload{
        { "query", kGetDownloadLinkMutation },
        { "variables", nlohmann::json{
            { "id", file_id },
            { "modelId", model_id },
            { "fileType", file_type },
            { "source", "model_detail" }
        }}
    };

    nlohmann::json root;
    if (!post_printables_graphql(payload, root, out_error))
        return false;

    const nlohmann::json data = root.value("data", nlohmann::json::object());
    if (!data.contains("getDownloadLink") || !data["getDownloadLink"].is_object()) {
        out_error = "Printables did not return a download link payload.";
        return false;
    }

    const nlohmann::json result = data["getDownloadLink"];
    if (!result.value("ok", false)) {
        const std::string reason = summarize_download_link_errors(result);
        out_error = reason.empty() ? "Printables rejected the download link request." : reason;
        return false;
    }

    const nlohmann::json output = result.value("output", nlohmann::json::object());
    if (!output.contains("link") || !output["link"].is_string()) {
        out_error = "Printables download link response did not include a usable URL.";
        return false;
    }

    out_link = output["link"].get<std::string>();
    return true;
}

std::string normalize_printables_file_type(std::string token)
{
    token = lower_copy(token);
    if (token.empty() || token == "auto" || token == "default")
        return "auto";
    if (token == "stl" || token == "model" || token == "models")
        return "stl";
    if (token == "gcode" || token == "print" || token == "gcodes")
        return "gcode";
    if (token == "sla" || token == "resin")
        return "sla";
    if (token == "other" || token == "other_file" || token == "other_files")
        return "other";
    if (token == "pack" || token == "zip" || token == "bundle")
        return "pack";
    return {};
}

std::string printables_model_relative_path(const std::string& model_id, const std::string& model_slug)
{
    if (model_slug.empty())
        return "/model/" + model_id;
    return "/model/" + model_id + "-" + model_slug;
}

bool parse_printables_model_reference(const std::string& input, std::string& out_model_id, std::string& out_model_slug)
{
    static const std::regex kUrlRegex(R"((?:https?://[^/]+)?/model/([0-9]+)(?:-([^/?#]+))?)", std::regex::icase);
    std::smatch match;
    if (std::regex_search(input, match, kUrlRegex)) {
        out_model_id = match[1].str();
        out_model_slug = match.size() > 2 ? match[2].str() : std::string();
        return !out_model_id.empty();
    }

    static const std::regex kIdOnlyRegex(R"(^\s*([0-9]+)(?:-([A-Za-z0-9._~-]+))?\s*$)");
    if (std::regex_match(input, match, kIdOnlyRegex)) {
        out_model_id = match[1].str();
        out_model_slug = match.size() > 2 ? match[2].str() : std::string();
        return !out_model_id.empty();
    }

    return false;
}

void append_candidates_from_array(const nlohmann::json& model_node,
                                  const char* field_name,
                                  const char* download_type,
                                  std::vector<PrintablesFileCandidate>& out)
{
    if (!model_node.contains(field_name) || !model_node[field_name].is_array())
        return;

    for (const nlohmann::json& item : model_node[field_name]) {
        if (!item.is_object())
            continue;
        const std::string id = json_to_string_id(item.value("id", nlohmann::json()));
        if (id.empty())
            continue;

        PrintablesFileCandidate candidate;
        candidate.id = id;
        candidate.name = item.value("name", std::string("file_" + id));
        candidate.download_type = download_type;
        candidate.group = field_name;
        candidate.file_size = json_to_size_bytes(item.value("fileSize", nlohmann::json()));
        if (item.contains("fileType") && item["fileType"].is_string())
            candidate.pack_type = item["fileType"].get<std::string>();
        out.push_back(std::move(candidate));
    }
}

std::vector<PrintablesFileCandidate> collect_printables_candidates(const nlohmann::json& model_node)
{
    std::vector<PrintablesFileCandidate> out;
    append_candidates_from_array(model_node, "stls", "stl", out);
    append_candidates_from_array(model_node, "slas", "sla", out);
    append_candidates_from_array(model_node, "gcodes", "gcode", out);
    append_candidates_from_array(model_node, "otherFiles", "other", out);
    append_candidates_from_array(model_node, "downloadPacks", "pack", out);
    return out;
}

std::vector<PrintablesFileCandidate> select_printables_candidates_by_type(const std::vector<PrintablesFileCandidate>& all, const std::string& requested_type)
{
    std::vector<PrintablesFileCandidate> out;
    if (requested_type == "auto") {
        const std::array<const char*, 5> priority = {"stl", "sla", "gcode", "other", "pack"};
        for (const char* type : priority) {
            for (const auto& candidate : all) {
                if (candidate.download_type == type)
                    out.push_back(candidate);
            }
        }
        return out;
    }

    for (const auto& candidate : all) {
        if (candidate.download_type == requested_type)
            out.push_back(candidate);
    }
    return out;
}

bool parse_axis(const nlohmann::json& value, Axis& axis)
{
    if (!value.is_string())
        return false;

    const std::string axis_name = lower_copy(value.get<std::string>());
    if (axis_name == "x") {
        axis = Axis::X;
        return true;
    }
    if (axis_name == "y") {
        axis = Axis::Y;
        return true;
    }
    if (axis_name == "z") {
        axis = Axis::Z;
        return true;
    }
    return false;
}

bool parse_conversion_type(const nlohmann::json& value, ConversionType& conversion_type)
{
    if (!value.is_string())
        return false;

    const std::string token = lower_copy(value.get<std::string>());
    if (token == "imperial" || token == "inch" || token == "inches") {
        conversion_type = ConversionType::CONV_FROM_INCH;
        return true;
    }
    if (token == "meters" || token == "meter" || token == "metres" || token == "metre") {
        conversion_type = ConversionType::CONV_FROM_METER;
        return true;
    }
    return false;
}

std::string normalize_shape_name(const nlohmann::json& value, bool& ok)
{
    ok = true;
    if (!value.is_string()) {
        ok = false;
        return {};
    }

    const std::string token = lower_copy(value.get<std::string>());
    if (token == "box" || token == "cube")
        return "Box";
    if (token == "cylinder")
        return "Cylinder";
    if (token == "sphere")
        return "Sphere";
    if (token == "slab")
        return "Slab";
    if (token == "gallery")
        return "Gallery";

    ok = false;
    return {};
}

int axis_to_index(Axis axis)
{
    switch (axis) {
    case Axis::X: return 0;
    case Axis::Y: return 1;
    case Axis::Z: return 2;
    default:      return 2;
    }
}

std::string axis_to_string(Axis axis)
{
    switch (axis) {
    case Axis::X: return "x";
    case Axis::Y: return "y";
    case Axis::Z: return "z";
    default:      return "z";
    }
}

nlohmann::json vec3_to_json(const Vec3d& v)
{
    return nlohmann::json::array({ round_coord(v(0)), round_coord(v(1)), round_coord(v(2)) });
}

nlohmann::json bbox_to_json(const BoundingBoxf3& bbox)
{
    const Vec3d bb_min = bbox.min.cast<double>();
    const Vec3d bb_max = bbox.max.cast<double>();
    const Vec3d bb_size = bbox.size().cast<double>();
    const Vec3d bb_center = bbox.center().cast<double>();
    return nlohmann::json{
        {"min", vec3_to_json(bb_min)},
        {"max", vec3_to_json(bb_max)},
        {"size", vec3_to_json(bb_size)},
        {"center", vec3_to_json(bb_center)}
    };
}

bool parse_anchor_mode(const nlohmann::json& value, AnchorMode& anchor)
{
    if (!value.is_string())
        return false;

    const std::string token = lower_copy(value.get<std::string>());
    if (token == "min") {
        anchor = AnchorMode::Min;
        return true;
    }
    if (token == "center" || token == "centre") {
        anchor = AnchorMode::Center;
        return true;
    }
    if (token == "max") {
        anchor = AnchorMode::Max;
        return true;
    }
    return false;
}

double bbox_axis_coordinate(const BoundingBoxf3& bbox, Axis axis, AnchorMode anchor)
{
    const int idx = axis_to_index(axis);
    switch (anchor) {
    case AnchorMode::Min:    return bbox.min(idx);
    case AnchorMode::Center: return bbox.center()(idx);
    case AnchorMode::Max:    return bbox.max(idx);
    default:                 return bbox.center()(idx);
    }
}

Transform3d cut_rotation_for_axis(Axis axis)
{
    switch (axis) {
    case Axis::X: return Geometry::rotation_transform(Vec3d(0.0, 90.0 * kDegToRad, 0.0));
    case Axis::Y: return Geometry::rotation_transform(Vec3d(-90.0 * kDegToRad, 0.0, 0.0));
    case Axis::Z: return Transform3d::Identity();
    default:      return Transform3d::Identity();
    }
}

std::string normalize_compact_token(std::string value)
{
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string arrange_geometry_handling_name(arr2::ArrangeSettingsView::GeometryHandling value)
{
    switch (value) {
    case arr2::ArrangeSettingsView::ghConvex:   return "convex";
    case arr2::ArrangeSettingsView::ghBalanced: return "balanced";
    case arr2::ArrangeSettingsView::ghAdvanced: return "advanced";
    default:                                    return "unknown";
    }
}

std::string arrange_strategy_name(arr2::ArrangeSettingsView::ArrangeStrategy value)
{
    switch (value) {
    case arr2::ArrangeSettingsView::asAuto:         return "auto";
    case arr2::ArrangeSettingsView::asPullToCenter: return "pulltocenter";
    default:                                        return "unknown";
    }
}

std::string arrange_xl_pivot_name(arr2::ArrangeSettingsView::XLPivots value)
{
    switch (value) {
    case arr2::ArrangeSettingsView::xlpCenter:     return "center";
    case arr2::ArrangeSettingsView::xlpRearLeft:   return "rearleft";
    case arr2::ArrangeSettingsView::xlpFrontLeft:  return "frontleft";
    case arr2::ArrangeSettingsView::xlpFrontRight: return "frontright";
    case arr2::ArrangeSettingsView::xlpRearRight:  return "rearright";
    case arr2::ArrangeSettingsView::xlpRandom:     return "random";
    default:                                       return "unknown";
    }
}

bool resolve_object_index(const nlohmann::json& params, const Model& model, int& resolved_index, std::string& error)
{
    if (params.contains("object_index")) {
        int object_index = -1;
        if (!parse_int(params["object_index"], object_index)) {
            error = "Parameter object_index must be an integer.";
            return false;
        }
        if (object_index < 0 || object_index >= static_cast<int>(model.objects.size())) {
            error = "object_index is out of range.";
            return false;
        }
        resolved_index = object_index;
        return true;
    }

    if (!params.contains("object_name") || !params["object_name"].is_string()) {
        error = "Expected either object_index or object_name.";
        return false;
    }

    const std::string query = params["object_name"].get<std::string>();
    if (query.empty()) {
        error = "object_name cannot be empty.";
        return false;
    }

    const std::string query_lower = lower_copy(query);

    std::vector<int> exact_matches;
    std::vector<int> partial_matches;

    for (size_t idx = 0; idx < model.objects.size(); ++idx) {
        const ModelObject* object = model.objects[idx];
        if (object == nullptr)
            continue;

        const std::string name = object_display_name(*object);
        const std::string name_lower = lower_copy(name);

        if (name_lower == query_lower)
            exact_matches.push_back(static_cast<int>(idx));
        if (name_lower.find(query_lower) != std::string::npos)
            partial_matches.push_back(static_cast<int>(idx));
    }

    const std::vector<int>& matches = !exact_matches.empty() ? exact_matches : partial_matches;

    if (matches.empty()) {
        error = "No object matched object_name='" + query + "'.";
        return false;
    }

    if (matches.size() > 1) {
        std::ostringstream os;
        os << "Ambiguous object_name='" << query << "'. Matching object indices: ";
        for (size_t i = 0; i < matches.size(); ++i) {
            if (i > 0)
                os << ", ";
            os << matches[i];
        }
        error = os.str();
        return false;
    }

    resolved_index = matches.front();
    return true;
}

bool resolve_key_by_def(const ConfigDef& def, const std::string& requested_key, std::string& resolved_key)
{
    if (def.has(requested_key)) {
        resolved_key = requested_key;
        return true;
    }

    const std::string normalized = normalize_setting_key_token(requested_key);
    if (def.has(normalized)) {
        resolved_key = normalized;
        return true;
    }

    return false;
}

bool resolve_key_in_model_config(const ModelConfig& config, const std::string& requested_key, std::string& resolved_key)
{
    if (config.has(requested_key)) {
        resolved_key = requested_key;
        return true;
    }

    const std::string normalized = normalize_setting_key_token(requested_key);
    if (config.has(normalized)) {
        resolved_key = normalized;
        return true;
    }

    return false;
}

std::string json_to_config_value_string(const nlohmann::json& in)
{
    if (in.is_string())
        return in.get<std::string>();
    if (in.is_boolean())
        return in.get<bool>() ? "1" : "0";
    if (in.is_number_integer())
        return std::to_string(in.get<long long>());
    if (in.is_number_unsigned())
        return std::to_string(in.get<unsigned long long>());
    if (in.is_number_float()) {
        std::ostringstream os;
        os << std::setprecision(16) << in.get<double>();
        return os.str();
    }
    if (in.is_array()) {
        std::ostringstream os;
        for (size_t i = 0; i < in.size(); ++i) {
            if (i > 0)
                os << ",";
            os << json_to_config_value_string(in[i]);
        }
        return os.str();
    }
    return in.dump();
}

bool is_sensitive_key(const std::string& key_raw)
{
    const std::string key = lower_copy(key_raw);
    return key.find("api_key") != std::string::npos ||
           key.find("token") != std::string::npos ||
           key.find("secret") != std::string::npos ||
           key.find("password") != std::string::npos;
}

std::string maybe_redact_value(const std::string& key, const std::string& value)
{
    if (!is_sensitive_key(key))
        return value;
    return value.empty() ? std::string() : std::string("***");
}

bool resolve_object_index_or_selected(const nlohmann::json& params, Plater& plater, int& resolved_index, std::string& error)
{
    if (params.contains("object_index") || params.contains("object_name"))
        return resolve_object_index(params, plater.model(), resolved_index, error);

    const int selected = plater.get_selection().get_object_idx();
    if (selected < 0 || selected >= static_cast<int>(plater.model().objects.size())) {
        error = "No object target specified and no valid object selected.";
        return false;
    }

    resolved_index = selected;
    return true;
}

std::string volume_display_name(const ModelVolume& volume, int volume_index)
{
    if (!volume.name.empty())
        return volume.name;
    return "Volume " + std::to_string(volume_index);
}

bool resolve_volume_index(const nlohmann::json& params, const ModelObject& object, int& resolved_index, std::string& error)
{
    if (params.contains("volume_index")) {
        int volume_index = -1;
        if (!parse_int(params["volume_index"], volume_index)) {
            error = "Parameter volume_index must be an integer.";
            return false;
        }
        if (volume_index < 0 || volume_index >= static_cast<int>(object.volumes.size())) {
            error = "volume_index is out of range.";
            return false;
        }
        resolved_index = volume_index;
        return true;
    }

    if (params.contains("volume_name")) {
        if (!params["volume_name"].is_string()) {
            error = "Parameter volume_name must be a string.";
            return false;
        }
        const std::string query = params["volume_name"].get<std::string>();
        if (query.empty()) {
            error = "volume_name cannot be empty.";
            return false;
        }

        const std::string query_lc = lower_copy(query);
        std::vector<int> exact_matches;
        std::vector<int> partial_matches;

        for (size_t idx = 0; idx < object.volumes.size(); ++idx) {
            const ModelVolume* volume = object.volumes[idx];
            if (volume == nullptr)
                continue;

            const std::string name_lc = lower_copy(volume_display_name(*volume, static_cast<int>(idx)));
            if (name_lc == query_lc)
                exact_matches.push_back(static_cast<int>(idx));
            if (name_lc.find(query_lc) != std::string::npos)
                partial_matches.push_back(static_cast<int>(idx));
        }

        const std::vector<int>& matches = !exact_matches.empty() ? exact_matches : partial_matches;
        if (matches.empty()) {
            error = "No volume matched volume_name='" + query + "'.";
            return false;
        }
        if (matches.size() > 1) {
            std::ostringstream os;
            os << "Ambiguous volume_name='" << query << "'. Matching indices: ";
            for (size_t i = 0; i < matches.size(); ++i) {
                if (i > 0)
                    os << ", ";
                os << matches[i];
            }
            error = os.str();
            return false;
        }

        resolved_index = matches.front();
        return true;
    }

    if (object.volumes.size() == 1) {
        resolved_index = 0;
        return true;
    }

    error = "Expected volume_index or volume_name (object has multiple volumes).";
    return false;
}

PresetCollection* resolve_preset_collection_for_domain(const std::string& domain, Plater& plater)
{
    if (wxGetApp().preset_bundle == nullptr)
        return nullptr;

    const std::string normalized = lower_copy(domain);
    if (normalized == "print")
        return plater.printer_technology() == ptFFF ? &wxGetApp().preset_bundle->prints : &wxGetApp().preset_bundle->sla_prints;
    if (normalized == "filament")
        return plater.printer_technology() == ptFFF ? &wxGetApp().preset_bundle->filaments : &wxGetApp().preset_bundle->sla_materials;
    if (normalized == "material")
        return &wxGetApp().preset_bundle->sla_materials;
    if (normalized == "printer")
        return &wxGetApp().preset_bundle->printers;
    return nullptr;
}

Tab* resolve_tab_for_domain(const std::string& domain, Plater& plater)
{
    const std::string normalized = lower_copy(domain);
    if (normalized == "print")
        return wxGetApp().get_tab(plater.printer_technology() == ptFFF ? Preset::TYPE_PRINT : Preset::TYPE_SLA_PRINT);
    if (normalized == "filament")
        return wxGetApp().get_tab(plater.printer_technology() == ptFFF ? Preset::TYPE_FILAMENT : Preset::TYPE_SLA_MATERIAL);
    if (normalized == "material")
        return wxGetApp().get_tab(Preset::TYPE_SLA_MATERIAL);
    if (normalized == "printer")
        return wxGetApp().get_tab(Preset::TYPE_PRINTER);
    return nullptr;
}

std::string normalize_preset_domain(const std::string& domain, Plater& plater)
{
    const std::string normalized = lower_copy(domain);
    if (normalized == "auto")
        return plater.printer_technology() == ptFFF ? "filament" : "material";
    return normalized;
}

bool set_config_option_via_deserialize(DynamicPrintConfig& config, const std::string& key, const std::string& value_text, std::string& error)
{
    try {
        config.set_deserialize_strict(key, value_text);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool value_is_numeric_like(const nlohmann::json& value, double& numeric, bool& has_percent)
{
    has_percent = false;
    if (value.is_number()) {
        numeric = value.get<double>();
        return true;
    }

    if (!value.is_string())
        return false;

    std::string text = value.get<std::string>();
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; }), text.end());
    if (text.empty())
        return false;

    if (text.find('%') != std::string::npos) {
        has_percent = true;
        text.erase(std::remove(text.begin(), text.end(), '%'), text.end());
    }

    if (text.empty())
        return false;

    try {
        numeric = std::stod(text);
        return true;
    } catch (...) {
        return false;
    }
}

std::string format_compact_double(double value)
{
    std::ostringstream os;
    os << std::setprecision(12) << value;
    return os.str();
}

bool normalize_setting_value_for_def(const ConfigOptionDef& def,
                                     const std::string& key,
                                     const nlohmann::json& original_value,
                                     std::string& out_value_text,
                                     std::string& out_note)
{
    out_note.clear();
    nlohmann::json normalized_value = original_value;

    const bool scalar_numeric_type =
        def.type == coInt || def.type == coFloat || def.type == coPercent || def.type == coFloatOrPercent;

    if (scalar_numeric_type) {
        double numeric = 0.0;
        bool has_percent = false;
        if (value_is_numeric_like(original_value, numeric, has_percent)) {
            const double min_allowed = static_cast<double>(def.min);
            const double max_allowed = static_cast<double>(def.max);
            const bool has_min_bound = def.min > -FLT_MAX / 2.0f;
            const bool has_max_bound = def.max < FLT_MAX / 2.0f;
            const double clamped = std::min(has_max_bound ? max_allowed : numeric, std::max(has_min_bound ? min_allowed : numeric, numeric));
            const bool changed = std::fabs(clamped - numeric) > 1e-9;

            if (def.type == coInt) {
                const int clamped_int = static_cast<int>(std::llround(clamped));
                normalized_value = clamped_int;
            } else if ((def.type == coPercent || def.type == coFloatOrPercent) && has_percent) {
                normalized_value = format_compact_double(clamped) + "%";
            } else {
                normalized_value = clamped;
            }

            if (changed) {
                out_note = "Value for '" + key + "' was clamped to allowed range ["
                    + format_compact_double(min_allowed) + ", " + format_compact_double(max_allowed) + "].";
            }
        }
    }

    out_value_text = json_to_config_value_string(normalized_value);
    return true;
}

void refresh_object_manipulation_panel()
{
    if (ObjectManipulation* manip = wxGetApp().obj_manipul(); manip != nullptr) {
        manip->set_dirty();
        manip->update_if_dirty();
    }
}

bool is_blank_text(const std::string& value)
{
    return value.empty() || std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

struct NormalAreaBucket
{
    Vec3d  normal{ Vec3d::UnitZ() };
    double area{ 0.0 };
};

bool compute_largest_placeable_face_normal(const ModelObject& object,
                                           int instance_index,
                                           Vec3d& out_normal,
                                           double& out_area,
                                           std::string& out_error)
{
    if (instance_index < 0 || instance_index >= static_cast<int>(object.instances.size())) {
        out_error = "Selected instance index is out of range.";
        return false;
    }

    const ModelInstance* instance = object.instances[instance_index];
    if (instance == nullptr) {
        out_error = "Selected instance is unavailable.";
        return false;
    }

    TriangleMesh hull_mesh;
    bool have_geometry = false;

    for (const ModelVolume* volume : object.volumes) {
        if (volume == nullptr || volume->type() != ModelVolumeType::MODEL_PART)
            continue;

        TriangleMesh volume_hull = volume->get_convex_hull();
        if (volume_hull.facets_count() <= 0)
            continue;

        volume_hull.transform(volume->get_matrix());

        if (!have_geometry) {
            hull_mesh = std::move(volume_hull);
            have_geometry = true;
        } else {
            hull_mesh.merge(volume_hull);
        }
    }

    if (!have_geometry || hull_mesh.facets_count() <= 0) {
        out_error = "Object has no model-part geometry suitable for place-on-face.";
        return false;
    }

    hull_mesh = hull_mesh.convex_hull_3d();
    if (hull_mesh.facets_count() <= 0) {
        out_error = "Failed to compute convex hull geometry for place-on-face.";
        return false;
    }

    const Transform3d instance_matrix = instance->get_matrix_no_offset();
    const std::vector<Vec3f> face_normals = its_face_normals(hull_mesh.its);

    if (face_normals.empty() || hull_mesh.its.indices.empty()) {
        out_error = "Convex hull has no faces.";
        return false;
    }

    constexpr double kJoinDotThreshold = 0.9995;
    std::vector<NormalAreaBucket> buckets;
    buckets.reserve(face_normals.size());

    for (size_t facet_idx = 0; facet_idx < hull_mesh.its.indices.size(); ++facet_idx) {
        if (facet_idx >= face_normals.size())
            break;

        Vec3d face_normal = face_normals[facet_idx].cast<double>();
        const double normal_norm = face_normal.norm();
        if (normal_norm < 1e-12)
            continue;
        face_normal /= normal_norm;

        const Vec3i face = hull_mesh.its.indices[facet_idx];
        if (face(0) < 0 || face(1) < 0 || face(2) < 0)
            continue;
        if (face(0) >= static_cast<int>(hull_mesh.its.vertices.size()) ||
            face(1) >= static_cast<int>(hull_mesh.its.vertices.size()) ||
            face(2) >= static_cast<int>(hull_mesh.its.vertices.size()))
            continue;

        const Vec3d v0 = hull_mesh.its.vertices[face(0)].cast<double>();
        const Vec3d v1 = hull_mesh.its.vertices[face(1)].cast<double>();
        const Vec3d v2 = hull_mesh.its.vertices[face(2)].cast<double>();
        const Vec3d w0 = instance_matrix * v0;
        const Vec3d w1 = instance_matrix * v1;
        const Vec3d w2 = instance_matrix * v2;
        const double area_world = 0.5 * ((w1 - w0).cross(w2 - w0)).norm();
        if (area_world <= 1e-9)
            continue;

        int matched_bucket = -1;
        double matched_dot = kJoinDotThreshold;
        for (size_t i = 0; i < buckets.size(); ++i) {
            const double dot = buckets[i].normal.dot(face_normal);
            if (dot > matched_dot) {
                matched_dot = dot;
                matched_bucket = static_cast<int>(i);
            }
        }

        if (matched_bucket < 0) {
            buckets.push_back(NormalAreaBucket{ face_normal, area_world });
        } else {
            NormalAreaBucket& bucket = buckets[matched_bucket];
            const double new_area = bucket.area + area_world;
            Vec3d merged_normal = bucket.normal * bucket.area + face_normal * area_world;
            const double merged_norm = merged_normal.norm();
            if (merged_norm > 1e-12)
                merged_normal /= merged_norm;
            bucket.normal = merged_normal;
            bucket.area = new_area;
        }
    }

    if (buckets.empty()) {
        out_error = "Could not identify any valid face normals for place-on-face.";
        return false;
    }

    const auto best_it = std::max_element(buckets.begin(), buckets.end(), [](const NormalAreaBucket& a, const NormalAreaBucket& b) {
        return a.area < b.area;
    });

    if (best_it == buckets.end() || best_it->normal.norm() < 1e-12) {
        out_error = "Failed to resolve the largest face normal.";
        return false;
    }

    out_normal = best_it->normal.normalized();
    out_area = best_it->area;
    return true;
}

struct CutExecutionOptions
{
    bool keep_upper{ true };
    bool keep_lower{ true };
    bool keep_as_parts{ false };
    bool place_on_cut_upper{ true };
    bool place_on_cut_lower{ false };
    bool flip_upper{ false };
    bool flip_lower{ false };
};

ActionResult execute_cut_selected_world_plane(Plater& plater,
                                              const std::string& action_name,
                                              Axis axis,
                                              double plane_axis_world,
                                              const CutExecutionOptions& options)
{
    Selection& selection = plater.canvas3D()->get_selection();
    if (selection.is_empty())
        return make_error(action_name, "No selection. Select one object instance first.");

    const int object_index = selection.get_object_idx();
    const int instance_index = selection.get_instance_idx();
    if (object_index < 0 || instance_index < 0)
        return make_error(action_name, "Cut requires a single object instance selection.");
    if (object_index >= static_cast<int>(plater.model().objects.size()))
        return make_error(action_name, "Selected object index is out of range.");

    ModelObject* object = plater.model().objects[object_index];
    if (object == nullptr)
        return make_error(action_name, "Selected object is unavailable.");
    if (instance_index >= static_cast<int>(object->instances.size()))
        return make_error(action_name, "Selected instance index is out of range.");
    if (!options.keep_upper && !options.keep_lower)
        return make_error(action_name, "At least one of keep_upper or keep_lower must be true.");

    const ModelInstance* instance = object->instances[instance_index];
    if (instance == nullptr)
        return make_error(action_name, "Selected instance is unavailable.");

    Vec3d plane_center = selection.get_bounding_box().center();
    plane_center(axis_to_index(axis)) = plane_axis_world;
    const Vec3d cut_center_offset = plane_center - instance->get_offset();

    const Transform3d cut_matrix = Geometry::translation_transform(cut_center_offset) * cut_rotation_for_axis(axis);

    ModelObjectCutAttributes attributes =
        only_if(options.keep_upper, ModelObjectCutAttribute::KeepUpper) |
        only_if(options.keep_lower, ModelObjectCutAttribute::KeepLower) |
        only_if(options.keep_as_parts, ModelObjectCutAttribute::KeepAsParts) |
        only_if(options.place_on_cut_upper, ModelObjectCutAttribute::PlaceOnCutUpper) |
        only_if(options.place_on_cut_lower, ModelObjectCutAttribute::PlaceOnCutLower) |
        only_if(options.flip_upper, ModelObjectCutAttribute::FlipUpper) |
        only_if(options.flip_lower, ModelObjectCutAttribute::FlipLower);

    plater.take_snapshot(std::string("AI: Cut selected by plane"));
    Cut cut(object, instance_index, cut_matrix, attributes);
    const ModelObjectPtrs& new_objects = cut.perform_with_plane();
    if (new_objects.empty())
        return make_error(action_name, "Cut operation produced no resulting objects.");

    plater.apply_cut_object_to_model(static_cast<size_t>(object_index), new_objects);
    refresh_object_manipulation_panel();
    plater.update();

    return make_success(action_name, "Cut completed on selected object.",
                        nlohmann::json{
                            {"source_object_index", object_index},
                            {"source_instance_index", instance_index},
                            {"axis", axis_to_string(axis)},
                            {"plane_axis_world_mm", round_coord(plane_axis_world)},
                            {"result_object_count", static_cast<int>(new_objects.size())}
                        });
}

Tab* resolve_settings_tab(Plater& plater, const nlohmann::json& params, std::string& domain_out)
{
    domain_out = params.value("domain", std::string("auto"));
    const std::string normalized = lower_copy(domain_out);

    if (normalized == "print" || normalized == "auto") {
        domain_out = "print";
        const Preset::Type tab_type = plater.printer_technology() == ptFFF ? Preset::TYPE_PRINT : Preset::TYPE_SLA_PRINT;
        return wxGetApp().get_tab(tab_type);
    }

    if (normalized == "filament" || normalized == "material") {
        domain_out = normalized == "material" ? "material" : "filament";
        const Preset::Type tab_type = plater.printer_technology() == ptFFF ? Preset::TYPE_FILAMENT : Preset::TYPE_SLA_MATERIAL;
        return wxGetApp().get_tab(tab_type);
    }

    if (normalized == "printer") {
        domain_out = "printer";
        return wxGetApp().get_tab(Preset::TYPE_PRINTER);
    }

    return nullptr;
}

ActionResult set_setting_for_tab(Plater& plater,
                                 const nlohmann::json& params,
                                 const std::string& action_name,
                                 const std::string& snapshot_name,
                                 Tab* tab)
{
    if (!params.contains("key") || !params["key"].is_string())
        return make_error(action_name, "Parameter 'key' is required and must be a string.");
    if (!params.contains("value"))
        return make_error(action_name, "Parameter 'value' is required.");
    if (tab == nullptr || tab->get_config() == nullptr)
        return make_error(action_name, "Settings tab configuration is unavailable.");

    DynamicPrintConfig* config = tab->get_config();
    const std::string requested_key = params["key"].get<std::string>();

    std::string key;
    if (!resolve_setting_key_generic(*config, requested_key, key))
        return make_error(action_name, "Unknown setting key: " + requested_key);

    const ConfigOptionDef* def = config->option_def(key);
    if (def == nullptr)
        return make_error(action_name, "Cannot read setting definition for key: " + key);

    std::string value_text;
    std::string value_note;
    normalize_setting_value_for_def(*def, key, params["value"], value_text, value_note);

    plater.take_snapshot(snapshot_name);
    std::string deserialize_error;
    if (!set_config_option_via_deserialize(*config, key, value_text, deserialize_error))
        return make_error(action_name, "Failed to set setting '" + key + "': " + deserialize_error);

    std::string dependency_note;
    bool bool_value = false;
    if (parse_bool(params["value"], bool_value)) {
        if (key == "support_material_auto" &&
            bool_value &&
            config->def()->has("support_material") &&
            !config->opt_bool("support_material")) {
            if (!set_config_option_via_deserialize(*config, "support_material", "1", deserialize_error))
                return make_error(action_name, "Failed to set dependent setting 'support_material': " + deserialize_error);
            dependency_note = " Enabled 'support_material' because auto-generated supports require supports to be enabled.";
        } else if (key == "support_material" &&
                   !bool_value &&
                   config->def()->has("support_material_auto") &&
                   config->opt_bool("support_material_auto")) {
            if (!set_config_option_via_deserialize(*config, "support_material_auto", "0", deserialize_error))
                return make_error(action_name, "Failed to set dependent setting 'support_material_auto': " + deserialize_error);
            dependency_note = " Disabled 'support_material_auto' because supports were turned off.";
        }
    }

    tab->update_dirty();
    tab->reload_config();
    tab->refresh_sidebar_frequently_changed_parameters();
    plater.update();

    const ConfigOption* opt = config->option(key);
    const std::string normalized = opt ? opt->serialize() : value_text;
    std::string message = "Set setting '" + key + "' to " + normalized + ".";
    if (!value_note.empty())
        message += " " + value_note;
    if (!dependency_note.empty())
        message += dependency_note;
    return make_success(action_name, message);
}

} // namespace

ActionRegistry::ActionRegistry(Plater& plater)
    : m_plater(plater)
{
    register_actions();
}

nlohmann::json ActionRegistry::tool_definitions() const
{
    nlohmann::json definitions = nlohmann::json::array();

    for (const auto& item : m_actions) {
        nlohmann::json tool;
        tool["name"] = item.first;
        tool["description"] = item.second.description;
        tool["parameters"] = item.second.schema;
        definitions.push_back(std::move(tool));
    }

    return definitions;
}

ActionResult ActionRegistry::execute(const ActionCall& call)
{
    const auto it = m_actions.find(call.name);
    if (it == m_actions.end())
        return make_error(call.name, "Unknown action: " + call.name);

    try {
        return it->second.executor(call.params.is_object() ? call.params : nlohmann::json::object());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "AI action failed: " << call.name << " error=" << e.what();
        return make_error(call.name, std::string("Action execution failed: ") + e.what());
    }
}

void ActionRegistry::register_actions()
{
    m_actions["get_scene_state"] = ActionDescriptor {
        "Return a structured snapshot of the current PrusaSlicer scene.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            return make_success("get_scene_state", "Scene state collected.", build_scene_snapshot(m_plater));
        }
    };

    m_actions["list_objects"] = ActionDescriptor {
        "List loaded objects with indices and instance counts.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            nlohmann::json objects = nlohmann::json::array();
            const Model& model = m_plater.model();
            for (size_t idx = 0; idx < model.objects.size(); ++idx) {
                const ModelObject* object = model.objects[idx];
                if (object == nullptr)
                    continue;

                objects.push_back(nlohmann::json{
                    { "index", static_cast<int>(idx) },
                    { "name", object_display_name(*object) },
                    { "instance_count", static_cast<int>(object->instances.size()) },
                    { "volume_count", static_cast<int>(object->volumes.size()) }
                });
            }
            return make_success("list_objects", "Objects listed.", nlohmann::json{{"objects", std::move(objects)}});
        }
    };

    m_actions["rename_object"] = ActionDescriptor {
        "Rename an object by index/name (or current selection).",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"new_name"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"new_name", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("new_name") || !params["new_name"].is_string())
                return make_error("rename_object", "Parameter new_name is required and must be a string.");

            const std::string new_name = params["new_name"].get<std::string>();
            if (is_blank_text(new_name))
                return make_error("rename_object", "new_name cannot be empty.");

            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("rename_object", error);

            ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("rename_object", "Object is unavailable.");

            const std::string old_name = object_display_name(*object);
            if (old_name == new_name)
                return make_success("rename_object", "Object name is already set.", nlohmann::json{{"object_index", object_index}, {"name", new_name}});

            m_plater.take_snapshot(std::string("AI: Rename object"));
            object->name = new_name;
            if (object->is_text() && !object->volumes.empty() && object->volumes.front() != nullptr)
                object->volumes.front()->name = new_name;

            m_plater.schedule_background_process();
            m_plater.object_list_changed();
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("rename_object",
                                "Renamed object from '" + old_name + "' to '" + new_name + "'.",
                                nlohmann::json{{"object_index", object_index}, {"old_name", old_name}, {"new_name", new_name}});
        }
    };

    m_actions["rename_volume"] = ActionDescriptor {
        "Rename a volume/sub-object on a resolved object.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"new_name"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"volume_index", nlohmann::json{{"type", "integer"}}},
                {"volume_name", nlohmann::json{{"type", "string"}}},
                {"new_name", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("new_name") || !params["new_name"].is_string())
                return make_error("rename_volume", "Parameter new_name is required and must be a string.");

            const std::string new_name = params["new_name"].get<std::string>();
            if (is_blank_text(new_name))
                return make_error("rename_volume", "new_name cannot be empty.");

            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("rename_volume", error);

            ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("rename_volume", "Object is unavailable.");

            int volume_index = -1;
            if (!resolve_volume_index(params, *object, volume_index, error))
                return make_error("rename_volume", error);

            ModelVolume* volume = object->volumes[volume_index];
            if (volume == nullptr)
                return make_error("rename_volume", "Volume is unavailable.");
            if (volume->is_text())
                return make_error("rename_volume", "Renaming text volumes is not supported by native object-list flow.");

            const std::string old_name = volume_display_name(*volume, volume_index);
            if (old_name == new_name)
                return make_success("rename_volume", "Volume name is already set.",
                                    nlohmann::json{{"object_index", object_index}, {"volume_index", volume_index}, {"name", new_name}});

            m_plater.take_snapshot(std::string("AI: Rename sub-object"));
            volume->name = new_name;
            if (object->volumes.size() == 1)
                object->name = new_name;

            m_plater.schedule_background_process();
            m_plater.object_list_changed();
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("rename_volume",
                                "Renamed volume from '" + old_name + "' to '" + new_name + "'.",
                                nlohmann::json{{"object_index", object_index}, {"volume_index", volume_index}, {"old_name", old_name}, {"new_name", new_name}});
        }
    };

    m_actions["get_selection_state"] = ActionDescriptor {
        "Return currently selected object/instance details.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            const Selection& selection = m_plater.get_selection();
            nlohmann::json data{
                { "is_empty", selection.is_empty() },
                { "object_index", selection.get_object_idx() },
                { "instance_index", selection.get_instance_idx() },
                { "volume_count", static_cast<int>(selection.volumes_count()) },
                { "is_single_full_object", selection.is_single_full_object() },
                { "is_single_full_instance", selection.is_single_full_instance() },
                { "is_single_volume", selection.is_single_volume() }
            };
            return make_success("get_selection_state", "Selection state collected.", data);
        }
    };

    m_actions["get_selection_geometry"] = ActionDescriptor {
        "Return compact geometry for current selection bounding box.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            const Selection& selection = m_plater.get_selection();
            if (selection.is_empty())
                return make_error("get_selection_geometry", "No selection.");

            const BoundingBoxf3 bbox = selection.get_bounding_box();
            nlohmann::json axes = nlohmann::json::object();
            for (Axis axis : {Axis::X, Axis::Y, Axis::Z}) {
                axes[axis_to_string(axis)] = nlohmann::json{
                    {"min", round_coord(bbox_axis_coordinate(bbox, axis, AnchorMode::Min))},
                    {"center", round_coord(bbox_axis_coordinate(bbox, axis, AnchorMode::Center))},
                    {"max", round_coord(bbox_axis_coordinate(bbox, axis, AnchorMode::Max))},
                    {"size", round_coord(bbox.size()(axis_to_index(axis)))}
                };
            }

            return make_success("get_selection_geometry", "Selection geometry collected.",
                                nlohmann::json{
                                    {"bbox_world", bbox_to_json(bbox)},
                                    {"axes", std::move(axes)}
                                });
        }
    };

    m_actions["get_object_geometry"] = ActionDescriptor {
        "Return compact geometry digest for a resolved object and optionally one instance.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"instance_index", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("get_object_geometry", error);

            const ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("get_object_geometry", "Object is unavailable.");

            nlohmann::json out{
                {"object_index", object_index},
                {"name", object_display_name(*object)},
                {"instance_count", static_cast<int>(object->instances.size())},
                {"volume_count", static_cast<int>(object->volumes.size())},
                {"facets_count", static_cast<int>(object->facets_count())},
                {"raw_mesh_bbox", bbox_to_json(object->raw_mesh_bounding_box())},
                {"object_bbox_world", bbox_to_json(object->bounding_box_exact())}
            };

            if (params.contains("instance_index")) {
                int instance_index = -1;
                if (!parse_int(params["instance_index"], instance_index))
                    return make_error("get_object_geometry", "instance_index must be an integer.");
                if (instance_index < 0 || instance_index >= static_cast<int>(object->instances.size()))
                    return make_error("get_object_geometry", "instance_index is out of range.");

                const ModelInstance* instance = object->instances[instance_index];
                out["instance"] = nlohmann::json{
                    {"index", instance_index},
                    {"offset", vec3_to_json(instance->get_offset())},
                    {"rotation", vec3_to_json(instance->get_rotation())},
                    {"scale", vec3_to_json(instance->get_scaling_factor())},
                    {"bbox_world", bbox_to_json(object->instance_bounding_box(instance_index))}
                };
            } else {
                nlohmann::json instances = nlohmann::json::array();
                for (size_t i = 0; i < object->instances.size(); ++i) {
                    instances.push_back(nlohmann::json{
                        {"index", static_cast<int>(i)},
                        {"bbox_world", bbox_to_json(object->instance_bounding_box(i))}
                    });
                }
                out["instances"] = std::move(instances);
            }

            return make_success("get_object_geometry", "Object geometry collected.", out);
        }
    };

    m_actions["import_model"] = ActionDescriptor {
        "Import a model from a filesystem path.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"path"})},
            {"properties", nlohmann::json{{"path", nlohmann::json{{"type", "string"}, {"description", "Absolute or relative path to model file."}}}}},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("path") || !params["path"].is_string())
                return make_error("import_model", "Parameter 'path' is required and must be a string.");

            const std::string path_str = params["path"].get<std::string>();
            const boost::filesystem::path input_path(path_str);
            if (!boost::filesystem::exists(input_path))
                return make_error("import_model", "Model path does not exist: " + path_str);

            m_plater.take_snapshot(std::string("AI: Import model"));
            const std::vector<size_t> loaded = m_plater.load_files(std::vector<boost::filesystem::path>{ input_path }, true, false, false);

            if (loaded.empty())
                return make_error("import_model", "No model was imported from: " + path_str);

            nlohmann::json loaded_indices = nlohmann::json::array();
            for (const size_t idx : loaded)
                loaded_indices.push_back(static_cast<int>(idx));

            const int selected_idx = static_cast<int>(loaded.front());
            Selection& selection = m_plater.canvas3D()->get_selection();
            selection.clear();
            selection.add_object(static_cast<unsigned int>(selected_idx), true);
            if (wxGetApp().obj_list() != nullptr)
                wxGetApp().obj_list()->update_selections();
            m_plater.canvas3D()->update_gizmos_on_off_state();
            refresh_object_manipulation_panel();

            m_plater.update();
            return make_success("import_model",
                                "Imported model from " + input_path.string() + " and selected object " + std::to_string(selected_idx) + ".",
                                nlohmann::json{{"loaded_object_indices", loaded_indices}, {"selected_object_index", selected_idx}});
        }
    };

    m_actions["search_printables_models"] = ActionDescriptor {
        "Search Printables models by query and return compact results.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"query"})},
            {"properties", nlohmann::json{
                {"query", nlohmann::json{{"type", "string"}}},
                {"limit", nlohmann::json{{"type", "integer"}, {"minimum", 1}, {"maximum", 30}}},
                {"selection_mode", nlohmann::json{{"type", "string"}, {"description", "best_match|most_downloaded|top_rated"}}},
                {"open_tab", nlohmann::json{{"type", "boolean"}, {"description", "Open Printables search tab in-app."}}}
            }},
            {"additionalProperties", false}
        },
        [](const nlohmann::json& params) {
            if (!params.contains("query") || !params["query"].is_string())
                return make_error("search_printables_models", "Parameter 'query' is required and must be a string.");

            const std::string query = params["query"].get<std::string>();
            if (is_blank_text(query))
                return make_error("search_printables_models", "query cannot be empty.");

            int limit = 8;
            if (params.contains("limit")) {
                if (!parse_int(params["limit"], limit))
                    return make_error("search_printables_models", "limit must be an integer.");
                limit = std::max(1, std::min(30, limit));
            }

            std::string selection_mode = "best_match";
            if (params.contains("selection_mode")) {
                if (!params["selection_mode"].is_string())
                    return make_error("search_printables_models", "selection_mode must be a string.");
                selection_mode = normalize_printables_selection_mode(params["selection_mode"].get<std::string>());
                if (selection_mode.empty())
                    return make_error("search_printables_models", "Unsupported selection_mode. Use: best_match, most_downloaded, top_rated.");
            }

            bool open_tab = false;
            if (params.contains("open_tab") && !parse_bool(params["open_tab"], open_tab))
                return make_error("search_printables_models", "open_tab must be a boolean.");

            nlohmann::json items;
            int            total_count = 0;
            std::string    api_error;
            if (!query_printables_search_models(query, limit, selection_mode, items, total_count, api_error))
                return make_error("search_printables_models", "Printables search failed: " + api_error);

            nlohmann::json results = nlohmann::json::array();
            for (const nlohmann::json& item : items) {
                if (!item.is_object())
                    continue;

                const std::string id = json_to_string_id(item.value("id", nlohmann::json()));
                if (id.empty())
                    continue;

                const std::string slug = item.value("slug", std::string());
                const std::string relative_path = printables_model_relative_path(id, slug);
                const nlohmann::json user = item.value("user", nlohmann::json::object());

                results.push_back(nlohmann::json{
                    {"id", id},
                    {"name", item.value("name", std::string())},
                    {"slug", slug},
                    {"model_path", relative_path},
                    {"model_url", Utils::ServiceConfig::instance().printables_url() + relative_path},
                    {"download_count", item.value("downloadCount", 0)},
                    {"likes_count", item.value("likesCount", 0)},
                    {"rating_avg", item.value("ratingAvg", std::string())},
                    {"price", item.contains("price") ? item["price"] : nlohmann::json(nullptr)},
                    {"club", item.value("club", false)},
                    {"author_handle", user.value("handle", std::string())},
                    {"author_name", user.value("publicUsername", std::string())}
                });
            }

            if (open_tab) {
                const std::string search_path = "/search/models?q=" + Http::url_encode(query);
                wxGetApp().open_link_in_printables(search_path);
            }

            const std::string msg = results.empty()
                ? "No Printables models found for query '" + query + "'."
                : "Found " + std::to_string(results.size()) + " Printables models for query '" + query + "'.";

            return make_success("search_printables_models",
                                msg,
                                nlohmann::json{
                                    {"query", query},
                                    {"selection_mode", selection_mode},
                                    {"returned_count", static_cast<int>(results.size())},
                                    {"total_count", total_count},
                                    {"results", std::move(results)}
                                });
        }
    };

    m_actions["import_printables_model"] = ActionDescriptor {
        "Search Printables or resolve a model URL/id and import one model file automatically.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"query", nlohmann::json{{"type", "string"}}},
                {"model_url", nlohmann::json{{"type", "string"}, {"description", "Printables model URL or /model/<id>-<slug> path."}}},
                {"model_id", nlohmann::json{{"type", "string"}, {"description", "Printables numeric model id, for example \"3161\"."}}},
                {"model_slug", nlohmann::json{{"type", "string"}}},
                {"result_index", nlohmann::json{{"type", "integer"}, {"minimum", 0}, {"description", "Used with query; picks Nth result."}}},
                {"selection_mode", nlohmann::json{{"type", "string"}, {"description", "best_match|most_downloaded|top_rated. Applies when using query."}}},
                {"file_type", nlohmann::json{{"type", "string"}, {"description", "auto|stl|gcode|sla|other|pack"}}},
                {"file_index", nlohmann::json{{"type", "integer"}, {"minimum", 0}, {"description", "Pick Nth file from chosen type."}}},
                {"auto_load", nlohmann::json{{"type", "boolean"}, {"description", "If true, auto-load downloaded model into scene."}}},
                {"open_tab", nlohmann::json{{"type", "boolean"}, {"description", "Open model page in Printables tab."}}}
            }},
            {"additionalProperties", false}
        },
        [](const nlohmann::json& params) {
            std::string model_id;
            std::string model_slug;
            std::string model_name;

            int result_index = 0;
            if (params.contains("result_index")) {
                if (!parse_int(params["result_index"], result_index))
                    return make_error("import_printables_model", "result_index must be an integer.");
                if (result_index < 0)
                    return make_error("import_printables_model", "result_index cannot be negative.");
            }

            int file_index = 0;
            if (params.contains("file_index")) {
                if (!parse_int(params["file_index"], file_index))
                    return make_error("import_printables_model", "file_index must be an integer.");
                if (file_index < 0)
                    return make_error("import_printables_model", "file_index cannot be negative.");
            }

            bool auto_load = true;
            if (params.contains("auto_load") && !parse_bool(params["auto_load"], auto_load))
                return make_error("import_printables_model", "auto_load must be a boolean.");

            bool open_tab = false;
            if (params.contains("open_tab") && !parse_bool(params["open_tab"], open_tab))
                return make_error("import_printables_model", "open_tab must be a boolean.");

            std::string selection_mode = "best_match";
            if (params.contains("selection_mode")) {
                if (!params["selection_mode"].is_string())
                    return make_error("import_printables_model", "selection_mode must be a string.");
                selection_mode = normalize_printables_selection_mode(params["selection_mode"].get<std::string>());
                if (selection_mode.empty())
                    return make_error("import_printables_model", "Unsupported selection_mode. Use: best_match, most_downloaded, top_rated.");
            }

            std::string requested_file_type = "auto";
            if (params.contains("file_type")) {
                if (!params["file_type"].is_string())
                    return make_error("import_printables_model", "file_type must be a string.");
                requested_file_type = params["file_type"].get<std::string>();
            }
            requested_file_type = normalize_printables_file_type(requested_file_type);
            if (requested_file_type.empty())
                return make_error("import_printables_model", "Unsupported file_type. Use one of: auto, stl, gcode, sla, other, pack.");

            if (params.contains("model_id")) {
                model_id = json_to_string_id(params["model_id"]);
                if (model_id.empty())
                    return make_error("import_printables_model", "model_id must be a string or integer.");
            }

            if (params.contains("model_slug")) {
                if (!params["model_slug"].is_string())
                    return make_error("import_printables_model", "model_slug must be a string.");
                model_slug = params["model_slug"].get<std::string>();
            }

            if (params.contains("model_url")) {
                if (!params["model_url"].is_string())
                    return make_error("import_printables_model", "model_url must be a string.");
                std::string parsed_id;
                std::string parsed_slug;
                if (!parse_printables_model_reference(params["model_url"].get<std::string>(), parsed_id, parsed_slug))
                    return make_error("import_printables_model", "Could not parse model_url. Expected /model/<id>-<slug>.");
                if (model_id.empty())
                    model_id = parsed_id;
                if (model_slug.empty())
                    model_slug = parsed_slug;
            }

            if (model_id.empty()) {
                if (!params.contains("query") || !params["query"].is_string())
                    return make_error("import_printables_model", "Provide either query, model_url, or model_id.");

                const std::string query = params["query"].get<std::string>();
                if (is_blank_text(query))
                    return make_error("import_printables_model", "query cannot be empty.");

                const int search_limit = std::max(8, std::min(30, result_index + 1));
                nlohmann::json items;
                int            total_count = 0;
                std::string    api_error;
                if (!query_printables_search_models(query, search_limit, selection_mode, items, total_count, api_error))
                    return make_error("import_printables_model", "Printables search failed: " + api_error);
                if (items.empty())
                    return make_error("import_printables_model", "No Printables models found for query '" + query + "'.");
                if (result_index >= static_cast<int>(items.size()))
                    return make_error("import_printables_model",
                                      "result_index is out of range for available results (" + std::to_string(items.size()) + ").");

                const nlohmann::json model_item = items[result_index];
                model_id = json_to_string_id(model_item.value("id", nlohmann::json()));
                model_slug = model_item.value("slug", std::string());
                model_name = model_item.value("name", std::string());
                if (model_id.empty())
                    return make_error("import_printables_model", "Selected Printables search result is missing model id.");
            }

            nlohmann::json model_files;
            std::string files_error;
            if (!query_printables_model_files(model_id, model_files, files_error))
                return make_error("import_printables_model", "Could not resolve model files on Printables: " + files_error);

            const std::vector<PrintablesFileCandidate> all_candidates = collect_printables_candidates(model_files);
            if (all_candidates.empty())
                return make_error("import_printables_model", "No downloadable files were found for this Printables model.");

            const std::vector<PrintablesFileCandidate> selected_pool =
                select_printables_candidates_by_type(all_candidates, requested_file_type);
            if (selected_pool.empty()) {
                std::vector<std::string> available;
                for (const char* type : {"stl", "sla", "gcode", "other", "pack"}) {
                    bool found = false;
                    for (const auto& candidate : all_candidates) {
                        if (candidate.download_type == type) {
                            found = true;
                            break;
                        }
                    }
                    if (found)
                        available.emplace_back(type);
                }

                std::ostringstream os;
                os << "Requested file_type '" << requested_file_type << "' is not available.";
                if (!available.empty()) {
                    os << " Available: ";
                    for (size_t i = 0; i < available.size(); ++i) {
                        if (i > 0)
                            os << ", ";
                        os << available[i];
                    }
                    os << ".";
                }
                return make_error("import_printables_model", os.str());
            }

            if (file_index >= static_cast<int>(selected_pool.size()))
                return make_error("import_printables_model",
                                  "file_index is out of range for selected file_type (" + std::to_string(selected_pool.size()) + ").");

            const PrintablesFileCandidate file = selected_pool[file_index];

            std::string download_url;
            std::string link_error;
            if (!query_printables_download_link(file.id, model_id, file.download_type, download_url, link_error))
                return make_error("import_printables_model", "Failed to get secure file URL from Printables: " + link_error);

            const std::string model_path = printables_model_relative_path(model_id, model_slug);
            const std::string model_url = Utils::ServiceConfig::instance().printables_url() + model_path;

            if (auto_load)
                wxGetApp().printables_slice_request(download_url, model_path);
            else
                wxGetApp().printables_download_request(download_url, model_path);

            if (open_tab)
                wxGetApp().open_link_in_printables(model_path);

            const std::string display_model = !model_name.empty() ? model_name : ("model " + model_id);
            const std::string message = auto_load
                ? "Started Printables import for '" + file.name + "' from " + display_model + "."
                : "Started Printables download for '" + file.name + "' from " + display_model + ".";

            return make_success("import_printables_model",
                                message,
                                nlohmann::json{
                                    {"model_id", model_id},
                                    {"model_slug", model_slug},
                                    {"model_name", model_name},
                                    {"model_path", model_path},
                                    {"model_url", model_url},
                                    {"selection_mode", selection_mode},
                                    {"requested_file_type", requested_file_type},
                                    {"selected_file", nlohmann::json{
                                        {"id", file.id},
                                        {"name", file.name},
                                        {"type", file.download_type},
                                        {"group", file.group},
                                        {"pack_type", file.pack_type},
                                        {"file_size", file.file_size}
                                    }},
                                    {"selected_file_index", file_index},
                                    {"auto_load", auto_load},
                                    {"download_url", download_url}
                                });
        }
    };

    m_actions["select_object"] = ActionDescriptor {
        "Select an object by index or object name.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index(params, m_plater.model(), object_index, error))
                return make_error("select_object", error);

            Selection& selection = m_plater.canvas3D()->get_selection();
            selection.clear();
            selection.add_object(static_cast<unsigned int>(object_index), true);

            wxGetApp().obj_list()->update_selections();
            m_plater.canvas3D()->update_gizmos_on_off_state();
            refresh_object_manipulation_panel();
            m_plater.update();

            const ModelObject* object = m_plater.model().objects[object_index];
            return make_success("select_object", "Selected object " + std::to_string(object_index) + " (" + object_display_name(*object) + ").");
        }
    };

    m_actions["select_instance"] = ActionDescriptor {
        "Select an instance by object and instance index.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"instance_index"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"instance_index", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index(params, m_plater.model(), object_index, error))
                return make_error("select_instance", error);

            int instance_index = -1;
            if (!parse_int(params["instance_index"], instance_index))
                return make_error("select_instance", "Parameter instance_index must be an integer.");

            const ModelObject* object = m_plater.model().objects[object_index];
            if (instance_index < 0 || instance_index >= static_cast<int>(object->instances.size()))
                return make_error("select_instance", "instance_index is out of range for the resolved object.");

            Selection& selection = m_plater.canvas3D()->get_selection();
            selection.clear();
            selection.add_instance(static_cast<unsigned int>(object_index), static_cast<unsigned int>(instance_index), true);

            wxGetApp().obj_list()->update_selections();
            m_plater.canvas3D()->update_gizmos_on_off_state();
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("select_instance",
                                "Selected instance " + std::to_string(instance_index) + " of object " + std::to_string(object_index) + ".");
        }
    };

    m_actions["move_selected"] = ActionDescriptor {
        "Move current selection by x/y/z millimeters.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"x", nlohmann::json{{"type", "number"}}},
                {"y", nlohmann::json{{"type", "number"}}},
                {"z", nlohmann::json{{"type", "number"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("move_selected", "No selection. Select an object first.");

            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            if (params.contains("x") && !parse_double(params["x"], x))
                return make_error("move_selected", "Parameter x must be numeric.");
            if (params.contains("y") && !parse_double(params["y"], y))
                return make_error("move_selected", "Parameter y must be numeric.");
            if (params.contains("z") && !parse_double(params["z"], z))
                return make_error("move_selected", "Parameter z must be numeric.");

            selection.setup_cache();
            selection.translate(Vec3d(x, y, z), TransformationType::World_Relative_Joint);
            m_plater.canvas3D()->do_move("AI: Move selected");
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("move_selected", "Moved selection by [" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + "] mm.");
        }
    };

    m_actions["move_selected_to"] = ActionDescriptor {
        "Move selection anchor to exact world coordinates for one or more axes.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"x", nlohmann::json{{"type", "number"}}},
                {"y", nlohmann::json{{"type", "number"}}},
                {"z", nlohmann::json{{"type", "number"}}},
                {"anchor", nlohmann::json{{"type", "string"}, {"description", "min|center|max, default center"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("move_selected_to", "No selection. Select an object first.");

            AnchorMode anchor = AnchorMode::Center;
            if (params.contains("anchor") && !parse_anchor_mode(params["anchor"], anchor))
                return make_error("move_selected_to", "anchor must be one of: min, center, max.");

            const BoundingBoxf3 bbox = selection.get_bounding_box();
            Vec3d delta = Vec3d::Zero();
            bool any_axis = false;

            if (params.contains("x")) {
                double target = 0.0;
                if (!parse_double(params["x"], target))
                    return make_error("move_selected_to", "x must be numeric.");
                delta(X) = target - bbox_axis_coordinate(bbox, Axis::X, anchor);
                any_axis = true;
            }
            if (params.contains("y")) {
                double target = 0.0;
                if (!parse_double(params["y"], target))
                    return make_error("move_selected_to", "y must be numeric.");
                delta(Y) = target - bbox_axis_coordinate(bbox, Axis::Y, anchor);
                any_axis = true;
            }
            if (params.contains("z")) {
                double target = 0.0;
                if (!parse_double(params["z"], target))
                    return make_error("move_selected_to", "z must be numeric.");
                delta(Z) = target - bbox_axis_coordinate(bbox, Axis::Z, anchor);
                any_axis = true;
            }

            if (!any_axis)
                return make_error("move_selected_to", "Provide at least one axis target (x, y, or z).");

            selection.setup_cache();
            selection.translate(delta, TransformationType::World_Relative_Joint);
            m_plater.canvas3D()->do_move("AI: Move selected to exact position");
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("move_selected_to", "Moved selection to exact coordinates.",
                                nlohmann::json{{"delta", vec3_to_json(delta)}, {"anchor", anchor == AnchorMode::Min ? "min" : anchor == AnchorMode::Max ? "max" : "center"}});
        }
    };

    m_actions["rotate_selected"] = ActionDescriptor {
        "Rotate current selection by x/y/z degrees in world coordinates.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"x_deg", nlohmann::json{{"type", "number"}}},
                {"y_deg", nlohmann::json{{"type", "number"}}},
                {"z_deg", nlohmann::json{{"type", "number"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("rotate_selected", "No selection. Select an object first.");

            double x_deg = 0.0;
            double y_deg = 0.0;
            double z_deg = 0.0;
            if (params.contains("x_deg") && !parse_double(params["x_deg"], x_deg))
                return make_error("rotate_selected", "Parameter x_deg must be numeric.");
            if (params.contains("y_deg") && !parse_double(params["y_deg"], y_deg))
                return make_error("rotate_selected", "Parameter y_deg must be numeric.");
            if (params.contains("z_deg") && !parse_double(params["z_deg"], z_deg))
                return make_error("rotate_selected", "Parameter z_deg must be numeric.");

            selection.setup_cache();
            selection.rotate(Vec3d(x_deg * kDegToRad, y_deg * kDegToRad, z_deg * kDegToRad),
                             TransformationType::World_Relative_Joint);
            m_plater.canvas3D()->do_rotate("AI: Rotate selected");
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("rotate_selected", "Rotated selection by [" + std::to_string(x_deg) + ", " + std::to_string(y_deg) + ", " + std::to_string(z_deg) + "] degrees.");
        }
    };

    m_actions["place_selected_on_largest_face"] = ActionDescriptor {
        "Rotate a single selected instance so its largest detected face is placed on the bed.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (!selection.is_single_full_instance())
                return make_error("place_selected_on_largest_face", "Action requires a single full instance selection.");

            const int object_index = selection.get_object_idx();
            const int instance_index = selection.get_instance_idx();
            if (object_index < 0 || object_index >= static_cast<int>(m_plater.model().objects.size()))
                return make_error("place_selected_on_largest_face", "Selected object index is out of range.");

            const ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("place_selected_on_largest_face", "Selected object is unavailable.");

            Vec3d normal = Vec3d::Zero();
            double area = 0.0;
            std::string error;
            if (!compute_largest_placeable_face_normal(*object, instance_index, normal, area, error))
                return make_error("place_selected_on_largest_face", error);

            selection.flattening_rotate(normal);
            m_plater.canvas3D()->do_rotate("AI: Place on largest face");
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("place_selected_on_largest_face",
                                "Placed selected instance on its largest detected face.",
                                nlohmann::json{
                                    {"object_index", object_index},
                                    {"instance_index", instance_index},
                                    {"face_normal_object", vec3_to_json(normal)},
                                    {"estimated_face_area_mm2", round_coord(area)}
                                });
        }
    };

    m_actions["set_selected_rotation"] = ActionDescriptor {
        "Set absolute rotation on one axis for a single selected instance.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"axis", "target_deg"})},
            {"properties", nlohmann::json{
                {"axis", nlohmann::json{{"type", "string"}, {"description", "x|y|z"}}},
                {"target_deg", nlohmann::json{{"type", "number"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("set_selected_rotation", "No selection. Select one object instance first.");

            const int object_index = selection.get_object_idx();
            const int instance_index = selection.get_instance_idx();
            if (object_index < 0 || instance_index < 0)
                return make_error("set_selected_rotation", "This action requires a single object instance selection.");

            Axis axis = Axis::Z;
            if (!params.contains("axis") || !parse_axis(params["axis"], axis))
                return make_error("set_selected_rotation", "axis must be one of x, y, z.");

            double target_deg = 0.0;
            if (!params.contains("target_deg") || !parse_double(params["target_deg"], target_deg))
                return make_error("set_selected_rotation", "target_deg must be numeric.");

            const ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr || instance_index >= static_cast<int>(object->instances.size()))
                return make_error("set_selected_rotation", "Resolved instance is unavailable.");

            const ModelInstance* instance = object->instances[instance_index];
            const double current_deg = instance->get_rotation(axis) / kDegToRad;
            const double delta_deg = target_deg - current_deg;
            if (std::abs(delta_deg) < 1e-9)
                return make_success("set_selected_rotation", "Rotation already at target.",
                                    nlohmann::json{{"axis", axis_to_string(axis)}, {"target_deg", round_coord(target_deg)}});

            Vec3d delta_rad = Vec3d::Zero();
            delta_rad(axis_to_index(axis)) = delta_deg * kDegToRad;

            selection.setup_cache();
            selection.rotate(delta_rad, TransformationType::World_Relative_Joint);
            m_plater.canvas3D()->do_rotate("AI: Set selected rotation");
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("set_selected_rotation", "Set selected rotation to target.",
                                nlohmann::json{{"axis", axis_to_string(axis)}, {"target_deg", round_coord(target_deg)}, {"delta_deg", round_coord(delta_deg)}});
        }
    };

    m_actions["scale_selected"] = ActionDescriptor {
        "Scale current selection. Use scale_percent (uniform) or x_factor/y_factor/z_factor.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"scale_percent", nlohmann::json{{"type", "number"}}},
                {"x_factor", nlohmann::json{{"type", "number"}}},
                {"y_factor", nlohmann::json{{"type", "number"}}},
                {"z_factor", nlohmann::json{{"type", "number"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("scale_selected", "No selection. Select an object first.");

            Vec3d factors = Vec3d::Ones();
            if (params.contains("scale_percent")) {
                double percent = 0.0;
                if (!parse_double(params["scale_percent"], percent))
                    return make_error("scale_selected", "Parameter scale_percent must be numeric.");
                const double factor = percent / 100.0;
                factors = Vec3d(factor, factor, factor);
            } else {
                double fx = 1.0;
                double fy = 1.0;
                double fz = 1.0;
                if (params.contains("x_factor") && !parse_double(params["x_factor"], fx))
                    return make_error("scale_selected", "Parameter x_factor must be numeric.");
                if (params.contains("y_factor") && !parse_double(params["y_factor"], fy))
                    return make_error("scale_selected", "Parameter y_factor must be numeric.");
                if (params.contains("z_factor") && !parse_double(params["z_factor"], fz))
                    return make_error("scale_selected", "Parameter z_factor must be numeric.");
                factors = Vec3d(fx, fy, fz);
            }

            if (factors(0) <= 0.0 || factors(1) <= 0.0 || factors(2) <= 0.0)
                return make_error("scale_selected", "Scale factors must be positive numbers.");

            selection.setup_cache();
            selection.scale(factors, TransformationType::World_Relative_Joint);
            m_plater.canvas3D()->do_scale("AI: Scale selected");
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("scale_selected", "Scaled selection by factors [" + std::to_string(factors(0)) + ", " + std::to_string(factors(1)) + ", " + std::to_string(factors(2)) + "].");
        }
    };

    m_actions["scale_selected_to_size"] = ActionDescriptor {
        "Scale selection so a chosen world-axis bbox size matches target_size_mm exactly.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"axis", "target_size_mm"})},
            {"properties", nlohmann::json{
                {"axis", nlohmann::json{{"type", "string"}, {"description", "x|y|z"}}},
                {"target_size_mm", nlohmann::json{{"type", "number"}}},
                {"uniform", nlohmann::json{{"type", "boolean"}, {"description", "default true: preserve proportions"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("scale_selected_to_size", "No selection. Select an object first.");

            Axis axis = Axis::Z;
            if (!params.contains("axis") || !parse_axis(params["axis"], axis))
                return make_error("scale_selected_to_size", "axis must be one of x, y, z.");

            double target_size = 0.0;
            if (!params.contains("target_size_mm") || !parse_double(params["target_size_mm"], target_size))
                return make_error("scale_selected_to_size", "target_size_mm must be numeric.");
            if (target_size <= 0.0)
                return make_error("scale_selected_to_size", "target_size_mm must be > 0.");

            const BoundingBoxf3 before_bbox = selection.get_bounding_box();
            const double current_size = before_bbox.size()(axis_to_index(axis));
            if (current_size <= std::numeric_limits<double>::epsilon())
                return make_error("scale_selected_to_size", "Current selected size on this axis is too small for stable scaling.");

            const double factor = target_size / current_size;
            const bool uniform = params.value("uniform", true);

            Vec3d scale_factors = Vec3d::Ones();
            if (uniform) {
                scale_factors = Vec3d(factor, factor, factor);
            } else {
                scale_factors(axis_to_index(axis)) = factor;
            }

            if (scale_factors(0) <= 0.0 || scale_factors(1) <= 0.0 || scale_factors(2) <= 0.0)
                return make_error("scale_selected_to_size", "Computed scale factors are invalid.");

            selection.setup_cache();
            selection.scale(scale_factors, TransformationType::World_Relative_Joint);
            m_plater.canvas3D()->do_scale("AI: Scale selected to target size");
            refresh_object_manipulation_panel();
            m_plater.update();

            const double resulting_size = selection.get_bounding_box().size()(axis_to_index(axis));
            return make_success("scale_selected_to_size", "Scaled selection to target size.",
                                nlohmann::json{
                                    {"axis", axis_to_string(axis)},
                                    {"target_size_mm", round_coord(target_size)},
                                    {"before_size_mm", round_coord(current_size)},
                                    {"after_size_mm", round_coord(resulting_size)},
                                    {"uniform", uniform},
                                    {"scale_factors", vec3_to_json(scale_factors)}
                                });
        }
    };

    m_actions["mirror_selected"] = ActionDescriptor {
        "Mirror current selection by axis x/y/z.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"axis"})},
            {"properties", nlohmann::json{
                {"axis", nlohmann::json{{"type", "string"}, {"description", "x, y, or z"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("mirror_selected", "No selection. Select an object first.");

            Axis axis = Axis::X;
            if (!params.contains("axis") || !parse_axis(params["axis"], axis))
                return make_error("mirror_selected", "Parameter axis must be one of: x, y, z.");

            selection.setup_cache();
            selection.mirror(axis, TransformationType::World_Relative_Joint);
            m_plater.canvas3D()->do_mirror("AI: Mirror selected");
            refresh_object_manipulation_panel();
            m_plater.update();

            return make_success("mirror_selected", "Mirrored selection on axis.");
        }
    };

    m_actions["duplicate_selected"] = ActionDescriptor {
        "Duplicate selected object/instance by adding new instances.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"count", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("duplicate_selected", "No selection. Select an object or instance first.");
            if (!m_plater.can_increase_instances())
                return make_error("duplicate_selected", "Current selection cannot be duplicated.");

            int count = 1;
            if (params.contains("count") && !parse_int(params["count"], count))
                return make_error("duplicate_selected", "Parameter count must be an integer.");
            if (count < 1)
                return make_error("duplicate_selected", "count must be >= 1.");

            const int object_idx = selection.get_object_idx();
            const int instance_idx = selection.get_instance_idx();
            if (object_idx >= 0)
                m_plater.increase_instances(static_cast<size_t>(count), object_idx, instance_idx);
            else
                m_plater.increase_instances(static_cast<size_t>(count));

            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("duplicate_selected", "Duplicated selection by " + std::to_string(count) + " instance(s).");
        }
    };

    m_actions["delete_selected"] = ActionDescriptor {
        "Delete current selection. Requires confirm=true.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"confirm"})},
            {"properties", nlohmann::json{
                {"confirm", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool confirm = false;
            if (!params.contains("confirm") || !parse_bool(params["confirm"], confirm) || !confirm)
                return make_error("delete_selected", "Destructive action blocked. Set confirm=true to proceed.");
            if (!m_plater.can_delete())
                return make_error("delete_selected", "Current selection cannot be deleted.");

            m_plater.remove_selected();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("delete_selected", "Deleted current selection.");
        }
    };

    m_actions["arrange_objects"] = ActionDescriptor {
        "Arrange objects on bed. Optional current_bed_only=true.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"current_bed_only", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!m_plater.can_arrange())
                return make_error("arrange_objects", "Arrange is not available for current state.");

            bool current_bed_only = false;
            if (params.contains("current_bed_only") && !parse_bool(params["current_bed_only"], current_bed_only))
                return make_error("arrange_objects", "Parameter current_bed_only must be boolean.");

            m_plater.arrange(current_bed_only);
            m_plater.update();
            return make_success("arrange_objects", current_bed_only ? "Started arrange on current bed." : "Started arrange on all beds.");
        }
    };

    m_actions["drop_to_bed"] = ActionDescriptor {
        "Move selected object/instance down to bed.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("drop_to_bed", "No selection. Select an object first.");

            const int object_idx = selection.get_object_idx();
            if (object_idx < 0 || object_idx >= static_cast<int>(m_plater.model().objects.size()))
                return make_error("drop_to_bed", "Drop to bed requires a single object or instance selection.");

            m_plater.take_snapshot(std::string("AI: Drop to bed"));
            m_plater.canvas3D()->ensure_on_bed(static_cast<unsigned int>(object_idx), m_plater.printer_technology() != ptSLA);
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("drop_to_bed", "Dropped selected object/instance to bed.");
        }
    };

    m_actions["list_setting_keys"] = ActionDescriptor {
        "List setting keys for domain: print/filament/material/printer/auto.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"domain", nlohmann::json{{"type", "string"}}},
                {"query", nlohmann::json{{"type", "string"}}},
                {"limit", nlohmann::json{{"type", "integer"}}},
                {"extruder_index", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            std::string domain;
            Tab* tab = resolve_settings_tab(m_plater, params, domain);
            if (tab == nullptr || tab->get_config() == nullptr)
                return make_error("list_setting_keys", "Unable to resolve settings domain.");

            if (domain == "filament" && m_plater.printer_technology() == ptFFF && params.contains("extruder_index")) {
                int extruder_index = -1;
                if (!parse_int(params["extruder_index"], extruder_index))
                    return make_error("list_setting_keys", "extruder_index must be an integer.");
                TabFilament* filament_tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
                if (filament_tab == nullptr || !filament_tab->set_active_extruder(extruder_index))
                    return make_error("list_setting_keys", "Failed to switch active filament extruder.");
                tab = filament_tab;
            }

            DynamicPrintConfig* config = tab->get_config();
            const std::string query = params.value("query", std::string());
            const std::string query_lc = lower_copy(query);
            int limit = 200;
            if (params.contains("limit") && !parse_int(params["limit"], limit))
                return make_error("list_setting_keys", "Parameter limit must be an integer.");
            if (limit < 1)
                limit = 1;

            nlohmann::json keys = nlohmann::json::array();
            int added = 0;
            for (const std::string& key : config->def()->keys()) {
                if (!query_lc.empty() && lower_copy(key).find(query_lc) == std::string::npos)
                    continue;
                keys.push_back(key);
                if (++added >= limit)
                    break;
            }

            return make_success("list_setting_keys", "Collected setting keys.",
                                nlohmann::json{{"domain", domain}, {"keys", std::move(keys)}, {"count", added}});
        }
    };

    m_actions["search_settings"] = ActionDescriptor {
        "Search setting keys by query for domain: print/filament/material/printer/auto.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"query"})},
            {"properties", nlohmann::json{
                {"domain", nlohmann::json{{"type", "string"}}},
                {"query", nlohmann::json{{"type", "string"}}},
                {"limit", nlohmann::json{{"type", "integer"}}},
                {"extruder_index", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("query") || !params["query"].is_string())
                return make_error("search_settings", "Parameter query is required and must be a string.");

            std::string domain;
            Tab* tab = resolve_settings_tab(m_plater, params, domain);
            if (tab == nullptr || tab->get_config() == nullptr)
                return make_error("search_settings", "Unable to resolve settings domain.");

            if (domain == "filament" && m_plater.printer_technology() == ptFFF && params.contains("extruder_index")) {
                int extruder_index = -1;
                if (!parse_int(params["extruder_index"], extruder_index))
                    return make_error("search_settings", "extruder_index must be an integer.");
                TabFilament* filament_tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
                if (filament_tab == nullptr || !filament_tab->set_active_extruder(extruder_index))
                    return make_error("search_settings", "Failed to switch active filament extruder.");
                tab = filament_tab;
            }

            DynamicPrintConfig* config = tab->get_config();
            const std::string query_lc = lower_copy(params["query"].get<std::string>());
            int limit = 100;
            if (params.contains("limit") && !parse_int(params["limit"], limit))
                return make_error("search_settings", "Parameter limit must be an integer.");
            if (limit < 1)
                limit = 1;

            nlohmann::json matches = nlohmann::json::array();
            int added = 0;
            for (const std::string& key : config->def()->keys()) {
                if (lower_copy(key).find(query_lc) == std::string::npos)
                    continue;
                matches.push_back(key);
                if (++added >= limit)
                    break;
            }

            return make_success("search_settings", "Found matching setting keys.",
                                nlohmann::json{{"domain", domain}, {"query", params["query"]}, {"keys", std::move(matches)}, {"count", added}});
        }
    };

    m_actions["get_setting"] = ActionDescriptor {
        "Get current value and metadata for a setting key.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key"})},
            {"properties", nlohmann::json{
                {"domain", nlohmann::json{{"type", "string"}}},
                {"key", nlohmann::json{{"type", "string"}}},
                {"extruder_index", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("key") || !params["key"].is_string())
                return make_error("get_setting", "Parameter key is required and must be a string.");

            std::string domain;
            Tab* tab = resolve_settings_tab(m_plater, params, domain);
            if (tab == nullptr || tab->get_config() == nullptr)
                return make_error("get_setting", "Unable to resolve settings domain.");

            if (domain == "filament" && m_plater.printer_technology() == ptFFF && params.contains("extruder_index")) {
                int extruder_index = -1;
                if (!parse_int(params["extruder_index"], extruder_index))
                    return make_error("get_setting", "extruder_index must be an integer.");
                TabFilament* filament_tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
                if (filament_tab == nullptr || !filament_tab->set_active_extruder(extruder_index))
                    return make_error("get_setting", "Failed to switch active filament extruder.");
                tab = filament_tab;
            }

            DynamicPrintConfig* config = tab->get_config();
            std::string key;
            const std::string requested_key = params["key"].get<std::string>();
            if (!resolve_setting_key_generic(*config, requested_key, key))
                return make_error("get_setting", "Unknown setting key: " + requested_key);

            const ConfigOption* option = config->option(key);
            if (option == nullptr)
                return make_error("get_setting", "Setting exists but has no value: " + key);

            const ConfigOptionDef* def = config->option_def(key);
            return make_success("get_setting", "Read setting value.",
                                nlohmann::json{
                                    {"domain", domain},
                                    {"key", key},
                                    {"value_serialized", option->serialize()},
                                    {"type", def ? static_cast<int>(def->type) : -1}
                                });
        }
    };

    m_actions["select_all_objects"] = ActionDescriptor {
        "Select all objects in scene.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.select_all();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("select_all_objects", "Selected all objects.");
        }
    };

    m_actions["clear_selection"] = ActionDescriptor {
        "Clear current selection.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.deselect_all();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("clear_selection", "Cleared selection.");
        }
    };

    m_actions["set_setting"] = ActionDescriptor {
        "Set any print/filament/material/printer setting using domain+key+value.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"domain", "key", "value"})},
            {"properties", nlohmann::json{
                {"domain", nlohmann::json{{"type", "string"}, {"description", "print|filament|material|printer"}}},
                {"key", nlohmann::json{{"type", "string"}}},
                {"value", nlohmann::json{{"description", "Setting value; supports string/number/bool/array."}}},
                {"extruder_index", nlohmann::json{{"type", "integer"}, {"description", "Optional active extruder for filament domain (FFF)."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("domain") || !params["domain"].is_string())
                return make_error("set_setting", "Parameter domain is required and must be a string.");

            const std::string domain = normalize_preset_domain(params["domain"].get<std::string>(), m_plater);
            Tab* tab = resolve_tab_for_domain(domain, m_plater);
            if (tab == nullptr)
                return make_error("set_setting", "Unsupported settings domain: " + domain);

            if (domain == "filament" && m_plater.printer_technology() == ptFFF && params.contains("extruder_index")) {
                int extruder_index = -1;
                if (!parse_int(params["extruder_index"], extruder_index))
                    return make_error("set_setting", "extruder_index must be an integer.");

                TabFilament* filament_tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
                if (filament_tab == nullptr || !filament_tab->set_active_extruder(extruder_index))
                    return make_error("set_setting", "Failed to switch active filament extruder to index " + std::to_string(extruder_index) + ".");
                tab = filament_tab;
            }

            return set_setting_for_tab(m_plater, params, "set_setting", "AI: Set " + domain + " setting", tab);
        }
    };

    m_actions["list_presets"] = ActionDescriptor {
        "List available presets/profiles for print/filament/material/printer domain.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"domain", nlohmann::json{{"type", "string"}, {"description", "Optional: print|filament|material|printer"}}},
                {"include_hidden", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (wxGetApp().preset_bundle == nullptr)
                return make_error("list_presets", "Preset bundle is unavailable.");

            const bool include_hidden = params.value("include_hidden", false);
            auto collect_domain = [this, include_hidden](const std::string& requested_domain) {
                const std::string domain = normalize_preset_domain(requested_domain, m_plater);
                PresetCollection* collection = resolve_preset_collection_for_domain(domain, m_plater);
                if (collection == nullptr)
                    return nlohmann::json::object();

                nlohmann::json presets = nlohmann::json::array();
                const std::deque<Preset>& all = collection->get_presets();
                for (const Preset& preset : all) {
                    if (!include_hidden && !preset.is_visible)
                        continue;
                    presets.push_back(nlohmann::json{
                        {"name", preset.name},
                        {"visible", preset.is_visible},
                        {"compatible", preset.is_compatible},
                        {"is_system", preset.is_system},
                        {"is_default", preset.is_default}
                    });
                }
                return nlohmann::json{
                    {"selected", collection->get_selected_preset_name()},
                    {"count", static_cast<int>(presets.size())},
                    {"presets", std::move(presets)}
                };
            };

            nlohmann::json data = nlohmann::json::object();
            if (params.contains("domain") && params["domain"].is_string()) {
                const std::string requested = params["domain"].get<std::string>();
                const std::string domain = normalize_preset_domain(requested, m_plater);
                const nlohmann::json one = collect_domain(domain);
                if (one.empty())
                    return make_error("list_presets", "Unsupported preset domain: " + domain);
                data["domain"] = domain;
                data["result"] = one;
            } else {
                for (const std::string& domain : {std::string("print"), std::string("filament"), std::string("material"), std::string("printer")}) {
                    const nlohmann::json one = collect_domain(domain);
                    if (!one.empty())
                        data[domain] = one;
                }
            }

            return make_success("list_presets", "Preset list collected.", data);
        }
    };

    m_actions["select_preset"] = ActionDescriptor {
        "Select active preset/profile by domain and name.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"domain", "name"})},
            {"properties", nlohmann::json{
                {"domain", nlohmann::json{{"type", "string"}, {"description", "print|filament|material|printer"}}},
                {"name", nlohmann::json{{"type", "string"}}},
                {"extruder_index", nlohmann::json{{"type", "integer"}, {"description", "Optional active extruder for filament domain (FFF)."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("domain") || !params["domain"].is_string() || !params.contains("name") || !params["name"].is_string())
                return make_error("select_preset", "Parameters domain and name are required.");

            const std::string domain = normalize_preset_domain(params["domain"].get<std::string>(), m_plater);
            const std::string name = params["name"].get<std::string>();
            if (name.empty())
                return make_error("select_preset", "Preset name cannot be empty.");

            Tab* tab = resolve_tab_for_domain(domain, m_plater);
            if (tab == nullptr)
                return make_error("select_preset", "Unsupported preset domain: " + domain);

            if (domain == "filament" && m_plater.printer_technology() == ptFFF && params.contains("extruder_index")) {
                int extruder_index = -1;
                if (!parse_int(params["extruder_index"], extruder_index))
                    return make_error("select_preset", "extruder_index must be an integer.");
                TabFilament* filament_tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
                if (filament_tab == nullptr || !filament_tab->set_active_extruder(extruder_index))
                    return make_error("select_preset", "Failed to switch active filament extruder to index " + std::to_string(extruder_index) + ".");
                tab = filament_tab;
            }

            if (!tab->select_preset(name, false))
                return make_error("select_preset", "Failed to select preset '" + name + "' for domain '" + domain + "'.");

            m_plater.update_ui_from_settings();
            m_plater.update();
            return make_success("select_preset", "Selected preset '" + name + "' for domain '" + domain + "'.");
        }
    };

    m_actions["list_object_setting_keys"] = ActionDescriptor {
        "List configurable setting keys for an object (or current object selection).",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"query", nlohmann::json{{"type", "string"}}},
                {"limit", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("list_object_setting_keys", error);

            const ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("list_object_setting_keys", "Object configuration is unavailable.");

            const std::string query = params.value("query", std::string());
            const std::string query_lc = lower_copy(query);
            int limit = 200;
            if (params.contains("limit") && !parse_int(params["limit"], limit))
                return make_error("list_object_setting_keys", "limit must be an integer.");
            if (limit < 1)
                limit = 1;

            nlohmann::json keys = nlohmann::json::array();
            int added = 0;
            for (const std::string& key : object->config.keys()) {
                if (!query_lc.empty() && lower_copy(key).find(query_lc) == std::string::npos)
                    continue;
                keys.push_back(key);
                if (++added >= limit)
                    break;
            }

            return make_success("list_object_setting_keys", "Collected object setting keys.",
                                nlohmann::json{{"object_index", object_index}, {"keys", std::move(keys)}, {"count", added}});
        }
    };

    m_actions["get_object_setting"] = ActionDescriptor {
        "Get object-level setting value by key.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"key", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("get_object_setting", error);
            if (!params.contains("key") || !params["key"].is_string())
                return make_error("get_object_setting", "Parameter key is required.");

            const ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("get_object_setting", "Object configuration is unavailable.");

            std::string key;
            const std::string requested_key = params["key"].get<std::string>();
            if (!resolve_key_in_model_config(object->config, requested_key, key))
                return make_error("get_object_setting", "Unknown object setting key: " + requested_key);

            const ConfigOption* option = object->config.option(key);
            if (option == nullptr)
                return make_error("get_object_setting", "Object setting has no value: " + key);

            return make_success("get_object_setting", "Read object setting.",
                                nlohmann::json{{"object_index", object_index}, {"key", key}, {"value_serialized", option->serialize()}});
        }
    };

    m_actions["set_object_setting"] = ActionDescriptor {
        "Set object-level setting by key for a specific object or current selection.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key", "value"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"key", nlohmann::json{{"type", "string"}}},
                {"value", nlohmann::json{{"description", "Setting value; supports string/number/bool/array."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("set_object_setting", error);
            if (!params.contains("key") || !params["key"].is_string() || !params.contains("value"))
                return make_error("set_object_setting", "Parameters key and value are required.");

            ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("set_object_setting", "Object configuration is unavailable.");

            std::string key;
            const std::string requested_key = params["key"].get<std::string>();
            if (!resolve_key_in_model_config(object->config, requested_key, key))
                return make_error("set_object_setting", "Unknown object setting key: " + requested_key);

            const std::string value_text = json_to_config_value_string(params["value"]);
            m_plater.take_snapshot(std::string("AI: Set object setting"));
            try {
                object->config.set_deserialize_strict(key, value_text);
            } catch (const std::exception& e) {
                return make_error("set_object_setting", std::string("Failed to set object setting: ") + e.what());
            }

            m_plater.changed_object(object_index);
            refresh_object_manipulation_panel();
            m_plater.update();

            const ConfigOption* option = object->config.option(key);
            return make_success("set_object_setting", "Set object setting '" + key + "'.",
                                nlohmann::json{{"object_index", object_index}, {"key", key}, {"value_serialized", option ? option->serialize() : value_text}});
        }
    };

    m_actions["list_volume_setting_keys"] = ActionDescriptor {
        "List configurable setting keys for a specific object volume.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"volume_index", nlohmann::json{{"type", "integer"}}},
                {"volume_name", nlohmann::json{{"type", "string"}}},
                {"query", nlohmann::json{{"type", "string"}}},
                {"limit", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("list_volume_setting_keys", error);

            const ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("list_volume_setting_keys", "Object is unavailable.");

            int volume_index = -1;
            if (!resolve_volume_index(params, *object, volume_index, error))
                return make_error("list_volume_setting_keys", error);

            const ModelVolume* volume = object->volumes[volume_index];
            if (volume == nullptr)
                return make_error("list_volume_setting_keys", "Volume is unavailable.");

            const std::string query = params.value("query", std::string());
            const std::string query_lc = lower_copy(query);
            int limit = 200;
            if (params.contains("limit") && !parse_int(params["limit"], limit))
                return make_error("list_volume_setting_keys", "limit must be an integer.");
            if (limit < 1)
                limit = 1;

            nlohmann::json keys = nlohmann::json::array();
            int added = 0;
            for (const std::string& key : volume->config.keys()) {
                if (!query_lc.empty() && lower_copy(key).find(query_lc) == std::string::npos)
                    continue;
                keys.push_back(key);
                if (++added >= limit)
                    break;
            }

            return make_success("list_volume_setting_keys", "Collected volume setting keys.",
                                nlohmann::json{
                                    {"object_index", object_index},
                                    {"volume_index", volume_index},
                                    {"keys", std::move(keys)},
                                    {"count", added}
                                });
        }
    };

    m_actions["get_volume_setting"] = ActionDescriptor {
        "Get volume-level setting value by key.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"volume_index", nlohmann::json{{"type", "integer"}}},
                {"volume_name", nlohmann::json{{"type", "string"}}},
                {"key", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("key") || !params["key"].is_string())
                return make_error("get_volume_setting", "Parameter key is required.");

            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("get_volume_setting", error);

            const ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("get_volume_setting", "Object is unavailable.");

            int volume_index = -1;
            if (!resolve_volume_index(params, *object, volume_index, error))
                return make_error("get_volume_setting", error);

            const ModelVolume* volume = object->volumes[volume_index];
            if (volume == nullptr)
                return make_error("get_volume_setting", "Volume is unavailable.");

            std::string key;
            const std::string requested_key = params["key"].get<std::string>();
            if (!resolve_key_in_model_config(volume->config, requested_key, key))
                return make_error("get_volume_setting", "Unknown volume setting key: " + requested_key);

            const ConfigOption* option = volume->config.option(key);
            if (option == nullptr)
                return make_error("get_volume_setting", "Volume setting has no value: " + key);

            return make_success("get_volume_setting", "Read volume setting.",
                                nlohmann::json{
                                    {"object_index", object_index},
                                    {"volume_index", volume_index},
                                    {"key", key},
                                    {"value_serialized", option->serialize()}
                                });
        }
    };

    m_actions["set_volume_setting"] = ActionDescriptor {
        "Set volume-level setting by key.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key", "value"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"volume_index", nlohmann::json{{"type", "integer"}}},
                {"volume_name", nlohmann::json{{"type", "string"}}},
                {"key", nlohmann::json{{"type", "string"}}},
                {"value", nlohmann::json{{"description", "Setting value; supports string/number/bool/array."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("key") || !params["key"].is_string() || !params.contains("value"))
                return make_error("set_volume_setting", "Parameters key and value are required.");

            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("set_volume_setting", error);

            ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("set_volume_setting", "Object is unavailable.");

            int volume_index = -1;
            if (!resolve_volume_index(params, *object, volume_index, error))
                return make_error("set_volume_setting", error);

            ModelVolume* volume = object->volumes[volume_index];
            if (volume == nullptr)
                return make_error("set_volume_setting", "Volume is unavailable.");

            std::string key;
            const std::string requested_key = params["key"].get<std::string>();
            if (!resolve_key_in_model_config(volume->config, requested_key, key))
                return make_error("set_volume_setting", "Unknown volume setting key: " + requested_key);

            const std::string value_text = json_to_config_value_string(params["value"]);
            m_plater.take_snapshot(std::string("AI: Set volume setting"));
            try {
                volume->config.set_deserialize_strict(key, value_text);
            } catch (const std::exception& e) {
                return make_error("set_volume_setting", std::string("Failed to set volume setting: ") + e.what());
            }

            m_plater.changed_object(object_index);
            refresh_object_manipulation_panel();
            m_plater.update();

            const ConfigOption* option = volume->config.option(key);
            return make_success("set_volume_setting", "Set volume setting '" + key + "'.",
                                nlohmann::json{
                                    {"object_index", object_index},
                                    {"volume_index", volume_index},
                                    {"key", key},
                                    {"value_serialized", option ? option->serialize() : value_text}
                                });
        }
    };

    m_actions["cut_selected"] = ActionDescriptor {
        "Cut selected object instance by axis plane. Supports offset from center, exact world coordinate, or ratio along bbox. Requires confirm=true.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"confirm"})},
            {"properties", nlohmann::json{
                {"axis", nlohmann::json{{"type", "string"}, {"description", "x|y|z, default z"}}},
                {"offset_mm", nlohmann::json{{"type", "number"}, {"description", "Plane offset from selected bbox center along axis."}}},
                {"plane_axis_world_mm", nlohmann::json{{"type", "number"}, {"description", "Exact world coordinate for cut plane on selected axis."}}},
                {"ratio_from_min", nlohmann::json{{"type", "number"}, {"description", "0..1 ratio from selected bbox min along axis."}}},
                {"keep_upper", nlohmann::json{{"type", "boolean"}}},
                {"keep_lower", nlohmann::json{{"type", "boolean"}}},
                {"keep_as_parts", nlohmann::json{{"type", "boolean"}}},
                {"place_on_cut_upper", nlohmann::json{{"type", "boolean"}}},
                {"place_on_cut_lower", nlohmann::json{{"type", "boolean"}}},
                {"flip_upper", nlohmann::json{{"type", "boolean"}}},
                {"flip_lower", nlohmann::json{{"type", "boolean"}}},
                {"confirm", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool confirm = false;
            if (!params.contains("confirm") || !parse_bool(params["confirm"], confirm) || !confirm)
                return make_error("cut_selected", "Destructive action blocked. Set confirm=true to proceed.");

            Axis axis = Axis::Z;
            if (params.contains("axis") && !parse_axis(params["axis"], axis))
                return make_error("cut_selected", "axis must be one of x, y, z.");

            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("cut_selected", "No selection. Select one object instance first.");

            const BoundingBoxf3 bbox = selection.get_bounding_box();
            const int axis_idx = axis_to_index(axis);
            const double axis_min = bbox.min(axis_idx);
            const double axis_size = bbox.size()(axis_idx);
            const double axis_center = bbox.center()(axis_idx);

            double plane_axis_world = axis_center;
            if (params.contains("plane_axis_world_mm")) {
                if (!parse_double(params["plane_axis_world_mm"], plane_axis_world))
                    return make_error("cut_selected", "plane_axis_world_mm must be numeric.");
            } else if (params.contains("ratio_from_min")) {
                double ratio = 0.0;
                if (!parse_double(params["ratio_from_min"], ratio))
                    return make_error("cut_selected", "ratio_from_min must be numeric.");
                if (ratio < 0.0 || ratio > 1.0)
                    return make_error("cut_selected", "ratio_from_min must be within [0, 1].");
                plane_axis_world = axis_min + ratio * axis_size;
            } else {
                double offset_mm = 0.0;
                if (params.contains("offset_mm") && !parse_double(params["offset_mm"], offset_mm))
                    return make_error("cut_selected", "offset_mm must be numeric.");
                plane_axis_world = axis_center + offset_mm;
            }

            CutExecutionOptions cut_options;
            cut_options.keep_upper = params.value("keep_upper", true);
            cut_options.keep_lower = params.value("keep_lower", true);
            cut_options.keep_as_parts = params.value("keep_as_parts", false);
            cut_options.place_on_cut_upper = params.value("place_on_cut_upper", true);
            cut_options.place_on_cut_lower = params.value("place_on_cut_lower", false);
            cut_options.flip_upper = params.value("flip_upper", false);
            cut_options.flip_lower = params.value("flip_lower", false);

            return execute_cut_selected_world_plane(m_plater, "cut_selected", axis, plane_axis_world, cut_options);
        }
    };

    m_actions["cut_selected_by_ratio"] = ActionDescriptor {
        "Cut selected object instance at an exact ratio (0..1) from bbox min along axis. Requires confirm=true.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"axis", "ratio_from_min", "confirm"})},
            {"properties", nlohmann::json{
                {"axis", nlohmann::json{{"type", "string"}, {"description", "x|y|z"}}},
                {"ratio_from_min", nlohmann::json{{"type", "number"}, {"description", "0..1 along selected bbox"}}},
                {"keep_upper", nlohmann::json{{"type", "boolean"}}},
                {"keep_lower", nlohmann::json{{"type", "boolean"}}},
                {"keep_as_parts", nlohmann::json{{"type", "boolean"}}},
                {"place_on_cut_upper", nlohmann::json{{"type", "boolean"}}},
                {"place_on_cut_lower", nlohmann::json{{"type", "boolean"}}},
                {"flip_upper", nlohmann::json{{"type", "boolean"}}},
                {"flip_lower", nlohmann::json{{"type", "boolean"}}},
                {"confirm", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool confirm = false;
            if (!params.contains("confirm") || !parse_bool(params["confirm"], confirm) || !confirm)
                return make_error("cut_selected_by_ratio", "Destructive action blocked. Set confirm=true to proceed.");

            Axis axis = Axis::Z;
            if (!params.contains("axis") || !parse_axis(params["axis"], axis))
                return make_error("cut_selected_by_ratio", "axis must be one of x, y, z.");

            double ratio = 0.0;
            if (!params.contains("ratio_from_min") || !parse_double(params["ratio_from_min"], ratio))
                return make_error("cut_selected_by_ratio", "ratio_from_min must be numeric.");
            if (ratio < 0.0 || ratio > 1.0)
                return make_error("cut_selected_by_ratio", "ratio_from_min must be within [0, 1].");

            Selection& selection = m_plater.canvas3D()->get_selection();
            if (selection.is_empty())
                return make_error("cut_selected_by_ratio", "No selection. Select one object instance first.");

            const BoundingBoxf3 bbox = selection.get_bounding_box();
            const int idx = axis_to_index(axis);
            const double plane_axis_world = bbox.min(idx) + ratio * bbox.size()(idx);

            CutExecutionOptions cut_options;
            cut_options.keep_upper = params.value("keep_upper", true);
            cut_options.keep_lower = params.value("keep_lower", true);
            cut_options.keep_as_parts = params.value("keep_as_parts", false);
            cut_options.place_on_cut_upper = params.value("place_on_cut_upper", true);
            cut_options.place_on_cut_lower = params.value("place_on_cut_lower", false);
            cut_options.flip_upper = params.value("flip_upper", false);
            cut_options.flip_lower = params.value("flip_lower", false);

            ActionResult result = execute_cut_selected_world_plane(m_plater, "cut_selected_by_ratio", axis, plane_axis_world, cut_options);
            if (result.success)
                result.data["ratio_from_min"] = round_coord(ratio);
            return result;
        }
    };

    m_actions["split_selected_to_objects"] = ActionDescriptor {
        "Split selected object into multiple objects.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_split(true))
                return make_error("split_selected_to_objects", "Split to objects is not available for current selection.");
            m_plater.split_object();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("split_selected_to_objects", "Split selected object into objects.");
        }
    };

    m_actions["split_selected_to_volumes"] = ActionDescriptor {
        "Split selected object into volumes (parts).",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_split(false))
                return make_error("split_selected_to_volumes", "Split to volumes is not available for current selection.");
            m_plater.split_volume();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("split_selected_to_volumes", "Split selected object into volumes.");
        }
    };

    m_actions["split_selected_instances"] = ActionDescriptor {
        "Split selected instances into separate objects using native object-list workflow.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("split_selected_instances", "Object list is unavailable.");
            if (!m_plater.can_set_instance_to_object())
                return make_error("split_selected_instances", "Splitting instances is not available for current selection.");

            list->split_instances();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("split_selected_instances", "Split selected instances into separate objects.");
        }
    };

    m_actions["merge_selected_objects"] = ActionDescriptor {
        "Merge selected objects either to multipart object (to_multipart=true) or to a single-object mesh.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"to_multipart", nlohmann::json{{"type", "boolean"}, {"description", "default true"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("merge_selected_objects", "Object list is unavailable.");

            const bool to_multipart = params.value("to_multipart", true);
            if (to_multipart && !list->can_merge_to_multipart_object())
                return make_error("merge_selected_objects", "Merge-to-multipart is not available for current selection.");
            if (!to_multipart && !list->can_merge_to_single_object())
                return make_error("merge_selected_objects", "Merge-to-single-object is not available for current selection.");

            list->merge(to_multipart);
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("merge_selected_objects",
                                to_multipart ? "Merged selected objects into a multipart object."
                                             : "Merged selected objects into a single mesh object.");
        }
    };

    m_actions["repair_selected_mesh"] = ActionDescriptor {
        "Run native mesh repair flow (WinSDK repair path) on current selection.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("repair_selected_mesh", "Object list is unavailable.");
            if (!m_plater.can_fix_through_winsdk())
                return make_error("repair_selected_mesh", "Mesh repair is not available for current selection.");

            list->fix_through_winsdk();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("repair_selected_mesh", "Triggered native mesh repair for current selection.");
        }
    };

    m_actions["simplify_selected_mesh"] = ActionDescriptor {
        "Run native simplify workflow for current selection.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("simplify_selected_mesh", "Object list is unavailable.");
            if (!m_plater.can_simplify())
                return make_error("simplify_selected_mesh", "Simplify is not available for current selection.");

            list->simplify();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("simplify_selected_mesh", "Triggered simplify workflow for current selection.");
        }
    };

    m_actions["remove_object"] = ActionDescriptor {
        "Remove one object by index or name. Requires confirm=true.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"confirm"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"confirm", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool confirm = false;
            if (!params.contains("confirm") || !parse_bool(params["confirm"], confirm) || !confirm)
                return make_error("remove_object", "Destructive action blocked. Set confirm=true to proceed.");

            int object_index = -1;
            std::string error;
            if (!resolve_object_index(params, m_plater.model(), object_index, error))
                return make_error("remove_object", error);

            if (object_index < 0 || object_index >= static_cast<int>(m_plater.model().objects.size()))
                return make_error("remove_object", "object_index is out of range.");

            m_plater.remove(static_cast<size_t>(object_index));
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("remove_object", "Removed object " + std::to_string(object_index) + ".");
        }
    };

    m_actions["delete_all_objects"] = ActionDescriptor {
        "Delete all objects from project. Requires confirm=true.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"confirm"})},
            {"properties", nlohmann::json{
                {"confirm", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool confirm = false;
            if (!params.contains("confirm") || !parse_bool(params["confirm"], confirm) || !confirm)
                return make_error("delete_all_objects", "Destructive action blocked. Set confirm=true to proceed.");
            if (!m_plater.can_delete_all())
                return make_error("delete_all_objects", "No objects to delete.");

            m_plater.reset();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("delete_all_objects", "Removed all objects from project.");
        }
    };

    m_actions["set_instance_count"] = ActionDescriptor {
        "Set object instance count exactly for one object.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"target_count"})},
            {"properties", nlohmann::json{
                {"object_index", nlohmann::json{{"type", "integer"}}},
                {"object_name", nlohmann::json{{"type", "string"}}},
                {"target_count", nlohmann::json{{"type", "integer"}, {"minimum", 1}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            int object_index = -1;
            std::string error;
            if (!resolve_object_index_or_selected(params, m_plater, object_index, error))
                return make_error("set_instance_count", error);

            int target_count = -1;
            if (!parse_int(params["target_count"], target_count) || target_count < 1)
                return make_error("set_instance_count", "target_count must be an integer >= 1.");

            ModelObject* object = m_plater.model().objects[object_index];
            if (object == nullptr)
                return make_error("set_instance_count", "Resolved object is unavailable.");

            const int current_count = static_cast<int>(object->instances.size());
            if (current_count == target_count)
                return make_success("set_instance_count", "Instance count already equals target.",
                                    nlohmann::json{{"object_index", object_index}, {"instance_count", current_count}});

            if (target_count > current_count) {
                m_plater.increase_instances(static_cast<size_t>(target_count - current_count), object_index,
                                            current_count > 0 ? current_count - 1 : -1);
            } else {
                m_plater.decrease_instances(static_cast<size_t>(current_count - target_count), object_index);
            }

            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("set_instance_count", "Updated instance count.",
                                nlohmann::json{{"object_index", object_index}, {"instance_count", target_count}});
        }
    };

    m_actions["add_instance"] = ActionDescriptor {
        "Add one or more instances to current selection.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"count", nlohmann::json{{"type", "integer"}, {"minimum", 1}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!m_plater.can_increase_instances())
                return make_error("add_instance", "Adding instances is not available for current selection.");

            int count = 1;
            if (params.contains("count") && (!parse_int(params["count"], count) || count < 1))
                return make_error("add_instance", "count must be an integer >= 1.");

            m_plater.increase_instances(static_cast<size_t>(count));
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("add_instance", "Added instance(s).",
                                nlohmann::json{{"count", count}});
        }
    };

    m_actions["remove_instance"] = ActionDescriptor {
        "Remove one or more instances from current selection.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"count", nlohmann::json{{"type", "integer"}, {"minimum", 1}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!m_plater.can_decrease_instances())
                return make_error("remove_instance", "Removing instances is not available for current selection.");

            int count = 1;
            if (params.contains("count") && (!parse_int(params["count"], count) || count < 1))
                return make_error("remove_instance", "count must be an integer >= 1.");

            m_plater.decrease_instances(static_cast<size_t>(count));
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("remove_instance", "Removed instance(s).",
                                nlohmann::json{{"count", count}});
        }
    };

    m_actions["toggle_printable"] = ActionDescriptor {
        "Toggle printable state of current selected object/instance(s).",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("toggle_printable", "Object list is unavailable.");
            if (!list->is_instance_or_object_selected())
                return make_error("toggle_printable", "Select an object or instance first.");

            list->toggle_printable_state();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("toggle_printable", "Toggled printable state for current selection.");
        }
    };

    m_actions["convert_selected_units"] = ActionDescriptor {
        "Convert selected model/part source units from imperial or meters.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"from"})},
            {"properties", nlohmann::json{
                {"from", nlohmann::json{{"type", "string"}, {"description", "imperial or meters"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            ConversionType conversion_type = ConversionType::CONV_FROM_INCH;
            if (!params.contains("from") || !parse_conversion_type(params["from"], conversion_type))
                return make_error("convert_selected_units", "from must be one of: imperial, inch, inches, meters.");

            m_plater.convert_unit(conversion_type);
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("convert_selected_units",
                                conversion_type == ConversionType::CONV_FROM_METER
                                    ? "Converted selected model from meters."
                                    : "Converted selected model from imperial units.");
        }
    };

    m_actions["add_part"] = ActionDescriptor {
        "Add a part shape to selected object/instance.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"shape", nlohmann::json{{"type", "string"}, {"description", "box, cylinder, sphere, slab, or gallery"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("add_part", "Object list is unavailable.");
            if (!list->is_instance_or_object_selected())
                return make_error("add_part", "Select an object or instance first.");

            std::string shape = "Box";
            if (params.contains("shape")) {
                bool ok = false;
                shape = normalize_shape_name(params["shape"], ok);
                if (!ok)
                    return make_error("add_part", "shape must be one of: box, cylinder, sphere, slab, gallery.");
            }

            if (shape == "Gallery")
                list->load_subobject(ModelVolumeType::MODEL_PART, true);
            else
                list->load_generic_subobject(shape, ModelVolumeType::MODEL_PART);

            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("add_part", "Triggered add-part flow.",
                                nlohmann::json{{"shape", lower_copy(shape)}});
        }
    };

    m_actions["add_negative_volume"] = ActionDescriptor {
        "Add a negative volume shape to selected object/instance.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"shape", nlohmann::json{{"type", "string"}, {"description", "box, cylinder, sphere, slab, or gallery"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("add_negative_volume", "Object list is unavailable.");
            if (!list->is_instance_or_object_selected())
                return make_error("add_negative_volume", "Select an object or instance first.");

            std::string shape = "Box";
            if (params.contains("shape")) {
                bool ok = false;
                shape = normalize_shape_name(params["shape"], ok);
                if (!ok)
                    return make_error("add_negative_volume", "shape must be one of: box, cylinder, sphere, slab, gallery.");
            }

            if (shape == "Gallery")
                list->load_subobject(ModelVolumeType::NEGATIVE_VOLUME, true);
            else
                list->load_generic_subobject(shape, ModelVolumeType::NEGATIVE_VOLUME);

            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("add_negative_volume", "Triggered add-negative-volume flow.",
                                nlohmann::json{{"shape", lower_copy(shape)}});
        }
    };

    m_actions["add_modifier"] = ActionDescriptor {
        "Add a modifier shape to selected object/instance.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"shape", nlohmann::json{{"type", "string"}, {"description", "box, cylinder, sphere, slab, or gallery"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("add_modifier", "Object list is unavailable.");
            if (!list->is_instance_or_object_selected())
                return make_error("add_modifier", "Select an object or instance first.");

            std::string shape = "Box";
            if (params.contains("shape")) {
                bool ok = false;
                shape = normalize_shape_name(params["shape"], ok);
                if (!ok)
                    return make_error("add_modifier", "shape must be one of: box, cylinder, sphere, slab, gallery.");
            }

            if (shape == "Gallery")
                list->load_subobject(ModelVolumeType::PARAMETER_MODIFIER, true);
            else
                list->load_generic_subobject(shape, ModelVolumeType::PARAMETER_MODIFIER);

            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("add_modifier", "Triggered add-modifier flow.",
                                nlohmann::json{{"shape", lower_copy(shape)}});
        }
    };

    m_actions["add_support_blocker"] = ActionDescriptor {
        "Add a support blocker shape to selected object/instance.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"shape", nlohmann::json{{"type", "string"}, {"description", "box, cylinder, sphere, slab"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("add_support_blocker", "Object list is unavailable.");
            if (!list->is_instance_or_object_selected())
                return make_error("add_support_blocker", "Select an object or instance first.");

            std::string shape = "Box";
            if (params.contains("shape")) {
                bool ok = false;
                shape = normalize_shape_name(params["shape"], ok);
                if (!ok || shape == "Gallery")
                    return make_error("add_support_blocker", "shape must be one of: box, cylinder, sphere, slab.");
            }

            list->load_generic_subobject(shape, ModelVolumeType::SUPPORT_BLOCKER);
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("add_support_blocker", "Added support blocker.",
                                nlohmann::json{{"shape", lower_copy(shape)}});
        }
    };

    m_actions["add_support_enforcer"] = ActionDescriptor {
        "Add a support enforcer shape to selected object/instance.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"shape", nlohmann::json{{"type", "string"}, {"description", "box, cylinder, sphere, slab"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("add_support_enforcer", "Object list is unavailable.");
            if (!list->is_instance_or_object_selected())
                return make_error("add_support_enforcer", "Select an object or instance first.");

            std::string shape = "Box";
            if (params.contains("shape")) {
                bool ok = false;
                shape = normalize_shape_name(params["shape"], ok);
                if (!ok || shape == "Gallery")
                    return make_error("add_support_enforcer", "shape must be one of: box, cylinder, sphere, slab.");
            }

            list->load_generic_subobject(shape, ModelVolumeType::SUPPORT_ENFORCER);
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("add_support_enforcer", "Added support enforcer.",
                                nlohmann::json{{"shape", lower_copy(shape)}});
        }
    };

    m_actions["add_height_range_modifier"] = ActionDescriptor {
        "Open/apply height range modifier workflow for selected object.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            ObjectList* list = wxGetApp().obj_list();
            if (list == nullptr)
                return make_error("add_height_range_modifier", "Object list is unavailable.");
            if (!list->is_instance_or_object_selected())
                return make_error("add_height_range_modifier", "Select an object or instance first.");

            list->layers_editing();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("add_height_range_modifier", "Triggered height range modifier workflow.");
        }
    };

    m_actions["copy_selection"] = ActionDescriptor {
        "Copy current selection to internal clipboard.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_copy_to_clipboard())
                return make_error("copy_selection", "Copy is not available for current selection.");
            m_plater.copy_selection_to_clipboard();
            return make_success("copy_selection", "Copied selection to clipboard.");
        }
    };

    m_actions["paste_selection"] = ActionDescriptor {
        "Paste selection/object/settings from internal clipboard.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_paste_from_clipboard())
                return make_error("paste_selection", "Paste is not available.");
            m_plater.paste_from_clipboard();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("paste_selection", "Pasted from clipboard.");
        }
    };

    m_actions["scale_to_fit_print_volume"] = ActionDescriptor {
        "Scale current selection to fit print volume.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_scale_to_print_volume())
                return make_error("scale_to_fit_print_volume", "Scale to fit is not available for current selection.");
            m_plater.scale_selection_to_fit_print_volume();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("scale_to_fit_print_volume", "Scaled selection to fit print volume.");
        }
    };

    m_actions["set_view_mode"] = ActionDescriptor {
        "Switch right now between 3D and Preview view.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"view"})},
            {"properties", nlohmann::json{
                {"view", nlohmann::json{{"type", "string"}, {"description", "3d or preview"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("view") || !params["view"].is_string())
                return make_error("set_view_mode", "Parameter view is required.");
            const std::string view_lc = lower_copy(params["view"].get<std::string>());
            if (view_lc == "3d" || view_lc == "plater" || view_lc == "editor") {
                m_plater.select_view_3D("3D");
                return make_success("set_view_mode", "Switched to 3D view.");
            }
            if (view_lc == "preview") {
                m_plater.select_view_3D("Preview");
                return make_success("set_view_mode", "Switched to Preview view.");
            }
            return make_error("set_view_mode", "Unsupported view value. Use '3d' or 'preview'.");
        }
    };

    m_actions["set_ui_state"] = ActionDescriptor {
        "Set core UI toggles: legend, labels, sidebar collapse, layers editing.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"show_legend", nlohmann::json{{"type", "boolean"}}},
                {"show_labels", nlohmann::json{{"type", "boolean"}}},
                {"sidebar_collapsed", nlohmann::json{{"type", "boolean"}}},
                {"layers_editing_enabled", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool changed = false;
            if (params.contains("show_legend")) {
                bool show = false;
                if (!parse_bool(params["show_legend"], show))
                    return make_error("set_ui_state", "show_legend must be boolean.");
                m_plater.show_legend(show);
                changed = true;
            }
            if (params.contains("show_labels")) {
                bool show = false;
                if (!parse_bool(params["show_labels"], show))
                    return make_error("set_ui_state", "show_labels must be boolean.");
                m_plater.show_view3D_labels(show);
                changed = true;
            }
            if (params.contains("sidebar_collapsed")) {
                bool collapsed = false;
                if (!parse_bool(params["sidebar_collapsed"], collapsed))
                    return make_error("set_ui_state", "sidebar_collapsed must be boolean.");
                m_plater.collapse_sidebar(collapsed);
                changed = true;
            }
            if (params.contains("layers_editing_enabled")) {
                bool enabled = false;
                if (!parse_bool(params["layers_editing_enabled"], enabled))
                    return make_error("set_ui_state", "layers_editing_enabled must be boolean.");
                m_plater.toggle_layers_editing(enabled);
                changed = true;
            }

            if (!changed)
                return make_error("set_ui_state", "No UI state fields provided.");

            m_plater.update();
            return make_success("set_ui_state", "Updated UI state.");
        }
    };

    m_actions["open_project"] = ActionDescriptor {
        "Open a project file (.3mf/.prusa/.amf etc.) from path.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"path"})},
            {"properties", nlohmann::json{
                {"path", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("path") || !params["path"].is_string())
                return make_error("open_project", "Parameter path is required and must be a string.");

            const std::string path = params["path"].get<std::string>();
            if (path.empty())
                return make_error("open_project", "path cannot be empty.");
            if (!boost::filesystem::exists(boost::filesystem::path(path)))
                return make_error("open_project", "Project path does not exist: " + path);

            m_plater.load_project(wxString::FromUTF8(path.c_str()));
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("open_project", "Opened project from " + path + ".");
        }
    };

    m_actions["load_gcode_file"] = ActionDescriptor {
        "Load a G-code/BGCode file from path.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"path"})},
            {"properties", nlohmann::json{
                {"path", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("path") || !params["path"].is_string())
                return make_error("load_gcode_file", "Parameter path is required and must be a string.");

            const std::string path = params["path"].get<std::string>();
            if (path.empty())
                return make_error("load_gcode_file", "path cannot be empty.");
            if (!boost::filesystem::exists(boost::filesystem::path(path)))
                return make_error("load_gcode_file", "G-code path does not exist: " + path);

            m_plater.load_gcode(wxString::FromUTF8(path.c_str()));
            return make_success("load_gcode_file", "Loaded G-code file " + path + ".");
        }
    };

    m_actions["reload_selected_from_disk"] = ActionDescriptor {
        "Reload selected model from disk source.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_reload_from_disk())
                return make_error("reload_selected_from_disk", "Reload from disk is not available for current selection.");
            m_plater.reload_from_disk();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("reload_selected_from_disk", "Reloaded selected model from disk.");
        }
    };

    m_actions["reload_all_from_disk"] = ActionDescriptor {
        "Reload all models in project from disk.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.reload_all_from_disk();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("reload_all_from_disk", "Reloaded all models from disk.");
        }
    };

    m_actions["reload_print"] = ActionDescriptor {
        "Reload current print result using current configuration.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.reload_print();
            return make_success("reload_print", "Reloaded print.");
        }
    };

    m_actions["new_project"] = ActionDescriptor {
        "Create a new project/reset current project. Requires confirm=true.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"confirm"})},
            {"properties", nlohmann::json{
                {"confirm", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool confirm = false;
            if (!params.contains("confirm") || !parse_bool(params["confirm"], confirm) || !confirm)
                return make_error("new_project", "Destructive action blocked. Set confirm=true to proceed.");
            m_plater.new_project();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("new_project", "Started a new project.");
        }
    };

    m_actions["fill_bed_with_instances"] = ActionDescriptor {
        "Fill current bed with copies of selected object where possible.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.fill_bed_with_instances();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("fill_bed_with_instances", "Started fill bed operation.");
        }
    };

    m_actions["replace_selected_with_stl"] = ActionDescriptor {
        "Replace selected object with STL from disk using native flow.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_replace_with_stl())
                return make_error("replace_selected_with_stl", "Replace with STL is not available for current selection.");
            m_plater.replace_with_stl();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("replace_selected_with_stl", "Triggered replace-with-STL flow.");
        }
    };

    m_actions["bring_instance_forward"] = ActionDescriptor {
        "Bring selected instance forward in draw/order stack when supported.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.bring_instance_forward();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("bring_instance_forward", "Brought selected instance forward.");
        }
    };

    m_actions["convert_gcode_to_ascii"] = ActionDescriptor {
        "Convert currently loaded G-code output to ASCII format.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.convert_gcode_to_ascii();
            return make_success("convert_gcode_to_ascii", "Triggered conversion to ASCII G-code.");
        }
    };

    m_actions["convert_gcode_to_binary"] = ActionDescriptor {
        "Convert currently loaded G-code output to binary format.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.convert_gcode_to_binary();
            return make_success("convert_gcode_to_binary", "Triggered conversion to binary G-code.");
        }
    };

    m_actions["send_gcode"] = ActionDescriptor {
        "Send/export G-code to configured print host/device.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.send_gcode();
            return make_success("send_gcode", "Triggered send G-code flow.");
        }
    };

    m_actions["connect_gcode"] = ActionDescriptor {
        "Send current bed G-code to Prusa Connect (or configured Connect endpoint).",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.connect_gcode();
            return make_success("connect_gcode", "Triggered Connect upload for current bed.");
        }
    };

    m_actions["connect_gcode_all"] = ActionDescriptor {
        "Send all bed G-codes to Prusa Connect.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.connect_gcode_all();
            return make_success("connect_gcode_all", "Triggered Connect upload for all beds.");
        }
    };

    m_actions["export_toolpaths_obj"] = ActionDescriptor {
        "Export preview toolpaths to OBJ when available.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.has_toolpaths_to_export())
                return make_error("export_toolpaths_obj", "No toolpaths available to export.");
            m_plater.export_toolpaths_to_obj();
            return make_success("export_toolpaths_obj", "Triggered toolpaths-to-OBJ export.");
        }
    };

    m_actions["export_all_gcodes"] = ActionDescriptor {
        "Export all bed G-code outputs using native export flow.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"prefer_removable", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool prefer_removable = false;
            if (params.contains("prefer_removable") && !parse_bool(params["prefer_removable"], prefer_removable))
                return make_error("export_all_gcodes", "prefer_removable must be boolean.");
            m_plater.export_all_gcodes(prefer_removable);
            return make_success("export_all_gcodes", "Triggered export-all G-code flow.");
        }
    };

    m_actions["export_stl"] = ActionDescriptor {
        "Export STL/OBJ mesh using native export flow.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"extended", nlohmann::json{{"type", "boolean"}}},
                {"selection_only", nlohmann::json{{"type", "boolean"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool extended = false;
            bool selection_only = false;
            if (params.contains("extended") && !parse_bool(params["extended"], extended))
                return make_error("export_stl", "extended must be boolean.");
            if (params.contains("selection_only") && !parse_bool(params["selection_only"], selection_only))
                return make_error("export_stl", "selection_only must be boolean.");
            m_plater.export_stl_obj(extended, selection_only);
            return make_success("export_stl", "Triggered mesh export flow.");
        }
    };

    m_actions["save_preset"] = ActionDescriptor {
        "Save current preset in a domain under a specific name.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"domain", "name"})},
            {"properties", nlohmann::json{
                {"domain", nlohmann::json{{"type", "string"}, {"description", "print|filament|material|printer"}}},
                {"name", nlohmann::json{{"type", "string"}}},
                {"detach", nlohmann::json{{"type", "boolean"}}},
                {"extruder_index", nlohmann::json{{"type", "integer"}, {"description", "Optional active extruder for filament domain (FFF)."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("domain") || !params["domain"].is_string() || !params.contains("name") || !params["name"].is_string())
                return make_error("save_preset", "Parameters domain and name are required.");

            const std::string domain = normalize_preset_domain(params["domain"].get<std::string>(), m_plater);
            const std::string name = params["name"].get<std::string>();
            if (name.empty())
                return make_error("save_preset", "Preset name cannot be empty.");

            Tab* tab = resolve_tab_for_domain(domain, m_plater);
            if (tab == nullptr)
                return make_error("save_preset", "Unsupported preset domain: " + domain);

            if (domain == "filament" && m_plater.printer_technology() == ptFFF && params.contains("extruder_index")) {
                int extruder_index = -1;
                if (!parse_int(params["extruder_index"], extruder_index))
                    return make_error("save_preset", "extruder_index must be an integer.");
                TabFilament* filament_tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
                if (filament_tab == nullptr || !filament_tab->set_active_extruder(extruder_index))
                    return make_error("save_preset", "Failed to switch active filament extruder.");
                tab = filament_tab;
            }

            const bool detach = params.value("detach", false);
            tab->save_preset(name, detach);
            m_plater.update_ui_from_settings();
            m_plater.update();

            const std::string selected = tab->get_presets() ? tab->get_presets()->get_selected_preset_name() : std::string();
            return make_success("save_preset", "Saved preset in domain '" + domain + "'.",
                                nlohmann::json{{"domain", domain}, {"selected", selected}});
        }
    };

    m_actions["get_capabilities"] = ActionDescriptor {
        "Return current action/capability availability for AI planning.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            ObjectList* list = wxGetApp().obj_list();
            nlohmann::json caps{
                {"can_delete", m_plater.can_delete()},
                {"can_delete_all", m_plater.can_delete_all()},
                {"can_increase_instances", m_plater.can_increase_instances()},
                {"can_decrease_instances", m_plater.can_decrease_instances()},
                {"can_split_instances", m_plater.can_set_instance_to_object()},
                {"can_mesh_repair", m_plater.can_fix_through_winsdk()},
                {"can_mesh_simplify", m_plater.can_simplify()},
                {"can_arrange", m_plater.can_arrange()},
                {"can_layers_editing", m_plater.can_layers_editing()},
                {"can_copy_to_clipboard", m_plater.can_copy_to_clipboard()},
                {"can_paste_from_clipboard", m_plater.can_paste_from_clipboard()},
                {"can_undo", m_plater.can_undo()},
                {"can_redo", m_plater.can_redo()},
                {"can_reload_from_disk", m_plater.can_reload_from_disk()},
                {"can_replace_with_stl", m_plater.can_replace_with_stl()},
                {"can_mirror", m_plater.can_mirror()},
                {"can_split_to_objects", m_plater.can_split_to_objects()},
                {"can_split_to_volumes", m_plater.can_split_to_volumes()},
                {"can_scale_to_print_volume", m_plater.can_scale_to_print_volume()},
                {"can_toggle_printable", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_convert_selected_units", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_add_part", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_add_negative_volume", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_add_modifier", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_add_support_blocker", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_add_support_enforcer", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_add_height_range_modifier", list != nullptr ? list->is_instance_or_object_selected() : false},
                {"can_merge_to_multipart", list != nullptr ? list->can_merge_to_multipart_object() : false},
                {"can_merge_to_single_object", list != nullptr ? list->can_merge_to_single_object() : false},
                {"is_view3d_shown", m_plater.is_view3D_shown()},
                {"is_preview_shown", m_plater.is_preview_shown()},
                {"is_sidebar_collapsed", m_plater.is_sidebar_collapsed()},
                {"is_legend_shown", m_plater.is_legend_shown()},
                {"are_view3d_labels_shown", m_plater.are_view3D_labels_shown()},
                {"is_layers_editing_enabled", m_plater.is_view3D_layers_editing_enabled()}
            };
            return make_success("get_capabilities", "Collected current capabilities.", caps);
        }
    };

    m_actions["get_arrange_settings"] = ActionDescriptor {
        "Get backend Arrange settings used by Arrange options UI.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            GLCanvas3D* canvas = m_plater.canvas3D();
            if (canvas == nullptr)
                return make_error("get_arrange_settings", "3D canvas is unavailable.");

            const ArrangeSettingsDb_AppCfg& db = canvas->arrange_settings_db();
            float obj_min = 0.f, obj_max = 0.f, bed_min = 0.f, bed_max = 0.f;
            db.distance_from_obj_range(obj_min, obj_max);
            db.distance_from_bed_range(bed_min, bed_max);

            return make_success("get_arrange_settings", "Arrange settings collected.",
                                nlohmann::json{
                                    {"distance_from_objects", db.get_distance_from_objects()},
                                    {"distance_from_bed", db.get_distance_from_bed()},
                                    {"rotation_enabled", db.is_rotation_enabled()},
                                    {"geometry_handling", arrange_geometry_handling_name(db.get_geometry_handling())},
                                    {"arrange_strategy", arrange_strategy_name(db.get_arrange_strategy())},
                                    {"xl_alignment", arrange_xl_pivot_name(db.get_xl_alignment())},
                                    {"ranges", nlohmann::json{
                                        {"distance_from_objects", nlohmann::json::array({obj_min, obj_max})},
                                        {"distance_from_bed", nlohmann::json::array({bed_min, bed_max})}
                                    }}
                                });
        }
    };

    m_actions["set_arrange_settings"] = ActionDescriptor {
        "Set backend Arrange settings used by Arrange options UI.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"distance_from_objects", nlohmann::json{{"type", "number"}}},
                {"distance_from_bed", nlohmann::json{{"type", "number"}}},
                {"rotation_enabled", nlohmann::json{{"type", "boolean"}}},
                {"geometry_handling", nlohmann::json{{"type", "string"}, {"description", "convex|balanced|advanced"}}},
                {"arrange_strategy", nlohmann::json{{"type", "string"}, {"description", "auto|pulltocenter"}}},
                {"xl_alignment", nlohmann::json{{"type", "string"}, {"description", "center|rearleft|frontleft|frontright|rearright|random"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            GLCanvas3D* canvas = m_plater.canvas3D();
            if (canvas == nullptr)
                return make_error("set_arrange_settings", "3D canvas is unavailable.");

            ArrangeSettingsDb_AppCfg& db = canvas->arrange_settings_db();
            bool changed = false;

            if (params.contains("distance_from_objects")) {
                double v = 0.0;
                if (!parse_double(params["distance_from_objects"], v))
                    return make_error("set_arrange_settings", "distance_from_objects must be numeric.");
                db.set_distance_from_objects(static_cast<float>(v));
                changed = true;
            }
            if (params.contains("distance_from_bed")) {
                double v = 0.0;
                if (!parse_double(params["distance_from_bed"], v))
                    return make_error("set_arrange_settings", "distance_from_bed must be numeric.");
                db.set_distance_from_bed(static_cast<float>(v));
                changed = true;
            }
            if (params.contains("rotation_enabled")) {
                bool v = false;
                if (!parse_bool(params["rotation_enabled"], v))
                    return make_error("set_arrange_settings", "rotation_enabled must be boolean.");
                db.set_rotation_enabled(v);
                changed = true;
            }
            if (params.contains("geometry_handling")) {
                if (!params["geometry_handling"].is_string())
                    return make_error("set_arrange_settings", "geometry_handling must be a string.");
                const std::string token = normalize_compact_token(params["geometry_handling"].get<std::string>());
                const auto parsed = arr2::ArrangeSettingsView::to_geometry_handling(token);
                if (!parsed.has_value())
                    return make_error("set_arrange_settings", "Unsupported geometry_handling value: " + token);
                db.set_geometry_handling(*parsed);
                changed = true;
            }
            if (params.contains("arrange_strategy")) {
                if (!params["arrange_strategy"].is_string())
                    return make_error("set_arrange_settings", "arrange_strategy must be a string.");
                const std::string token = normalize_compact_token(params["arrange_strategy"].get<std::string>());
                const auto parsed = arr2::ArrangeSettingsView::to_arrange_strategy(token);
                if (!parsed.has_value())
                    return make_error("set_arrange_settings", "Unsupported arrange_strategy value: " + token);
                db.set_arrange_strategy(*parsed);
                changed = true;
            }
            if (params.contains("xl_alignment")) {
                if (!params["xl_alignment"].is_string())
                    return make_error("set_arrange_settings", "xl_alignment must be a string.");
                const std::string token = normalize_compact_token(params["xl_alignment"].get<std::string>());
                const auto parsed = arr2::ArrangeSettingsView::to_xl_pivots(token);
                if (!parsed.has_value())
                    return make_error("set_arrange_settings", "Unsupported xl_alignment value: " + token);
                db.set_xl_alignment(*parsed);
                changed = true;
            }

            if (!changed)
                return make_error("set_arrange_settings", "No arrange setting fields provided.");

            canvas->set_as_dirty();
            m_plater.update();
            return make_success("set_arrange_settings", "Updated arrange settings.");
        }
    };

    m_actions["list_backend_sections"] = ActionDescriptor {
        "List backend AppConfig sections.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (wxGetApp().app_config == nullptr)
                return make_error("list_backend_sections", "App config is unavailable.");

            nlohmann::json sections = nlohmann::json::array();
            for (const std::string& section : wxGetApp().app_config->sections())
                sections.push_back(section);

            return make_success("list_backend_sections", "Backend sections listed.",
                                nlohmann::json{{"sections", std::move(sections)}});
        }
    };

    m_actions["list_backend_settings"] = ActionDescriptor {
        "List backend AppConfig key/value settings from a section.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"section", nlohmann::json{{"type", "string"}, {"description", "Section name, empty for root section."}}},
                {"limit", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (wxGetApp().app_config == nullptr)
                return make_error("list_backend_settings", "App config is unavailable.");

            const std::string section = params.value("section", std::string());
            if (!wxGetApp().app_config->has_section(section))
                return make_error("list_backend_settings", "App config section not found: '" + section + "'.");

            int limit = 1000;
            if (params.contains("limit") && !parse_int(params["limit"], limit))
                return make_error("list_backend_settings", "limit must be an integer.");
            if (limit < 1)
                limit = 1;

            nlohmann::json entries = nlohmann::json::array();
            int added = 0;
            for (const auto& kv : wxGetApp().app_config->get_section(section)) {
                entries.push_back(nlohmann::json{{"key", kv.first}, {"value", maybe_redact_value(kv.first, kv.second)}});
                if (++added >= limit)
                    break;
            }

            return make_success("list_backend_settings", "Backend settings listed.",
                                nlohmann::json{{"section", section}, {"count", added}, {"entries", std::move(entries)}});
        }
    };

    m_actions["search_backend_settings"] = ActionDescriptor {
        "Search backend AppConfig settings by key/value text.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"query"})},
            {"properties", nlohmann::json{
                {"query", nlohmann::json{{"type", "string"}}},
                {"section", nlohmann::json{{"type", "string"}, {"description", "Optional section filter."}}},
                {"limit", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (wxGetApp().app_config == nullptr)
                return make_error("search_backend_settings", "App config is unavailable.");
            if (!params.contains("query") || !params["query"].is_string())
                return make_error("search_backend_settings", "Parameter query is required and must be a string.");

            const std::string query = params["query"].get<std::string>();
            if (is_blank_text(query))
                return make_error("search_backend_settings", "query cannot be empty.");
            const std::string query_lc = lower_copy(query);

            int limit = 200;
            if (params.contains("limit") && !parse_int(params["limit"], limit))
                return make_error("search_backend_settings", "limit must be an integer.");
            if (limit < 1)
                limit = 1;

            std::vector<std::string> sections;
            if (params.contains("section")) {
                if (!params["section"].is_string())
                    return make_error("search_backend_settings", "section must be a string.");
                const std::string section = params["section"].get<std::string>();
                if (!wxGetApp().app_config->has_section(section))
                    return make_error("search_backend_settings", "App config section not found: '" + section + "'.");
                sections.push_back(section);
            } else {
                sections = wxGetApp().app_config->sections();
            }

            nlohmann::json matches = nlohmann::json::array();
            int count = 0;
            for (const std::string& section : sections) {
                for (const auto& kv : wxGetApp().app_config->get_section(section)) {
                    const std::string key_lc = lower_copy(kv.first);
                    const std::string value_lc = lower_copy(kv.second);
                    if (key_lc.find(query_lc) == std::string::npos && value_lc.find(query_lc) == std::string::npos)
                        continue;

                    matches.push_back(nlohmann::json{
                        {"section", section},
                        {"key", kv.first},
                        {"value", maybe_redact_value(kv.first, kv.second)}
                    });

                    if (++count >= limit)
                        break;
                }
                if (count >= limit)
                    break;
            }

            return make_success("search_backend_settings", "Backend settings search completed.",
                                nlohmann::json{{"query", query}, {"count", count}, {"matches", std::move(matches)}});
        }
    };

    m_actions["get_backend_setting"] = ActionDescriptor {
        "Get backend AppConfig setting by section and key.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key"})},
            {"properties", nlohmann::json{
                {"section", nlohmann::json{{"type", "string"}}},
                {"key", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (wxGetApp().app_config == nullptr)
                return make_error("get_backend_setting", "App config is unavailable.");
            if (!params.contains("key") || !params["key"].is_string())
                return make_error("get_backend_setting", "Parameter key is required.");

            const std::string section = params.value("section", std::string());
            const std::string key = params["key"].get<std::string>();
            std::string value;
            if (!wxGetApp().app_config->get(section, key, value))
                return make_error("get_backend_setting", "Backend setting not found: [" + section + "] " + key);

            return make_success("get_backend_setting", "Backend setting read.",
                                nlohmann::json{{"section", section}, {"key", key}, {"value", maybe_redact_value(key, value)}});
        }
    };

    m_actions["set_backend_setting"] = ActionDescriptor {
        "Set backend AppConfig setting by section and key.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key", "value"})},
            {"properties", nlohmann::json{
                {"section", nlohmann::json{{"type", "string"}}},
                {"key", nlohmann::json{{"type", "string"}}},
                {"value", nlohmann::json{{"description", "String/number/bool backend value."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (wxGetApp().app_config == nullptr)
                return make_error("set_backend_setting", "App config is unavailable.");
            if (!params.contains("key") || !params["key"].is_string() || !params.contains("value"))
                return make_error("set_backend_setting", "Parameters key and value are required.");

            const std::string section = params.value("section", std::string());
            const std::string key = params["key"].get<std::string>();
            const std::string value_text = json_to_config_value_string(params["value"]);

            wxGetApp().app_config->set(section, key, value_text);
            if (section == "arrange" && m_plater.canvas3D() != nullptr)
                m_plater.canvas3D()->arrange_settings_db().sync();

            m_plater.update_ui_from_settings();
            m_plater.update();

            return make_success("set_backend_setting", "Backend setting updated.",
                                nlohmann::json{{"section", section}, {"key", key}, {"value", maybe_redact_value(key, value_text)}});
        }
    };

    m_actions["set_print_setting"] = ActionDescriptor {
        "Set a print setting key to a new value in the active print preset.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key", "value"})},
            {"properties", nlohmann::json{
                {"key", nlohmann::json{{"type", "string"}}},
                {"value", nlohmann::json{{"description", "New value. Type depends on the setting."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            const Preset::Type tab_type = m_plater.printer_technology() == ptFFF ? Preset::TYPE_PRINT : Preset::TYPE_SLA_PRINT;
            Tab* tab = wxGetApp().get_tab(tab_type);
            return set_setting_for_tab(m_plater, params, "set_print_setting", "AI: Set print setting", tab);
        }
    };

    m_actions["set_filament_setting"] = ActionDescriptor {
        "Set a filament/material setting key in the active filament/material preset.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key", "value"})},
            {"properties", nlohmann::json{
                {"key", nlohmann::json{{"type", "string"}}},
                {"value", nlohmann::json{{"description", "New value. Type depends on the setting."}}},
                {"extruder_index", nlohmann::json{{"type", "integer"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            const Preset::Type tab_type = m_plater.printer_technology() == ptFFF ? Preset::TYPE_FILAMENT : Preset::TYPE_SLA_MATERIAL;
            Tab* tab = wxGetApp().get_tab(tab_type);
            if (m_plater.printer_technology() == ptFFF && params.contains("extruder_index")) {
                int extruder_index = -1;
                if (!parse_int(params["extruder_index"], extruder_index))
                    return make_error("set_filament_setting", "extruder_index must be an integer.");
                TabFilament* filament_tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
                if (filament_tab == nullptr || !filament_tab->set_active_extruder(extruder_index))
                    return make_error("set_filament_setting", "Failed to switch active filament extruder.");
                tab = filament_tab;
            }
            return set_setting_for_tab(m_plater, params, "set_filament_setting", "AI: Set filament/material setting", tab);
        }
    };

    m_actions["set_printer_setting"] = ActionDescriptor {
        "Set a printer setting key in the active printer preset.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"key", "value"})},
            {"properties", nlohmann::json{
                {"key", nlohmann::json{{"type", "string"}}},
                {"value", nlohmann::json{{"description", "New value. Type depends on the setting."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
            return set_setting_for_tab(m_plater, params, "set_printer_setting", "AI: Set printer setting", tab);
        }
    };

    m_actions["save_project"] = ActionDescriptor {
        "Save project to .3mf/.prusa path.",
        nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"path"})},
            {"properties", nlohmann::json{
                {"path", nlohmann::json{{"type", "string"}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            if (!params.contains("path") || !params["path"].is_string())
                return make_error("save_project", "Parameter path is required and must be a string.");

            const std::string path = params["path"].get<std::string>();
            if (path.empty())
                return make_error("save_project", "path cannot be empty.");

            const bool ok = m_plater.export_3mf(boost::filesystem::path(path));
            if (!ok)
                return make_error("save_project", "Failed to save project to: " + path);

            return make_success("save_project", "Saved project to " + path + ".", nlohmann::json{{"path", path}});
        }
    };

    m_actions["export_gcode"] = ActionDescriptor {
        "Export G-code. Supports dialog flow or explicit output path/filename.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json{
                {"prefer_removable", nlohmann::json{{"type", "boolean"}}},
                {"path", nlohmann::json{{"type", "string"}, {"description", "Explicit output file path. Relative paths are resolved from preferred output root."}}},
                {"directory", nlohmann::json{{"type", "string"}, {"description", "Output directory. Use with filename."}}},
                {"filename", nlohmann::json{{"type", "string"}, {"description", "Output file name, extension optional."}}}
            }},
            {"additionalProperties", false}
        },
        [this](const nlohmann::json& params) {
            bool prefer_removable = false;
            if (params.contains("prefer_removable") && !parse_bool(params["prefer_removable"], prefer_removable))
                return make_error("export_gcode", "Parameter prefer_removable must be boolean.");

            const bool has_path = params.contains("path");
            const bool has_directory = params.contains("directory");
            const bool has_filename = params.contains("filename");

            if (has_path && (has_directory || has_filename))
                return make_error("export_gcode", "Use either path, or directory+filename, but not both.");

            if (has_path || has_directory || has_filename) {
                boost::filesystem::path requested_path;

                if (has_path) {
                    if (!params["path"].is_string())
                        return make_error("export_gcode", "Parameter path must be a string.");
                    const std::string raw = params["path"].get<std::string>();
                    if (raw.empty())
                        return make_error("export_gcode", "path cannot be empty.");
                    requested_path = boost::filesystem::path(raw);
                } else {
                    if (!has_filename || !params["filename"].is_string())
                        return make_error("export_gcode", "When directory is used, filename is required and must be a string.");

                    const std::string filename = params["filename"].get<std::string>();
                    if (filename.empty())
                        return make_error("export_gcode", "filename cannot be empty.");

                    if (has_directory) {
                        if (!params["directory"].is_string())
                            return make_error("export_gcode", "Parameter directory must be a string.");
                        const std::string directory = params["directory"].get<std::string>();
                        requested_path = boost::filesystem::path(directory) / boost::filesystem::path(filename);
                    } else {
                        requested_path = boost::filesystem::path(filename);
                    }
                }

                if (const std::optional<std::string> error = m_plater.export_gcode_explicit_path(requested_path, prefer_removable))
                    return make_error("export_gcode", *error);

                return make_success("export_gcode", "Started G-code export to " + requested_path.string() + ".");
            }

            m_plater.export_gcode(prefer_removable);
            return make_success("export_gcode", "Triggered G-code export dialog flow.");
        }
    };

    m_actions["undo"] = ActionDescriptor {
        "Undo one step.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_undo())
                return make_error("undo", "Undo is not available.");
            m_plater.undo();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("undo", "Undid last action.");
        }
    };

    m_actions["redo"] = ActionDescriptor {
        "Redo one step.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            if (!m_plater.can_redo())
                return make_error("redo", "Redo is not available.");
            m_plater.redo();
            refresh_object_manipulation_panel();
            m_plater.update();
            return make_success("redo", "Redid last action.");
        }
    };

    m_actions["slice_now"] = ActionDescriptor {
        "Run slicing and switch to Preview view.",
        nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}},
        [this](const nlohmann::json&) {
            m_plater.reslice();
            m_plater.select_view_3D("Preview");
            return make_success("slice_now", "Started slicing and switched to Preview.");
        }
    };
}

} // namespace AI
} // namespace GUI
} // namespace Slic3r
