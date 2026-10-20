#pragma once

#include "source-buffer.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

namespace SVReplay {

struct ReplaySaveResult {
    bool success = false;
    uint32_t playId = 0;
    int totalClips = 0;
    std::vector<std::string> savedFiles;
    std::vector<std::string> errors;
};

using SaveCompletionCallback = std::function<void(const ReplaySaveResult &result)>;

class MultiCamManager {
public:
    static MultiCamManager& Instance();

    // Ciclo de vida general
    void Initialize();
    void Shutdown();

    // Gestión de fuentes seleccionadas
    void SetSourceActive(const std::string &sourceName, bool active);
    bool IsSourceActive(const std::string &sourceName) const;
    std::vector<std::string> GetActiveSources() const;

    // Configuración global
    void SetBufferDuration(int seconds);
    int GetBufferDuration() const { return m_bufferDuration; }

    void SetOutputDirectory(const std::string &dirPath);
    std::string GetOutputDirectory() const;

    void SetFilePrefix(const std::string &prefix);
    std::string GetFilePrefix() const;

    // Estadísticas y monitoreo
    size_t GetTotalEstimatedMemoryBytes() const;
    std::string GetSourceStatusString(const std::string &sourceName) const;
    size_t GetSourceBufferedFrames(const std::string &sourceName) const;
    double GetSourceBufferedSeconds(const std::string &sourceName) const;

    // Disparador de repetición (llamado por atajo de teclado o botón UI)
    bool TriggerSaveReplay(SaveCompletionCallback callback = nullptr);
    bool IsSaving() const { return m_isSaving.load(); }

    // Persistencia de configuración
    void LoadConfiguration();
    void SaveConfiguration();

    // Hook de renderizado de OBS (llamado internamente por el callback de gráficos)
    void OnMainRenderTick();

private:
    MultiCamManager();
    ~MultiCamManager();
    MultiCamManager(const MultiCamManager&) = delete;
    MultiCamManager& operator=(const MultiCamManager&) = delete;

    std::string GenerateTimestampString() const;

    mutable std::mutex m_managerMutex;
    std::unordered_map<std::string, std::unique_ptr<SourceBuffer>> m_sources;

    int m_bufferDuration = 10;
    std::string m_outputDirectory;
    std::string m_filePrefix = "replay";
    std::atomic<uint32_t> m_playCounter{1};

    std::atomic<bool> m_isSaving{false};
    std::atomic<bool> m_isInitialized{false};
    bool m_renderCallbackRegistered = false;
};

} // namespace SVReplay
