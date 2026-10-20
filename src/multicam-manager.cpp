#include "multicam-manager.hpp"

#include <obs.h>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace SVReplay {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kSessionTimeoutNs = 30ULL * 1000000000ULL;

std::string PathToUtf8(const fs::path &path)
{
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string SanitizeForFilename(const std::string &name)
{
    static const std::string illegal = "\\/:*?\"<>| ";
    std::string out = name;
    for (char &c : out) {
        if (illegal.find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) {
            c = '_';
        }
    }
    return out.empty() ? std::string("camara") : out;
}

const char *EncoderToString(EncoderPreference pref)
{
    switch (pref) {
    case EncoderPreference::Nvenc:
        return "nvenc";
    case EncoderPreference::X264:
        return "x264";
    case EncoderPreference::Auto:
    default:
        return "auto";
    }
}

EncoderPreference EncoderFromString(const std::string &text)
{
    if (text == "nvenc") {
        return EncoderPreference::Nvenc;
    }
    if (text == "x264") {
        return EncoderPreference::X264;
    }
    return EncoderPreference::Auto;
}

} // namespace

MultiCamManager &MultiCamManager::Instance()
{
    static MultiCamManager instance;
    return instance;
}

MultiCamManager::~MultiCamManager()
{
    Shutdown();
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------

void MultiCamManager::Initialize(const std::string &configFilePath)
{
    if (m_initialized.exchange(true)) {
        return;
    }

    m_configPath = configFilePath;
    LoadGlobalConfig();

    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerStop = false;
    }
    m_worker = std::thread(&MultiCamManager::WorkerLoop, this);

    blog(LOG_INFO, "[SV MultiCam] Gestor multicámara inicializado.");
}

void MultiCamManager::Shutdown()
{
    if (!m_initialized.exchange(false)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerStop = true;
    }
    m_workerCv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }

    StopAllPipelines();

    // Esperar (máx. 5 s) a que terminen los renombrados de clips en curso.
    for (int i = 0; i < 100 && m_pendingWorkers.load() > 0; ++i) {
        os_sleep_ms(50);
    }

    SetResultListener(nullptr);
    SaveGlobalConfig();
    blog(LOG_INFO, "[SV MultiCam] Gestor multicámara finalizado.");
}

// ---------------------------------------------------------------------------
// Hilo de supervisión
// ---------------------------------------------------------------------------

void MultiCamManager::WorkerLoop()
{
    std::unique_lock<std::mutex> lock(m_workerMutex);
    while (!m_workerStop) {
        m_workerCv.wait_for(lock, std::chrono::milliseconds(1000), [this] { return m_workerStop; });
        if (m_workerStop) {
            break;
        }
        lock.unlock();
        TickPipelines();
        CheckSessionTimeout();
        lock.lock();
    }
}

void MultiCamManager::TickPipelines()
{
    std::vector<std::shared_ptr<CameraPipeline>> graveyard;
    std::vector<std::shared_ptr<CameraPipeline>> active;
    ReplaySettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        graveyard.swap(m_graveyard);
        settings = m_settings;
        active.reserve(m_pipelines.size());
        for (auto &pair : m_pipelines) {
            active.push_back(pair.second);
        }
    }

    // Detener (fuera del candado) las cámaras que el usuario desmarcó.
    for (auto &pipeline : graveyard) {
        pipeline->Retire();
    }
    graveyard.clear();

    for (auto &pipeline : active) {
        pipeline->Tick(settings);
    }

    RefreshStatusCache();
}

void MultiCamManager::RefreshStatusCache()
{
    std::vector<std::shared_ptr<CameraPipeline>> pipelines;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pipelines.reserve(m_pipelines.size());
        for (auto &pair : m_pipelines) {
            pipelines.push_back(pair.second);
        }
    }

    std::vector<CameraStatus> statuses;
    statuses.reserve(pipelines.size());
    for (auto &pipeline : pipelines) {
        statuses.push_back(pipeline->GetStatus());
    }

    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusCache = std::move(statuses);
}

std::vector<CameraStatus> MultiCamManager::GetStatuses() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_statusCache;
}

// ---------------------------------------------------------------------------
// Cámaras
// ---------------------------------------------------------------------------

