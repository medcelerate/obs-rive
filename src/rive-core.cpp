// rive-core.cpp

#include "rive-core.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/math/aabb.hpp"
#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/text/text_value_run.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_string_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_number_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_boolean_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_trigger_runtime.hpp"

namespace obsrive {

// SMI input core type keys, mirroring TDRive (avoids dynamic_cast across
// hidden-visibility typeinfo).
static constexpr uint16_t kInputTypeNumber  = 56;
static constexpr uint16_t kInputTypeTrigger = 58;
static constexpr uint16_t kInputTypeBool    = 59;

static InputInfo::Kind kindOf(uint16_t t)
{
    switch (t) {
        case kInputTypeNumber:  return InputInfo::Kind::Number;
        case kInputTypeBool:    return InputInfo::Kind::Bool;
        case kInputTypeTrigger: return InputInfo::Kind::Trigger;
        default:                return InputInfo::Kind::Unknown;
    }
}

RiveCore::RiveCore() = default;
RiveCore::~RiveCore()
{
    mVMRuntime.reset();
    mSMI = nullptr;
    mScene.reset();
    mArtboard.reset();
    mFile.reset();
    mBackend.reset();
}

bool RiveCore::ensureBackend(std::string& err)
{
    if (mBackendReady) return true;
    if (!mBackend) mBackend = CreateBackend();
    if (!mBackend) { err = "Backend factory returned null."; return false; }
    if (!mBackend->init(err)) return false;
    mBackendReady = true;
    return true;
}

void RiveCore::unloadFile()
{
    mVMRuntime.reset();
    mSMI = nullptr;
    mScene.reset();
    mArtboard.reset();
    mFile.reset();
    mLoadedPath.clear();
    mLoadedArtboard.clear();
    mLoadedStateMachine.clear();
}

bool RiveCore::loadFile(const std::string& path)
{
    if (!mBackendReady) { setError("Backend not initialized."); return false; }
    if (path.empty()) { unloadFile(); return false; }
    if (path == mLoadedPath && mFile) return true;

    std::ifstream f(path, std::ios::binary);
    if (!f.good()) { setError("Could not open .riv file: " + path); unloadFile(); return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());

    rive::ImportResult ir;
    auto file = rive::File::import(
        rive::Span<const uint8_t>(bytes.data(), bytes.size()),
        mBackend->factory(), &ir);
    if (!file || ir != rive::ImportResult::success) {
        setError("Failed to parse .riv: " + path);
        unloadFile();
        return false;
    }

    mFile = std::move(file);
    mArtboard.reset(); mScene.reset(); mSMI = nullptr; mVMRuntime.reset();
    mLoadedPath = path;
    mLoadedArtboard.clear();
    mLoadedStateMachine.clear();
    clearError();
    return true;
}

bool RiveCore::selectArtboard(const std::string& name)
{
    if (!mFile) return false;
    if (mArtboard && name == mLoadedArtboard) return true;

    std::unique_ptr<rive::ArtboardInstance> ab =
        name.empty() ? mFile->artboardDefault() : mFile->artboardNamed(name);
    if (!ab) {
        setError(name.empty()
                 ? std::string("No default artboard.")
                 : std::string("Artboard not found: ") + name);
        mArtboard.reset(); mScene.reset(); mSMI = nullptr; mVMRuntime.reset();
        return false;
    }
    mArtboard = std::move(ab);
    mLoadedArtboard = name;
    mSMI = nullptr;
    bindArtboardViewModel();
    clearError();
    return true;
}

bool RiveCore::selectStateMachine(const std::string& name)
{
    if (!mArtboard) return false;
    if (mScene && name == mLoadedStateMachine) return true;

    std::unique_ptr<rive::StateMachineInstance> smi;
    std::unique_ptr<rive::Scene> scene;
    if (!name.empty()) {
        smi = mArtboard->stateMachineNamed(name);
        if (!smi) { setError("State machine not found: " + name); return false; }
        scene = std::move(smi);
        mSMI = static_cast<rive::StateMachineInstance*>(scene.get());
    } else if (mArtboard->stateMachineCount() > 0) {
        int defIdx = mArtboard->defaultStateMachineIndex();
        size_t idx = (defIdx >= 0) ? (size_t)defIdx : 0;
        smi = mArtboard->stateMachineAt(idx);
        if (smi) {
            scene = std::move(smi);
            mSMI = static_cast<rive::StateMachineInstance*>(scene.get());
        }
    }
    if (!scene) {
        scene = mArtboard->defaultScene();
        mSMI = nullptr;
    }
    mScene = std::move(scene);
    mLoadedStateMachine = name;
    clearError();
    return true;
}

void RiveCore::bindArtboardViewModel()
{
    mVMRuntime.reset();
    if (!mFile || !mArtboard) return;
    auto* vmr = mFile->defaultArtboardViewModel(mArtboard.get());
    if (!vmr) return;
    auto runtime = vmr->createDefaultInstance();
    if (!runtime) runtime = vmr->createInstance();
    if (!runtime) return;
    mArtboard->bindViewModelInstance(runtime->instance());
    mVMRuntime = std::move(runtime);
}

// -----------------------------------------------------------------------------
// Introspection

std::vector<std::string> RiveCore::artboardNames() const
{
    std::vector<std::string> v;
    if (!mFile) return v;
    v.reserve(mFile->artboardCount());
    for (size_t i = 0; i < mFile->artboardCount(); ++i) {
        v.emplace_back(mFile->artboardNameAt(i));
    }
    return v;
}

std::vector<std::string> RiveCore::stateMachineNames() const
{
    std::vector<std::string> v;
    if (!mArtboard) return v;
    v.reserve(mArtboard->stateMachineCount());
    for (size_t i = 0; i < mArtboard->stateMachineCount(); ++i) {
        if (auto* sm = mArtboard->stateMachine(i)) v.emplace_back(sm->name());
    }
    return v;
}

std::vector<InputInfo> RiveCore::stateMachineInputs() const
{
    std::vector<InputInfo> v;
    if (!mSMI) return v;
    v.reserve(mSMI->inputCount());
    for (size_t i = 0; i < mSMI->inputCount(); ++i) {
        auto* in = mSMI->input(i);
        v.push_back({ in->name(), kindOf(in->inputCoreType()) });
    }
    return v;
}

std::vector<VMPropertyInfo> RiveCore::viewModelProperties() const
{
    std::vector<VMPropertyInfo> v;
    if (!mVMRuntime) return v;
    auto props = mVMRuntime->properties();
    v.reserve(props.size());
    for (auto& p : props) v.push_back({ p.name, p.type });
    return v;
}

// -----------------------------------------------------------------------------
// Setters

bool RiveCore::setNumberInput(const std::string& name, float value)
{
    if (!mSMI) return false;
    for (size_t i = 0; i < mSMI->inputCount(); ++i) {
        auto* in = mSMI->input(i);
        if (in->name() == name && in->inputCoreType() == kInputTypeNumber) {
            auto* n = static_cast<rive::SMINumber*>(in);
            if (n->value() != value) n->value(value);
            return true;
        }
    }
    return false;
}

bool RiveCore::setBoolInput(const std::string& name, bool value)
{
    if (!mSMI) return false;
    for (size_t i = 0; i < mSMI->inputCount(); ++i) {
        auto* in = mSMI->input(i);
        if (in->name() == name && in->inputCoreType() == kInputTypeBool) {
            auto* b = static_cast<rive::SMIBool*>(in);
            if (b->value() != value) b->value(value);
            return true;
        }
    }
    return false;
}

bool RiveCore::fireTriggerInput(const std::string& name)
{
    if (!mSMI) return false;
    for (size_t i = 0; i < mSMI->inputCount(); ++i) {
        auto* in = mSMI->input(i);
        if (in->name() == name && in->inputCoreType() == kInputTypeTrigger) {
            static_cast<rive::SMITrigger*>(in)->fire();
            return true;
        }
    }
    return false;
}

bool RiveCore::setVMString(const std::string& name, const std::string& value)
{
    if (!mVMRuntime) return false;
    auto* sp = mVMRuntime->propertyString(name);
    if (!sp) return false;
    if (sp->value() != value) sp->value(value);
    return true;
}

bool RiveCore::setVMNumber(const std::string& name, float value)
{
    if (!mVMRuntime) return false;
    auto* np = mVMRuntime->propertyNumber(name);
    if (!np) return false;
    if (np->value() != value) np->value(value);
    return true;
}

bool RiveCore::setVMBool(const std::string& name, bool value)
{
    if (!mVMRuntime) return false;
    auto* bp = mVMRuntime->propertyBoolean(name);
    if (!bp) return false;
    if (bp->value() != value) bp->value(value);
    return true;
}

bool RiveCore::fireVMTrigger(const std::string& name)
{
    if (!mVMRuntime) return false;
    auto* tp = mVMRuntime->propertyTrigger(name);
    if (!tp) return false;
    tp->trigger();
    return true;
}

bool RiveCore::setTextRun(const std::string& name, const std::string& value)
{
    if (!mArtboard) return false;
    auto* tvr = mArtboard->getTextRun(name, "");
    if (!tvr) return false;
    if (tvr->text() != value) tvr->text(value);
    return true;
}

// -----------------------------------------------------------------------------
// Tick + render

void RiveCore::advance(const FrameParams& params)
{
    float dt = std::max(0.0f, params.dt) * std::max(0.0f, params.speed);
    if (mScene)         mScene->advanceAndApply(dt);
    else if (mArtboard) mArtboard->advance(dt);
}

bool RiveCore::render(const FrameParams& params, void* dst)
{
    if (!mBackendReady) { setError("Backend not initialized."); return false; }
    std::string err;
    if (!mBackend->ensureRenderTarget(params.width, params.height, err)) {
        setError(err);
        return false;
    }

    rive::gpu::RenderContext::FrameDescriptor fd;
    fd.renderTargetWidth  = params.width;
    fd.renderTargetHeight = params.height;
    fd.loadAction = rive::gpu::LoadAction::clear;
    fd.clearColor = params.clearColor;

    auto draw = [this, params](rive::Renderer* r) {
        if (!mArtboard) return;
        r->save();
        r->align(params.fit, params.alignment,
                 rive::AABB(0, 0, (float)params.width, (float)params.height),
                 mArtboard->bounds());
        mArtboard->draw(r);
        r->restore();
    };

    if (!mBackend->renderAndReadback(fd, draw, dst, err)) {
        setError(err);
        return false;
    }
    clearError();
    return true;
}

} // namespace obsrive
