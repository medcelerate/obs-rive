// plugin-main.cpp
//
// OBS module entry points. Module-level glue only - the source registration
// itself lives in rive-source.cpp.

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-rive", "en-US")

MODULE_EXPORT const char* obs_module_name(void)        { return "Rive";        }
MODULE_EXPORT const char* obs_module_description(void) { return "Render Rive (.riv) files as an OBS source."; }

extern "C" void register_rive_source(void);

bool obs_module_load(void)
{
    register_rive_source();
    blog(LOG_INFO, "[obs-rive] loaded");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-rive] unloaded");
}