void MultiCamManager::SetCameraEnabled(const std::string &name, bool enabled)
{
    if (name.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (enabled) {
            m_desired.insert(name);
        } else {
            m_desired.erase(name);
        }
        ReconcileLocked();
    }
    RefreshStatusCache();
}

bool MultiCamManager::IsCameraEnabled(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_desired.count(name) > 0;
}

std::vector<std::string> MultiCamManager::GetEnabledCameras() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<std::string>(m_desired.begin(), m_desired.end());
}

void MultiCamManager::SetEnabledCameras(const std::vector<std::string> &names)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_desired.clear();
    for (const auto &name : names) {
        if (!name.empty()) {
            m_desired.insert(name);
        }
    }
}

void MultiCamManager::ApplyEnabledCameras()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ReconcileLocked();
    }
    RefreshStatusCache();
}

void MultiCamManager::ReconcileLocked()
{
    // Pipelines que sobran -> al cementerio (los detiene el hilo de supervisión).
    for (auto it = m_pipelines.begin(); it != m_pipelines.end();) {
        if (m_desired.count(it->first) == 0) {
            m_graveyard.push_back(std::move(it->second));
            it = m_pipelines.erase(it);
        } else {
            ++it;
        }
    }

    // Pipelines que faltan -> se crean detenidos; el siguiente Tick los arranca.
    for (const auto &name : m_desired) {
        if (m_pipelines.count(name) == 0) {
            m_pipelines[name] = std::make_shared<CameraPipeline>(
                name, [this](const std::string &camera, const std::string &path) { OnClipSaved(camera, path); });
        }
    }
}

void MultiCamManager::StopAllPipelines()
{
    std::vector<std::shared_ptr<CameraPipeline>> toStop;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto &pair : m_pipelines) {
            toStop.push_back(std::move(pair.second));
        }
        m_pipelines.clear();
        for (auto &pipeline : m_graveyard) {
            toStop.push_back(std::move(pipeline));
        }
        m_graveyard.clear();
    }

    for (auto &pipeline : toStop) {
        pipeline->Retire();
    }
    toStop.clear();

    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusCache.clear();
}

// ---------------------------------------------------------------------------
// Ajustes
// ---------------------------------------------------------------------------

ReplaySettings MultiCamManager::GetSettings() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

void MultiCamManager::UpdateSettings(const ReplaySettings &newSettings)
{
    ReplaySettings clamped = newSettings;
    clamped.Clamp();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool restart = m_settings.RequiresPipelineRestart(clamped);
        m_settings = clamped;
        if (restart) {
            for (auto &pair : m_pipelines) {
                pair.second->RequestRestart();
            }
        }
    }
    SaveGlobalConfig();
}

void MultiCamManager::LoadGlobalConfig()
{
    if (m_configPath.empty()) {
        return;
    }

    obs_data_t *data = obs_data_create_from_json_file(m_configPath.c_str());
    if (!data) {
        return;
    }

    ReplaySettings loaded;
    loaded.bufferSeconds = static_cast<int>(obs_data_get_int(data, "buffer_seconds"));
    if (loaded.bufferSeconds <= 0) {
        loaded.bufferSeconds = 10;
    }
    loaded.videoBitrateKbps = static_cast<int>(obs_data_get_int(data, "video_bitrate_kbps"));
    if (loaded.videoBitrateKbps <= 0) {
        loaded.videoBitrateKbps = 12000;
    }
    loaded.audioTrack = static_cast<int>(obs_data_get_int(data, "audio_track"));
    if (loaded.audioTrack <= 0) {
        loaded.audioTrack = 6;
    }
    loaded.encoder = EncoderFromString(obs_data_get_string(data, "encoder"));

    const char *directory = obs_data_get_string(data, "output_directory");
    if (directory && *directory) {
        loaded.outputDirectory = directory;
    }
    const char *prefix = obs_data_get_string(data, "file_prefix");
    if (prefix && *prefix) {
        loaded.filePrefix = prefix;
    }

    const auto lastPlay = static_cast<uint32_t>(obs_data_get_int(data, "last_play_id"));
    m_playCounter.store(lastPlay + 1);

    obs_data_release(data);
    loaded.Clamp();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings = loaded;
}

