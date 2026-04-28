#include "SceneSnapshot.hpp"

#include <cmath>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "../Plater.hpp"
#include "../Selection.hpp"
#include "../GUI_App.hpp"
#include "../GLCanvas3D.hpp"

namespace Slic3r {
namespace GUI {
namespace AI {

namespace {

double round_coord(const double value, const int decimals = 4)
{
    const double scale = std::pow(10.0, static_cast<double>(decimals));
    return std::round(value * scale) / scale;
}

std::string object_display_name(const ModelObject& object)
{
    if (!object.name.empty())
        return object.name;
    if (!object.input_file.empty())
        return boost::filesystem::path(object.input_file).filename().string();
    return "Object";
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

nlohmann::json make_selection_json(const Selection& selection)
{
    nlohmann::json out;
    out["is_empty"] = selection.is_empty();
    out["mode"] = selection.is_instance_mode() ? "instance" : "volume";
    out["object_index"] = selection.get_object_idx();
    out["instance_index"] = selection.get_instance_idx();
    out["volume_count"] = static_cast<int>(selection.volumes_count());
    out["is_single_full_object"] = selection.is_single_full_object();
    out["is_single_full_instance"] = selection.is_single_full_instance();
    out["is_single_volume"] = selection.is_single_volume();

    nlohmann::json volume_ids = nlohmann::json::array();
    for (const unsigned int id : selection.get_volume_idxs())
        volume_ids.push_back(id);
    out["selected_volume_ids"] = std::move(volume_ids);

    return out;
}

} // namespace

nlohmann::json build_scene_snapshot(const Plater& plater)
{
    const Model& model = plater.model();
    const Selection& selection = plater.get_selection();

    nlohmann::json snapshot;
    snapshot["printer_technology"] = plater.printer_technology() == ptFFF ? "FFF" : "SLA";
    snapshot["object_count"] = static_cast<int>(model.objects.size());
    snapshot["project_dirty"] = plater.is_project_dirty();
    snapshot["selection"] = make_selection_json(selection);

    nlohmann::json objects = nlohmann::json::array();
    for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
        const ModelObject* object = model.objects[obj_idx];
        if (object == nullptr)
            continue;

        nlohmann::json object_json;
        object_json["index"] = static_cast<int>(obj_idx);
        object_json["name"] = object_display_name(*object);
        object_json["source_path"] = object->input_file;
        object_json["instance_count"] = static_cast<int>(object->instances.size());
        object_json["volume_count"] = static_cast<int>(object->volumes.size());
        object_json["facets_count"] = static_cast<int>(object->facets_count());
        object_json["raw_mesh_bbox"] = bbox_to_json(object->raw_mesh_bounding_box());
        object_json["object_bbox_world"] = bbox_to_json(object->bounding_box_exact());

        nlohmann::json instances = nlohmann::json::array();
        for (size_t inst_idx = 0; inst_idx < object->instances.size(); ++inst_idx) {
            const ModelInstance* instance = object->instances[inst_idx];
            if (instance == nullptr)
                continue;

            nlohmann::json instance_json;
            instance_json["index"] = static_cast<int>(inst_idx);
            instance_json["offset"] = vec3_to_json(instance->get_offset());
            instance_json["rotation"] = vec3_to_json(instance->get_rotation());
            instance_json["scale"] = vec3_to_json(instance->get_scaling_factor());
            instance_json["mirror"] = vec3_to_json(instance->get_mirror());
            instance_json["bbox_world"] = bbox_to_json(object->instance_bounding_box(inst_idx));
            instances.push_back(std::move(instance_json));
        }
        object_json["instances"] = std::move(instances);

        objects.push_back(std::move(object_json));
    }
    snapshot["objects"] = std::move(objects);

