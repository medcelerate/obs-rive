// rive-source.cpp
//
// The OBS source itself. We expose Rive as an async video source: we render
// our own frames offscreen (via the IBackend's Metal/D3D11/GL path), then
// push them to OBS through obs_source_output_video(). That keeps us decoupled
// from OBS's graphics subsystem - no gs_texture interop needed.
//
// Properties are rebuilt every time the property panel opens so that the
// list of artboards, state machines, SMI inputs, and view-model properties
// reflects the currently loaded .riv file.

#include <obs-module.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rive-core.h"

namespace {

// Property key prefixes used for dynamically generated inputs / VM props.
// Stored in obs_data alongside the static settings; we read them back in
// update() and push into RiveCore.
constexpr const char* kPrefixInput = "in.";
constexpr const char* kPrefixVM    = "vm.";

struct rive_source {
    obs_source_t* source = nullptr;

    std::mutex                mu;
    obsrive::RiveCore         core;
    std::string               lastError;

    // Cached settings (read under `mu`).
    std::string riv_path;
    std::string artboard;
    std::string state_machine;
    int         width  = 1280;
    int         height = 720;
    int         fit_idx = 0;        // matches FitFromIndex below
    int         align_idx = 4;      // center
    double      speed = 1.0;
    uint32_t    bg_argb = 0x00000000;

    // For dt accounting.
    std::chrono::steady_clock::time_point lastTick;
    bool hasTick = false;

    // For trigger edge detection (was the bool/trigger high last frame?).
    // bool inputs: name -> previous bool, used to drop redundant writes.
    // trigger inputs: name -> previous bool, fires on rising edge.
    // VM triggers: same idea, separate map.
    std::unordered_map<std::string, bool> prevInputBool;
    std::unordered_map<std::string, bool> prevInputTrigger;
    std::unordered_map<std::string, bool> prevVMTrigger;