void MultiCamManager::SaveGlobalConfig()
{
    if (m_configPath.empty()) {
        return;
    }

    // La carpeta de configuración del plugin puede no existir todavía.
    std::error_code ec;
    fs::create_directories(fs::u8path(m_configPath).parent_path(), ec);

    ReplaySettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        settings = m_settings;
    }

    obs_data_t *data = obs_data_create();
    obs_data_set_int(data, "buffer_seconds", settings.bufferSeconds);
    obs_data_set_int(data, "video_bitrate_kbps", settings.videoBitrateKbps);
    obs_data_set_int(data, "audio_track", settings.audioTrack);
    obs_data_set_string(data, "encoder", EncoderToString(settings.encoder));
    obs_data_set_string(data, "output_directory", settings.outputDirectory.c_str());
    obs_data_set_string(data, "file_prefix", settings.filePrefix.c_str());
    obs_data_set_int(data, "last_play_id", static_cast<long long>(m_playCounter.load()) - 1);

    if (!obs_data_save_json_safe(data, m_configPath.c_str(), "tmp", "bak")) {
        blog(LOG_WARNING, "[SV MultiCam] No se pudo guardar la configuración en %s", m_configPath.c_str());
    }
    obs_data_release(data);
}

// ---------------------------------------------------------------------------
// Guardado de repeticiones
// ---------------------------------------------------------------------------

void MultiCamManager::SetResultListener(SaveResultListener listener)
{
    std::lock_guard<std::mutex> lock(m_listenerMutex);
    m_listener = std::move(listener);
}

bool MultiCamManager::IsSaving() const
{
    std::lock_guard<std::mutex> lock(m_sessionMutex);
    return m_session.active;
}

std::string MultiCamManager::GenerateTimestampString()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    std::ostringstream ss;
    ss << std::put_time(&local, "%Y%m%d_%H%M%S");
    return ss.str();
}

bool MultiCamManager::TriggerSaveReplay(std::string *outError)
{
    auto fail = [&](const std::string &message) {
        blog(LOG_WARNING, "[SV MultiCam] %s", message.c_str());
        if (outError) {
            *outError = message;
        }
        return false;
    };

    // Cámaras que tienen búfer activo en este instante.
    std::vector<std::shared_ptr<CameraPipeline>> pipelines;
    ReplaySettings settings;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        settings = m_settings;
        for (auto &pair : m_pipelines) {
            pipelines.push_back(pair.second);
        }
    }
    if (pipelines.empty()) {
        return fail("No hay cámaras seleccionadas.");
    }

    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        if (m_session.active) {
            return fail("Ya hay un guardado en curso.");
        }
        m_session = SaveSession();
        m_session.active = true;
        m_session.playId = m_playCounter.fetch_add(1);
        m_session.timestamp = GenerateTimestampString();
        m_session.prefix = settings.filePrefix;
        m_session.startedNs = os_gettime_ns();
        m_session.result.playId = m_session.playId;
        for (auto &pipeline : pipelines) {
            m_session.pending.insert(pipeline->Name());
        }
    }

    // Disparar todas las cámaras seguidas: comparten el mismo instante de corte.
    std::vector<std::string> notReady;
    for (auto &pipeline : pipelines) {
        if (!pipeline->RequestSave()) {
            notReady.push_back(pipeline->Name());
        }
    }

    bool finishNow = false;
    SaveSession snapshot;
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        for (const auto &name : notReady) {
            m_session.pending.erase(name);
            m_session.result.errors.push_back(name + ": la cámara aún no tiene búfer activo.");
        }
        if (m_session.pending.empty()) {
            finishNow = true;
            snapshot = m_session;
            m_session.active = false;
        }
    }

    SaveGlobalConfig(); // persiste el contador de jugadas

    if (finishNow) {
        FinishSession(snapshot);
        if (outError) {
            *outError = "Ninguna cámara tenía búfer activo todavía.";
        }
        return false;
    }

    blog(LOG_INFO, "[SV MultiCam] Guardando repetición #%04u en %zu cámaras...", m_session.playId,
         pipelines.size() - notReady.size());
    return true;
}

