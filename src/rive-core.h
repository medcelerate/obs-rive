// rive-core.h
//
// Framework-agnostic wrapper around the Rive runtime. The OBS source layer
// (or any other host) owns one of these per source instance, drives it with
// a backend, parameters, and per-frame ticks; the core does all the file /
// artboard / state-machine / view-model bookkeeping.
//
// No OBS or TouchDesigner headers reach in here - this stays portable so we
// can grow another host later without churning this file.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/scene.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"
#include "rive/layout.hpp"

#include "IBackend.h"

namespace obsrive {

// Public introspection types used by the host's properties UI.
struct InputInfo {
    std::string name;
    enum class Kind { Number, Bool, Trigger, Unknown } kind = Kind::Unknown;
};

struct VMPropertyInfo {
    std::string name;
    rive::DataType type = rive::DataType::none;
};

// What the host hands the core every cook.
struct FrameParams {
    uint32_t        width  = 1280;
    uint32_t        height = 720;
    rive::Fit       fit    = rive::Fit::contain;
    rive::Alignment alignment = rive::Alignment::center;
    float           speed  = 1.0f;
    // Clear color, packed 0xAARRGGBB to match Rive's ColorInt convention.
    uint32_t        clearColor = 0x00000000;
    // Seconds since the last cook. Multiplied by `speed` before advance.
    float           dt = 0.0f;
};

class RiveCore {
public:
    RiveCore();
    ~RiveCore();

    // Backend must be initialized before any of the file/scene calls below.
    // The core does not own the backend - the host does (so the host can
    // recreate it on a device-lost event etc.).
    bool ensureBackend(std::string& err);
    IBackend* backend() { return mBackend.get(); }

    // Returns true if the file was (re)loaded successfully or was already
    // loaded at this path. Returns false + setError("...") on failure.
    bool loadFile(const std::string& absPath);

    // Empty name = default artboard. Same for state machine.
    bool selectArtboard(const std::string& name);
    bool selectStateMachine(const std::string& name);

    // Introspection (for property panels / info DATs / etc.).
    std::vector<std::string>    artboardNames() const;
    std::vector<std::string>    stateMachineNames() const;
    std::vector<InputInfo>      stateMachineInputs() const;
    std::vector<VMPropertyInfo> viewModelProperties() const;

    // Direct setters for one SMI input. Edge detection (for triggers) and
    // change detection live in the host - this just writes the value.
    bool setNumberInput(const std::string& name, float value);
    bool setBoolInput(const std::string& name, bool value);
    bool fireTriggerInput(const std::string& name);

    // VM property writers, type-coerced.
    bool setVMString(const std::string& name, const std::string& value);
    bool setVMNumber(const std::string& name, float value);
    bool setVMBool(const std::string& name, bool value);
    bool fireVMTrigger(const std::string& name);

    // Falls back to a named text run on the artboard for hosts that want a
    // "write to anything string-shaped" path.
    bool setTextRun(const std::string& name, const std::string& value);

    // Advance the scene/animation by params.dt * params.speed.
    void advance(const FrameParams& params);

    // Render the current scene into a BGRA8 top-left-origin buffer (size =
    // width*height*4) sized to params.width x params.height. Returns false
    // and sets error() on failure.
    bool render(const FrameParams& params, void* dstBGRA8);

    // Resets to a clean state (no file loaded). Called on file-path change
    // or an explicit reload pulse.
    void unloadFile();

    const std::string& error() const { return mError; }
    void clearError() { mError.clear(); }

private:
    void setError(const std::string& s) { mError = s; }
    void bindArtboardViewModel();

    std::unique_ptr<IBackend>                 mBackend;
    bool                                      mBackendReady = false;

    rive::rcp<rive::File>                     mFile;
    std::unique_ptr<rive::ArtboardInstance>   mArtboard;
    std::unique_ptr<rive::Scene>              mScene;
    rive::StateMachineInstance*               mSMI = nullptr;  // non-owning, points into mScene
    rive::rcp<rive::ViewModelInstanceRuntime> mVMRuntime;

    std::string mLoadedPath;
    std::string mLoadedArtboard;
    std::string mLoadedStateMachine;

    std::string mError;
};

} // namespace obsrive
