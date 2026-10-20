#include "multicam-manager.hpp"
#include "mp4-muxer.hpp"
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace SVReplay {

namespace fs = std::filesystem;

static void OBS_MainRenderCallback(void *param, uint32_t /*cx*/, uint32_t /*cy*/)
{
    auto *manager = static_cast<MultiCamManager*>(param);
    if (manager) {
        manager->OnMainRenderTick();
    }
}

MultiCamManager& MultiCamManager::Instance()
{
    static MultiCamManager s_instance;
    return s_instance;
}

MultiCamManager::MultiCamManager() = default;

MultiCamManager::~MultiCamManager()
{
    Shutdown();
}

void MultiCamManager::Initialize()
{
    if (m_isInitialized.exchange(true)) {
        return;
    }

    // Ruta de salida por defecto en la carpeta de videos del usuario
    char *userVideoPath = os_get_default_recording_path();
    if (userVideoPath) {
        m_outputDirectory = userVideoPath;
        bfree(userVideoPath);
    } else {
        m_outputDirectory = "C:\\Replays";
    }

    LoadConfiguration();

    // Registrar el callback de renderizado en el subsistema gráfico de OBS
    obs_enter_graphics();
    if (!m_renderCallbackRegistered) {
        obs_add_main_render_callback(OBS_MainRenderCallback, this);
        m_renderCallbackRegistered = true;
    }
    obs_leave_graphics();

    blog(LOG_INFO, "[SV MultiCam] Gestor multicámara inicializado con éxito.");
}

void MultiCamManager::Shutdown()
{
    if (!m_isInitialized.exchange(false)) {
        return;
    }

    SaveConfiguration();

    obs_enter_graphics();
    if (m_renderCallbackRegistered) {
        obs_remove_main_render_callback(OBS_MainRenderCallback, this);
        m_renderCallbackRegistered = false;
    }
    obs_leave_graphics();

    std::lock_guard<std::mutex> lock(m_managerMutex);
    for (auto &pair : m_sources) {
        if (pair.second) {
            pair.second->StopCapture();
        }
    }
    m_sources.clear();
    blog(LOG_INFO, "[SV MultiCam] Gestor multicámara finalizado correctamente.");
}

void MultiCamManager::OnMainRenderTick()
{
    if (!m_isInitialized.load()) {
        return;
    }

    uint64_t now_ns = os_gettime_ns();

    std::lock_guard<std::mutex> lock(m_managerMutex);
    for (auto &pair : m_sources) {
        if (pair.second) {
            pair.second->RenderAndCapture(now_ns);
        }
    }
}

void MultiCamManager::SetSourceActive(const std::string &sourceName, bool active)
{
    std::lock_guard<std::mutex> lock(m_managerMutex);

    auto it = m_sources.find(sourceName);
    if (active) {
        if (it == m_sources.end()) {
            auto buffer = std::make_unique<SourceBuffer>(sourceName);
            buffer->SetBufferDuration(m_bufferDuration);
            buffer->StartCapture();
            m_sources[sourceName] = std::move(buffer);
        }
    } else {
        if (it != m_sources.end()) {
            it->second->StopCapture();
            m_sources.erase(it);
        }
    }
}

bool MultiCamManager::IsSourceActive(const std::string &sourceName) const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    return m_sources.find(sourceName) != m_sources.end();
}

std::vector<std::string> MultiCamManager::GetActiveSources() const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    std::vector<std::string> activeList;
    activeList.reserve(m_sources.size());
    for (const auto &pair : m_sources) {
        activeList.push_back(pair.first);
    }
    return activeList;
}

void MultiCamManager::SetBufferDuration(int seconds)
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    m_bufferDuration = seconds;
    for (auto &pair : m_sources) {
        if (pair.second) {
            pair.second->SetBufferDuration(seconds);
        }
    }
}

void MultiCamManager::SetOutputDirectory(const std::string &dirPath)
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    m_outputDirectory = dirPath;
}

std::string MultiCamManager::GetOutputDirectory() const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    return m_outputDirectory;
}

void MultiCamManager::SetFilePrefix(const std::string &prefix)
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    m_filePrefix = prefix;
}

std::string MultiCamManager::GetFilePrefix() const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    return m_filePrefix;
}

size_t MultiCamManager::GetTotalEstimatedMemoryBytes() const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    size_t total = 0;
    for (const auto &pair : m_sources) {
        if (pair.second) {
            total += pair.second->GetEstimatedMemoryBytes();
        }
    }
    return total;
}

std::string MultiCamManager::GetSourceStatusString(const std::string &sourceName) const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    auto it = m_sources.find(sourceName);
    if (it != m_sources.end() && it->second) {
        return it->second->GetStatusString();
    }
    return "Inactivo";
}

size_t MultiCamManager::GetSourceBufferedFrames(const std::string &sourceName) const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    auto it = m_sources.find(sourceName);
    if (it != m_sources.end() && it->second) {
        return it->second->GetBufferedFrameCount();
    }
    return 0;
}

double MultiCamManager::GetSourceBufferedSeconds(const std::string &sourceName) const
{
    std::lock_guard<std::mutex> lock(m_managerMutex);
    auto it = m_sources.find(sourceName);
    if (it != m_sources.end() && it->second) {
        return it->second->GetBufferedDurationSeconds();
    }
    return 0.0;
}

std::string MultiCamManager::GenerateTimestampString() const
{
    auto now = std::chrono::system_clock::now();
    std::time_t in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf;
#if defined(_WIN32)
    localtime_s(&buf, &in_time_t);
#else
    localtime_r(&in_time_t, &buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&buf, "%Y%m%d_%H%M%S");
    return ss.str();
}