void MultiCamManager::OnClipSaved(const std::string &cameraName, const std::string &savedPath)
{
    uint32_t playId = 0;
    std::string timestamp;
    std::string prefix;
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        if (!m_session.active || m_session.pending.count(cameraName) == 0) {
            return;
        }
        playId = m_session.playId;
        timestamp = m_session.timestamp;
        prefix = m_session.prefix;
    }

    // El renombrado puede tener que esperar a que OBS suelte el archivo: se hace fuera del
    // hilo de OBS, con reintentos.
    m_pendingWorkers.fetch_add(1);
    std::thread([this, cameraName, savedPath, playId, timestamp, prefix]() {
        FinalizeClip(cameraName, savedPath, playId, timestamp, prefix);
        m_pendingWorkers.fetch_sub(1);
    }).detach();
}

void MultiCamManager::FinalizeClip(const std::string &cameraName, const std::string &savedPath, uint32_t playId,
                                   const std::string &timestamp, const std::string &prefix)
{
    if (savedPath.empty()) {
        CompleteClip(cameraName, false, "", "OBS no informó la ruta del clip guardado.");
        return;
    }

    const fs::path source = fs::u8path(savedPath);
    const fs::path directory = source.parent_path();
    std::string extension = PathToUtf8(source.extension());
    if (extension.empty()) {
        extension = ".mp4";
    }

    std::ostringstream name;
    name << SanitizeForFilename(prefix.empty() ? std::string("replay") : prefix) << "_" << std::setw(4)
         << std::setfill('0') << playId << "_" << timestamp << "_" << SanitizeForFilename(cameraName);
    const std::string baseName = name.str();

    fs::path target = directory / fs::u8path(baseName + extension);
    std::error_code ec;
    for (int suffix = 1; fs::exists(target, ec) && suffix < 100; ++suffix) {
        target = directory / fs::u8path(baseName + "_" + std::to_string(suffix) + extension);
    }

    std::string lastError;
    for (int attempt = 0; attempt < 20; ++attempt) {
        ec.clear();
        if (fs::exists(source, ec)) {
            fs::rename(source, target, ec);
            if (!ec) {
                CompleteClip(cameraName, true, PathToUtf8(target), "");
                return;
            }
            lastError = ec.message();
        } else {
            lastError = "el archivo guardado por OBS no existe.";
        }
        os_sleep_ms(250);
    }

    CompleteClip(cameraName, false, "", "No se pudo renombrar el clip: " + lastError);
}

void MultiCamManager::CompleteClip(const std::string &cameraName, bool ok, const std::string &finalPath,
                                   const std::string &error)
{
    SaveSession finished;
    bool done = false;
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        if (!m_session.active) {
            return;
        }
        m_session.pending.erase(cameraName);
        if (ok) {
            m_session.result.savedFiles.push_back(finalPath);
            m_session.result.totalClips++;
            blog(LOG_INFO, "[SV MultiCam] Clip guardado: %s", finalPath.c_str());
        } else {
            m_session.result.errors.push_back(cameraName + ": " + error);
            blog(LOG_ERROR, "[SV MultiCam] Error con el clip de '%s': %s", cameraName.c_str(), error.c_str());
        }
        if (m_session.pending.empty()) {
            finished = m_session;
            m_session.active = false;
            done = true;
        }
    }

    if (done) {
        FinishSession(finished);
    }
}

void MultiCamManager::CheckSessionTimeout()
{
    SaveSession finished;
    bool done = false;
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        if (!m_session.active) {
            return;
        }
        if (os_gettime_ns() - m_session.startedNs < kSessionTimeoutNs) {
            return;
        }
        for (const auto &name : m_session.pending) {
            m_session.result.errors.push_back(name + ": OBS no confirmó el guardado a tiempo.");
        }
        m_session.pending.clear();
        finished = m_session;
        m_session.active = false;
        done = true;
    }

    if (done) {
        FinishSession(finished);
    }
}

void MultiCamManager::FinishSession(SaveSession &session)
{
    session.result.success = session.result.errors.empty() && session.result.totalClips > 0;
    blog(LOG_INFO, "[SV MultiCam] Repetición #%04u terminada: %d clips, %zu errores.", session.playId,
         session.result.totalClips, session.result.errors.size());

    SaveResultListener listener;
    {
        std::lock_guard<std::mutex> lock(m_listenerMutex);
        listener = m_listener;
    }
    if (listener) {
        listener(session.result);
    }
}

} // namespace SVReplay