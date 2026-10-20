#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/base.h>

#include <string>
#include <vector>

#include "multicam-manager.hpp"
#include "ui/multicam-dock.hpp"

#ifndef SV_PLUGIN_VERSION
#define SV_PLUGIN_VERSION "1.0.0"
#endif

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void)
{
    return "SV MultiCam Replay";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Búfer de repetición comprimido (H.264) y sincronizado para múltiples cámaras.";
}

MODULE_EXPORT const char *obs_module_author(void)
{
    return "SV MultiCam Team";
}

namespace {

constexpr const char *kSaveDataKey = "sv_multicam_replay";

SVReplay::MultiCamDock *s_dock = nullptr;
obs_hotkey_id s_hotkeyId = OBS_INVALID_HOTKEY_ID;

void OnHotkeySaveReplay(void * /*data*/, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
    if (!pressed) {
        return;
    }
    blog(LOG_INFO, "[SV MultiCam] Atajo presionado: guardando repetición multicámara.");
    std::string error;
    if (!SVReplay::MultiCamManager::Instance().TriggerSaveReplay(&error)) {
        blog(LOG_WARNING, "[SV MultiCam] No se inició el guardado: %s", error.c_str());
    }
}

// Las cámaras elegidas y el atajo se guardan DENTRO de la colección de escenas,
// porque los nombres de las fuentes pertenecen a cada colección.
void OnSaveCallback(obs_data_t *saveData, bool saving, void * /*privateData*/)
{
    auto &manager = SVReplay::MultiCamManager::Instance();

    if (saving) {
        obs_data_t *mine = obs_data_create();

        obs_data_array_t *cameras = obs_data_array_create();
        for (const std::string &name : manager.GetEnabledCameras()) {
            obs_data_t *item = obs_data_create();
            obs_data_set_string(item, "name", name.c_str());
            obs_data_array_push_back(cameras, item);
            obs_data_release(item);
        }
        obs_data_set_array(mine, "cameras", cameras);
        obs_data_array_release(cameras);

        if (s_hotkeyId != OBS_INVALID_HOTKEY_ID) {
            obs_data_array_t *hotkeyData = obs_hotkey_save(s_hotkeyId);
            obs_data_set_array(mine, "hotkey", hotkeyData);
            obs_data_array_release(hotkeyData);
        }

        obs_data_set_obj(saveData, kSaveDataKey, mine);
        obs_data_release(mine);
        return;
    }

    // Carga de una colección de escenas.
    std::vector<std::string> names;
    obs_data_t *mine = obs_data_get_obj(saveData, kSaveDataKey);
    if (mine) {
        obs_data_array_t *cameras = obs_data_get_array(mine, "cameras");
        if (cameras) {
            const size_t count = obs_data_array_count(cameras);
            for (size_t i = 0; i < count; ++i) {
                obs_data_t *item = obs_data_array_item(cameras, i);
                const char *name = obs_data_get_string(item, "name");
                if (name && *name) {
                    names.emplace_back(name);
                }
                obs_data_release(item);
            }
            obs_data_array_release(cameras);
        }

        obs_data_array_t *hotkeyData = obs_data_get_array(mine, "hotkey");
        if (hotkeyData) {
            if (s_hotkeyId != OBS_INVALID_HOTKEY_ID) {
                obs_hotkey_load(s_hotkeyId, hotkeyData);
            }
            obs_data_array_release(hotkeyData);
        }
        obs_data_release(mine);
    }
    // Solo se anota la lista; las cámaras arrancan al terminar la carga (evento).
    manager.SetEnabledCameras(names);
}

void AddDockOnce()
{
    if (s_dock) {
        return;
    }
    s_dock = new SVReplay::MultiCamDock();
    if (!obs_frontend_add_dock_by_id("sv_multicam_replay_dock", "SV MultiCam Replay", s_dock)) {
        blog(LOG_WARNING, "[SV MultiCam] OBS no aceptó el panel acoplable.");
        delete s_dock;
        s_dock = nullptr;
        return;
    }
    blog(LOG_INFO, "[SV MultiCam] Panel acoplable agregado.");
}

void OnFrontendEvent(enum obs_frontend_event event, void * /*privateData*/)
{
    auto &manager = SVReplay::MultiCamManager::Instance();

    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        AddDockOnce();
        manager.ApplyEnabledCameras();
        if (s_dock) {
            s_dock->RefreshAvailableSources();
        }
        break;

    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        manager.ApplyEnabledCameras();
        if (s_dock) {
            s_dock->RefreshAvailableSources();
        }
        break;

    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
        // Las fuentes de la colección actual van a destruirse: hay que soltarlas ya.
        manager.StopAllPipelines();
        break;

    case OBS_FRONTEND_EVENT_EXIT:
        manager.Shutdown();
        break;

    default:
        break;
    }
}

} // namespace

MODULE_EXPORT bool obs_module_load(void)
{
    blog(LOG_INFO, "[SV MultiCam] Cargando SV MultiCam Replay v%s...", SV_PLUGIN_VERSION);

    char *configPath = obs_module_config_path("config.json");
    const std::string configFile = configPath ? configPath : "";
    bfree(configPath);
    SVReplay::MultiCamManager::Instance().Initialize(configFile);

    s_hotkeyId = obs_hotkey_register_frontend("sv_multicam_save_replay_hotkey",
                                              "SV MultiCam: Guardar repetición multicámara", OnHotkeySaveReplay,
                                              nullptr);

    obs_frontend_add_save_callback(OnSaveCallback, nullptr);
    obs_frontend_add_event_callback(OnFrontendEvent, nullptr);

    blog(LOG_INFO, "[SV MultiCam] Plugin cargado.");
    return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
    obs_frontend_remove_save_callback(OnSaveCallback, nullptr);
    SVReplay::MultiCamManager::Instance().Shutdown();
    blog(LOG_INFO, "[SV MultiCam] Plugin descargado.");
}