    if (wxGetApp().preset_bundle != nullptr) {
        const PresetBundle& bundle = *wxGetApp().preset_bundle;
        nlohmann::json presets;
        presets["printer"] = bundle.printers.get_selected_preset_name();

        if (plater.printer_technology() == ptFFF) {
            presets["print"] = bundle.prints.get_selected_preset_name();
            nlohmann::json filaments = nlohmann::json::array();
            for (const ExtruderFilaments& extruder : bundle.extruders_filaments)
                filaments.push_back(extruder.get_selected_preset_name());
            presets["filaments"] = std::move(filaments);
        } else {
            presets["print"] = bundle.sla_prints.get_selected_preset_name();
            presets["material"] = bundle.sla_materials.get_selected_preset_name();
        }

        snapshot["active_presets"] = std::move(presets);
    }

    if (const DynamicPrintConfig* config = plater.config(); config != nullptr) {
        nlohmann::json print_summary;
        const std::vector<std::string> keys {
            "layer_height",
            "fill_density",
            "fill_pattern",
            "support_material",
            "support_material_auto",
            "brim_width"
        };
        for (const std::string& key : keys) {
            if (!config->def()->has(key))
                continue;
            if (const ConfigOption* opt = config->option(key); opt != nullptr)
                print_summary[key] = opt->serialize();
        }
        snapshot["print_settings_summary"] = std::move(print_summary);
    }

    snapshot["capabilities"] = nlohmann::json{
        {"can_delete", plater.can_delete()},
        {"can_delete_all", plater.can_delete_all()},
        {"can_increase_instances", plater.can_increase_instances()},
        {"can_decrease_instances", plater.can_decrease_instances()},
        {"can_arrange", plater.can_arrange()},
        {"can_layers_editing", plater.can_layers_editing()},
        {"can_copy_to_clipboard", plater.can_copy_to_clipboard()},
        {"can_paste_from_clipboard", plater.can_paste_from_clipboard()},
        {"can_undo", plater.can_undo()},
        {"can_redo", plater.can_redo()},
        {"can_reload_from_disk", plater.can_reload_from_disk()},
        {"can_replace_with_stl", plater.can_replace_with_stl()},
        {"can_mirror", plater.can_mirror()},
        {"can_split_to_objects", plater.can_split_to_objects()},
        {"can_split_to_volumes", plater.can_split_to_volumes()},
        {"can_scale_to_print_volume", plater.can_scale_to_print_volume()},
        {"is_view3d_shown", plater.is_view3D_shown()},
        {"is_preview_shown", plater.is_preview_shown()},
        {"is_sidebar_collapsed", plater.is_sidebar_collapsed()},
        {"is_legend_shown", plater.is_legend_shown()},
        {"are_view3d_labels_shown", plater.are_view3D_labels_shown()},
        {"is_layers_editing_enabled", plater.is_view3D_layers_editing_enabled()}
    };

    if (!selection.is_empty()) {
        snapshot["selection_geometry"] = bbox_to_json(selection.get_bounding_box());
    }

    if (const GLCanvas3D* canvas = plater.canvas3D(); canvas != nullptr) {
        const ArrangeSettingsDb_AppCfg& arrange_db = canvas->arrange_settings_db();
        float obj_min = 0.f, obj_max = 0.f, bed_min = 0.f, bed_max = 0.f;
        arrange_db.distance_from_obj_range(obj_min, obj_max);
        arrange_db.distance_from_bed_range(bed_min, bed_max);
        snapshot["arrange_settings"] = nlohmann::json{
            {"distance_from_objects", arrange_db.get_distance_from_objects()},
            {"distance_from_bed", arrange_db.get_distance_from_bed()},
            {"rotation_enabled", arrange_db.is_rotation_enabled()},
            {"geometry_handling_code", static_cast<int>(arrange_db.get_geometry_handling())},
            {"arrange_strategy_code", static_cast<int>(arrange_db.get_arrange_strategy())},
            {"xl_alignment_code", static_cast<int>(arrange_db.get_xl_alignment())},
            {"ranges", nlohmann::json{
                {"distance_from_objects", nlohmann::json::array({obj_min, obj_max})},
                {"distance_from_bed", nlohmann::json::array({bed_min, bed_max})}
            }}
        };
    }

    return snapshot;
}

} // namespace AI
} // namespace GUI
} // namespace Slic3r
