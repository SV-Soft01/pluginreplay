#ifdef _MSC_VER
#pragma warning(disable: 4996)
#endif
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "multicam-manager.hpp"
#include "ui/multicam-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("sv-multicam-replay", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
    return "SV MultiCam Replay";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Búfer de repetición independiente y sincronizado para múltiples cámaras deportivas en OBS Studio.";
}

MODULE_EXPORT const char *obs_module_author(void)
{
    return "SV MultiCam Team";
}

// Puntero estático al dock de interfaz
static SVReplay::MultiCamDock *s_dock = nullptr;

// Función ejecutada al presionar el atajo de teclado global registrado en OBS
static void OnHotkeySaveReplay(void * /*data*/, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
    if (!pressed) {
        return;
    }
    blog(LOG_INFO, "[SV MultiCam] Atajo de teclado presionado: Guardando repetición multicámara...");
    SVReplay::MultiCamManager::Instance().TriggerSaveReplay([](const SVReplay::ReplaySaveResult &result) {
        if (result.success) {
            blog(LOG_INFO, "[SV MultiCam] Repetición #%04u guardada correctamente por atajo.", result.playId);
        } else {
            blog(LOG_ERROR, "[SV MultiCam] Fallo al guardar repetición por atajo.");
        }
    });
}

// Callback de eventos de la interfaz de OBS Studio
static void OnFrontendEvent(enum obs_frontend_event event, void * /*private_data*/)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        // Inicializar el gestor una vez que todas las fuentes y plugins de OBS han cargado
        SVReplay::MultiCamManager::Instance().Initialize();

        // Crear y acoplar el panel de interfaz en OBS Studio
        s_dock = new SVReplay::MultiCamDock();
        obs_frontend_add_dock(s_dock);
        blog(LOG_INFO, "[SV MultiCam] Panel acoplable agregado a la interfaz de OBS.");
        break;

    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
    case OBS_FRONTEND_EVENT_EXIT:
        // Guardar configuración y liberar recursos antes de salir
        SVReplay::MultiCamManager::Instance().Shutdown();
        break;

    default:
        break;
    }
}

MODULE_EXPORT bool obs_module_load(void)
{
    blog(LOG_INFO, "[SV MultiCam] Cargando plugin SV MultiCam Replay v1.0.0...");

    // 1. Registrar el atajo de teclado en la lista nativa de Atajos de OBS Studio
    obs_hotkey_register_frontend(
        "sv_multicam_save_replay_hotkey",
        "SV MultiCam: Guardar Repetición Multicámara",
        OnHotkeySaveReplay,
        nullptr
    );

    // 2. Suscribirse a los eventos del frontend para agregar el Dock cuando OBS esté listo
    obs_frontend_add_event_callback(OnFrontendEvent, nullptr);

    blog(LOG_INFO, "[SV MultiCam] Plugin cargado y atajo registrado satisfactoriamente.");
    return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
    blog(LOG_INFO, "[SV MultiCam] Descargando plugin SV MultiCam Replay...");
    SVReplay::MultiCamManager::Instance().Shutdown();
    blog(LOG_INFO, "[SV MultiCam] Plugin descargado.");
}