bool MultiCamManager::TriggerSaveReplay(SaveCompletionCallback callback)
{
    bool expected = false;
    if (!m_isSaving.compare_exchange_strong(expected, true)) {
        blog(LOG_WARNING, "[SV MultiCam] Ya hay un proceso de guardado en curso. Ignorando disparo.");
        return false;
    }

    uint64_t now_ns = os_gettime_ns();
    uint64_t duration_ns = static_cast<uint64_t>(m_bufferDuration) * 1000000000ULL;
    uint64_t start_ns = (now_ns > duration_ns) ? (now_ns - duration_ns) : 0;

    uint32_t playId = m_playCounter.fetch_add(1);
    std::string timestampStr = GenerateTimestampString();
    std::string outputDir = GetOutputDirectory();
    std::string prefix = GetFilePrefix();

    // Estructura de extracción de fotogramas sincronizados
    struct SourceExtraction {
        std::string name;
        std::vector<VideoFramePacket> frames;
    };
    std::vector<SourceExtraction> extractions;

    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        extractions.reserve(m_sources.size());
        for (const auto &pair : m_sources) {
            if (pair.second) {
                SourceExtraction ext;
                ext.name = pair.first;
                ext.frames = pair.second->ExtractReplaySlice(start_ns, now_ns);
                extractions.push_back(std::move(ext));
            }
        }
    }

    if (extractions.empty()) {
        m_isSaving.store(false);
        blog(LOG_WARNING, "[SV MultiCam] No hay fuentes activas seleccionadas para guardar repetición.");
        return false;
    }

    // Delegar la codificación y escritura de archivos MP4 al hilo de trabajo asíncrono
    std::thread([this, extractions = std::move(extractions), playId, timestampStr, outputDir, prefix, callback]() {
        ReplaySaveResult result;
        result.playId = playId;
        result.success = true;

        blog(LOG_INFO, "[SV MultiCam] Iniciando guardado asíncrono de repetición #%04u para %zu cámaras...",
             playId, extractions.size());

        for (const auto &item : extractions) {
            MuxerOptions options;
            options.outputDirectory = outputDir;
            options.filenamePrefix = prefix;
            options.playId = playId;
            options.timestampStr = timestampStr;
            options.sourceName = item.name;
            options.fps = 60;

            std::string finalPath;
            std::string errorMessage;

            bool success = MP4Muxer::SaveClipsToMP4(options, item.frames, finalPath, errorMessage);
            if (success) {
                result.savedFiles.push_back(finalPath);
                result.totalClips++;
                blog(LOG_INFO, "[SV MultiCam] Clip generado exitosamente: %s", finalPath.c_str());
            } else {
                result.success = false;
                result.errors.push_back(item.name + ": " + errorMessage);
                blog(LOG_ERROR, "[SV MultiCam] Error al guardar clip para '%s': %s",
                     item.name.c_str(), errorMessage.c_str());
            }
        }

        m_isSaving.store(false);

        if (callback) {
            callback(result);
        }
    }).detach();

    return true;
}

void MultiCamManager::LoadConfiguration()
{
    char *configPath = obs_module_config_path(obs_current_module(), "config.json");
    if (!configPath) {
        return;
    }

    obs_data_t *data = obs_data_create_from_json_file(configPath);
    if (data) {
        m_bufferDuration = static_cast<int>(obs_data_get_int(data, "buffer_duration_sec"));
        if (m_bufferDuration <= 0) m_bufferDuration = 10;

        const char *dir = obs_data_get_string(data, "output_directory");
        if (dir && strlen(dir) > 0) {
            m_outputDirectory = dir;
        }

        const char *prefix = obs_data_get_string(data, "filename_prefix");
        if (prefix && strlen(prefix) > 0) {
            m_filePrefix = prefix;
        }

        m_playCounter.store(static_cast<uint32_t>(obs_data_get_int(data, "last_play_id")) + 1);

        obs_data_array_t *sourcesArray = obs_data_get_array(data, "active_sources");
        if (sourcesArray) {
            size_t count = obs_data_array_count(sourcesArray);
            for (size_t i = 0; i < count; ++i) {
                obs_data_t *item = obs_data_array_item(sourcesArray, i);
                const char *sName = obs_data_get_string(item, "name");
                if (sName && strlen(sName) > 0) {
                    SetSourceActive(sName, true);
                }
                obs_data_release(item);
            }
            obs_data_array_release(sourcesArray);
        }

        obs_data_release(data);
        blog(LOG_INFO, "[SV MultiCam] Configuración cargada desde %s", configPath);
    }
    bfree(configPath);
}

void MultiCamManager::SaveConfiguration()
{
    char *configPath = obs_module_config_path(obs_current_module(), "config.json");
    if (!configPath) {
        return;
    }

    obs_data_t *data = obs_data_create();
    obs_data_set_int(data, "buffer_duration_sec", m_bufferDuration);
    obs_data_set_string(data, "output_directory", m_outputDirectory.c_str());
    obs_data_set_string(data, "filename_prefix", m_filePrefix.c_str());
    obs_data_set_int(data, "last_play_id", m_playCounter.load());

    obs_data_array_t *sourcesArray = obs_data_array_create();
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        for (const auto &pair : m_sources) {
            obs_data_t *item = obs_data_create();
            obs_data_set_string(item, "name", pair.first.c_str());
            obs_data_array_push_back(sourcesArray, item);
            obs_data_release(item);
        }
    }
    obs_data_set_array(data, "active_sources", sourcesArray);
    obs_data_array_release(sourcesArray);

    obs_data_save_json_safe(data, configPath, "tmp", "bak");
    obs_data_release(data);
    bfree(configPath);
    blog(LOG_INFO, "[SV MultiCam] Configuración guardada correctamente.");
}

} // namespace SVReplay