    std::vector<uint8_t> frame; // reused BGRA buffer
};

rive::Fit fit_from_index(int i)
{
    switch (i) {
        case 0: return rive::Fit::contain;
        case 1: return rive::Fit::cover;
        case 2: return rive::Fit::fill;
        case 3: return rive::Fit::fitWidth;
        case 4: return rive::Fit::fitHeight;
        case 5: return rive::Fit::none;
        case 6: return rive::Fit::scaleDown;
        default: return rive::Fit::contain;
    }
}

rive::Alignment align_from_index(int i)
{
    switch (i) {
        case 0: return rive::Alignment::topLeft;
        case 1: return rive::Alignment::topCenter;
        case 2: return rive::Alignment::topRight;
        case 3: return rive::Alignment::centerLeft;
        case 4: return rive::Alignment::center;
        case 5: return rive::Alignment::centerRight;
        case 6: return rive::Alignment::bottomLeft;
        case 7: return rive::Alignment::bottomCenter;
        case 8: return rive::Alignment::bottomRight;
        default: return rive::Alignment::center;
    }
}

// ----------------------------------------------------------------------------
// Helpers used by both update() and get_properties()
// ----------------------------------------------------------------------------

// Returns the OBS property key for an SMI input named `n`. obs_data keys
// can be any UTF-8 string, so we just prefix to avoid collisions.
std::string input_key(const std::string& n) { return std::string(kPrefixInput) + n; }
std::string vm_key   (const std::string& n) { return std::string(kPrefixVM)    + n; }

// ----------------------------------------------------------------------------
// OBS source callbacks
// ----------------------------------------------------------------------------

const char* rive_get_name(void*) { return "Rive Source"; }

void rive_update(void* data, obs_data_t* settings);

void* rive_create(obs_data_t* settings, obs_source_t* source)
{
    auto* s = new rive_source();
    s->source = source;
    std::string err;
    if (!s->core.ensureBackend(err)) {
        blog(LOG_ERROR, "[obs-rive] backend init failed: %s", err.c_str());
    }
    rive_update(s, settings);
    return s;
}

void rive_destroy(void* data)
{
    delete static_cast<rive_source*>(data);
}

uint32_t rive_get_width (void* data) { return (uint32_t)static_cast<rive_source*>(data)->width;  }
uint32_t rive_get_height(void* data) { return (uint32_t)static_cast<rive_source*>(data)->height; }

void rive_defaults(obs_data_t* s)
{
    obs_data_set_default_string(s, "riv_path", "");
    obs_data_set_default_string(s, "artboard", "");
    obs_data_set_default_string(s, "state_machine", "");
    obs_data_set_default_int   (s, "width",  1280);
    obs_data_set_default_int   (s, "height", 720);
    obs_data_set_default_int   (s, "fit", 0);
    obs_data_set_default_int   (s, "alignment", 4);
    obs_data_set_default_double(s, "speed", 1.0);
    obs_data_set_default_int   (s, "bg_color", 0x00000000);
}

void rive_update(void* data, obs_data_t* settings)
{
    auto* s = static_cast<rive_source*>(data);
    std::lock_guard<std::mutex> lock(s->mu);

    s->riv_path      = obs_data_get_string(settings, "riv_path");
    s->artboard      = obs_data_get_string(settings, "artboard");
    s->state_machine = obs_data_get_string(settings, "state_machine");
    s->width         = (int)obs_data_get_int(settings, "width");
    s->height        = (int)obs_data_get_int(settings, "height");
    s->fit_idx       = (int)obs_data_get_int(settings, "fit");
    s->align_idx     = (int)obs_data_get_int(settings, "alignment");
    s->speed         = obs_data_get_double(settings, "speed");
    s->bg_argb       = (uint32_t)obs_data_get_int(settings, "bg_color");

    if (s->width  < 16) s->width  = 16;
    if (s->height < 16) s->height = 16;

    // Make sure file/artboard/SM are current. Errors here just surface in
    // the log; we keep cooking blank frames so the source stays alive.
    if (!s->core.loadFile(s->riv_path)) {
        if (!s->core.error().empty()) {
            blog(LOG_WARNING, "[obs-rive] %s", s->core.error().c_str());
        }
    } else {
        s->core.selectArtboard(s->artboard);
        s->core.selectStateMachine(s->state_machine);
    }

    // Push dynamically-keyed input + VM values into the core.
    for (auto& in : s->core.stateMachineInputs()) {
        std::string k = input_key(in.name);
        switch (in.kind) {
            case obsrive::InputInfo::Kind::Number:
                if (obs_data_has_user_value(settings, k.c_str())) {
                    s->core.setNumberInput(in.name,
                        (float)obs_data_get_double(settings, k.c_str()));
                }
                break;
            case obsrive::InputInfo::Kind::Bool: {
                bool v = obs_data_get_bool(settings, k.c_str());
                bool& prev = s->prevInputBool[in.name];
                if (v != prev) { s->core.setBoolInput(in.name, v); prev = v; }
                break;
            }
            case obsrive::InputInfo::Kind::Trigger: {
                // A toggled-on bool -> rising edge -> fire.
                bool v = obs_data_get_bool(settings, k.c_str());
                bool& prev = s->prevInputTrigger[in.name];
                if (v && !prev) s->core.fireTriggerInput(in.name);
                prev = v;
                break;
            }
            default: break;
        }
    }

    for (auto& p : s->core.viewModelProperties()) {
        std::string k = vm_key(p.name);
        switch (p.type) {
            case rive::DataType::string:
                if (obs_data_has_user_value(settings, k.c_str())) {
                    s->core.setVMString(p.name, obs_data_get_string(settings, k.c_str()));
                }
                break;
            case rive::DataType::number:
                if (obs_data_has_user_value(settings, k.c_str())) {
                    s->core.setVMNumber(p.name, (float)obs_data_get_double(settings, k.c_str()));
                }
                break;
            case rive::DataType::boolean:
                s->core.setVMBool(p.name, obs_data_get_bool(settings, k.c_str()));
                break;
            case rive::DataType::trigger: {
                bool v = obs_data_get_bool(settings, k.c_str());
                bool& prev = s->prevVMTrigger[p.name];
                if (v && !prev) s->core.fireVMTrigger(p.name);
                prev = v;
                break;
            }
            default: break;
        }
    }
}

// ----------------------------------------------------------------------------
// Properties
// ----------------------------------------------------------------------------

bool reload_pressed(obs_properties_t*, obs_property_t*, void* data)
{
    auto* s = static_cast<rive_source*>(data);
    std::lock_guard<std::mutex> lock(s->mu);
    s->core.unloadFile();
    // Re-running update() against the current settings will reload from disk.
    obs_data_t* settings = obs_source_get_settings(s->source);
    if (settings) {
        rive_update(s, settings);
        obs_data_release(settings);
    }
    return true; // refresh the property panel so menus repopulate
}

bool riv_path_changed(void* data, obs_properties_t*, obs_property_t*,
                      obs_data_t* settings)
{
    rive_update(data, settings);
    return true; // refresh property panel
}

obs_properties_t* rive_properties(void* data)
{
    auto* s = static_cast<rive_source*>(data);
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_path(props, "riv_path", "Riv File",
                            OBS_PATH_FILE,
                            "Rive Files (*.riv);;All Files (*.*)",
                            nullptr);
    obs_property_set_modified_callback2(
        obs_properties_get(props, "riv_path"), riv_path_changed, data);

    obs_properties_add_button(props, "reload", "Reload", reload_pressed);

    // Artboard / state machine dropdowns - populated from the currently
    // loaded file.
    obs_property_t* abList = obs_properties_add_list(props, "artboard",
        "Artboard", OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    obs_property_t* smList = obs_properties_add_list(props, "state_machine",
        "State Machine", OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    {
        std::lock_guard<std::mutex> lock(s->mu);
        obs_property_list_add_string(abList, "(default)", "");
        for (auto& n : s->core.artboardNames()) {
            obs_property_list_add_string(abList, n.c_str(), n.c_str());
        }
        obs_property_list_add_string(smList, "(default)", "");
        for (auto& n : s->core.stateMachineNames()) {
            obs_property_list_add_string(smList, n.c_str(), n.c_str());
        }
    }

    // Output configuration.
    obs_properties_add_int   (props, "width",  "Width",  16, 4096, 1);
    obs_properties_add_int   (props, "height", "Height", 16, 4096, 1);
    obs_property_t* fitList = obs_properties_add_list(props, "fit",
        "Fit", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(fitList, "Contain",    0);
    obs_property_list_add_int(fitList, "Cover",      1);
    obs_property_list_add_int(fitList, "Fill",       2);
    obs_property_list_add_int(fitList, "Fit Width",  3);
    obs_property_list_add_int(fitList, "Fit Height", 4);
    obs_property_list_add_int(fitList, "None",       5);
    obs_property_list_add_int(fitList, "Scale Down", 6);
    obs_property_t* alList = obs_properties_add_list(props, "alignment",
        "Alignment", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    const char* alNames[] = {"Top Left","Top Center","Top Right",
                             "Center Left","Center","Center Right",
                             "Bottom Left","Bottom Center","Bottom Right"};
    for (int i = 0; i < 9; ++i) obs_property_list_add_int(alList, alNames[i], i);

    obs_properties_add_float_slider(props, "speed", "Speed", 0.0, 4.0, 0.01);
    obs_properties_add_color_alpha(props, "bg_color", "Background");

    // Dynamic SMI inputs.
    {
        std::lock_guard<std::mutex> lock(s->mu);
        auto inputs = s->core.stateMachineInputs();
        if (!inputs.empty()) {
            obs_properties_add_group(props, "_grp_inputs", "State Machine Inputs",
                                     OBS_GROUP_NORMAL, obs_properties_create());
            obs_properties_t* g = obs_property_group_content(
                obs_properties_get(props, "_grp_inputs"));
            for (auto& in : inputs) {
                std::string key = input_key(in.name);
                switch (in.kind) {
                    case obsrive::InputInfo::Kind::Number:
                        obs_properties_add_float(g, key.c_str(), in.name.c_str(),
                                                 -1e9, 1e9, 0.01);
                        break;
                    case obsrive::InputInfo::Kind::Bool:
                        obs_properties_add_bool(g, key.c_str(), in.name.c_str());
                        break;
                    case obsrive::InputInfo::Kind::Trigger:
                        // No "pulse" widget in OBS - use a bool the user
                        // toggles. Rising edges fire the trigger.
                        obs_properties_add_bool(g, key.c_str(),
                            (in.name + " (toggle to fire)").c_str());
                        break;
                    default: break;
                }
            }
        }
    }

    // Dynamic VM properties.
    {
        std::lock_guard<std::mutex> lock(s->mu);
        auto vmProps = s->core.viewModelProperties();
        if (!vmProps.empty()) {
            obs_properties_add_group(props, "_grp_vm", "View Model",
                                     OBS_GROUP_NORMAL, obs_properties_create());
            obs_properties_t* g = obs_property_group_content(
                obs_properties_get(props, "_grp_vm"));
            for (auto& p : vmProps) {
                std::string key = vm_key(p.name);
                switch (p.type) {
                    case rive::DataType::string:
                        obs_properties_add_text(g, key.c_str(), p.name.c_str(),
                                                OBS_TEXT_DEFAULT);
                        break;
                    case rive::DataType::number:
                        obs_properties_add_float(g, key.c_str(), p.name.c_str(),
                                                 -1e9, 1e9, 0.01);
                        break;
                    case rive::DataType::boolean:
                        obs_properties_add_bool(g, key.c_str(), p.name.c_str());
                        break;
                    case rive::DataType::trigger:
                        obs_properties_add_bool(g, key.c_str(),
                            (p.name + " (toggle to fire)").c_str());
                        break;
                    default: break;
                }
            }
        }
    }

    return props;
}

// ----------------------------------------------------------------------------
// Tick + render
// ----------------------------------------------------------------------------

void rive_tick(void* data, float /*seconds*/)
{
    auto* s = static_cast<rive_source*>(data);
    std::lock_guard<std::mutex> lock(s->mu);

    auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (s->hasTick) {
        dt = std::chrono::duration<float>(now - s->lastTick).count();
        if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
    }
    s->lastTick = now;
    s->hasTick  = true;

    obsrive::FrameParams fp;
    fp.width      = (uint32_t)s->width;
    fp.height     = (uint32_t)s->height;
    fp.fit        = fit_from_index(s->fit_idx);
    fp.alignment  = align_from_index(s->align_idx);
    fp.speed      = (float)s->speed;
    fp.clearColor = s->bg_argb;
    fp.dt         = dt;

    s->core.advance(fp);

    s->frame.resize((size_t)s->width * s->height * 4);
    if (!s->core.render(fp, s->frame.data())) {
        // Don't spam the log; only on first failure of each kind.
        const std::string& e = s->core.error();
        if (!e.empty() && e != s->lastError) {
            blog(LOG_WARNING, "[obs-rive] render: %s", e.c_str());
            s->lastError = e;
        }
        return;
    }
    s->lastError.clear();

    obs_source_frame frame{};
    frame.data[0]     = s->frame.data();
    frame.linesize[0] = (uint32_t)s->width * 4;
    frame.width       = (uint32_t)s->width;
    frame.height      = (uint32_t)s->height;
    frame.format      = VIDEO_FORMAT_BGRA;
    frame.timestamp   = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    obs_source_output_video(s->source, &frame);
}

} // namespace

extern "C" void register_rive_source(void)
{
    obs_source_info info = {};
    info.id              = "obs_rive_source";
    info.type            = OBS_SOURCE_TYPE_INPUT;
    info.output_flags    = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC_VIDEO;
    info.get_name        = rive_get_name;
    info.create          = rive_create;
    info.destroy         = rive_destroy;
    info.get_width       = rive_get_width;
    info.get_height      = rive_get_height;
    info.get_defaults    = rive_defaults;
    info.get_properties  = rive_properties;
    info.update          = rive_update;
    info.video_tick      = rive_tick;
    info.icon_type       = OBS_ICON_TYPE_IMAGE;
    obs_register_source(&info);
}
