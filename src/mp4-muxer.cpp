#include "mp4-muxer.hpp"
#include <util/platform.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <cstring>

namespace SVReplay {

namespace fs = std::filesystem;

MP4Muxer::MP4Muxer() = default;
MP4Muxer::~MP4Muxer() = default;

std::string MP4Muxer::SanitizeFilename(const std::string &name)
{
    std::string sanitized = name;
    const std::string illegalChars = "\\/:*?\"<>| ";
    for (char &c : sanitized) {
        if (illegalChars.find(c) != std::string::npos) {
            c = '_';
        }
    }
    return sanitized;
}

std::string MP4Muxer::BuildFilename(const MuxerOptions &options)
{
    std::ostringstream ss;
    ss << (options.filenamePrefix.empty() ? "replay" : options.filenamePrefix)
       << "_"
       << std::setw(4) << std::setfill('0') << options.playId
       << "_"
       << options.timestampStr
       << "_"
       << SanitizeFilename(options.sourceName)
       << ".mp4";
    return ss.str();
}

// Funciones auxiliares para escribir enteros en formato Big Endian (estándar MP4/ISOBMFF)
static void WriteU32(std::ostream &os, uint32_t val)
{
    uint8_t bytes[4] = {
        static_cast<uint8_t>((val >> 24) & 0xFF),
        static_cast<uint8_t>((val >> 16) & 0xFF),
        static_cast<uint8_t>((val >> 8) & 0xFF),
        static_cast<uint8_t>(val & 0xFF)
    };
    os.write(reinterpret_cast<const char*>(bytes), 4);
}

static void WriteU16(std::ostream &os, uint16_t val)
{
    uint8_t bytes[2] = {
        static_cast<uint8_t>((val >> 8) & 0xFF),
        static_cast<uint8_t>(val & 0xFF)
    };
    os.write(reinterpret_cast<const char*>(bytes), 2);
}

static void WriteFourCC(std::ostream &os, const char fourcc[5])
{
    os.write(fourcc, 4);
}

bool MP4Muxer::SaveClipsToMP4(
    const MuxerOptions &options,
    const std::vector<VideoFramePacket> &frames,
    std::string &outFinalPath,
    std::string &outErrorMessage)
{
    if (frames.empty()) {
        outErrorMessage = "No hay fotogramas en el búfer para el intervalo seleccionado.";
        return false;
    }

    try {
        // Asegurar que el directorio de destino existe
        fs::path dirPath = fs::u8path(options.outputDirectory);
        if (!fs::exists(dirPath)) {
            fs::create_directories(dirPath);
        }

        std::string filename = BuildFilename(options);
        fs::path fullPath = dirPath / fs::u8path(filename);
        outFinalPath = fullPath.u8string();

        std::ofstream outFile(fullPath, std::ios::binary);
        if (!outFile.is_open()) {
            outErrorMessage = "No se pudo abrir el archivo para escritura: " + outFinalPath;
            return false;
        }

        uint32_t width = frames.front().width;
        uint32_t height = frames.front().height;
        uint32_t frameCount = static_cast<uint32_t>(frames.size());
        uint32_t timeScale = 1000; // Milisegundos de resolución
        uint32_t frameDuration = (options.fps > 0) ? (timeScale / options.fps) : 16;
        uint32_t totalDuration = frameCount * frameDuration;

        // 1. Átomo ftyp (File Type Box)
        WriteU32(outFile, 32); // Tamaño del átomo ftyp (32 bytes)
        WriteFourCC(outFile, "ftyp");
        WriteFourCC(outFile, "isom"); // Major brand
        WriteU32(outFile, 0x00000200); // Minor version
        WriteFourCC(outFile, "isom"); // Compatible brands
        WriteFourCC(outFile, "iso2");
        WriteFourCC(outFile, "mp41");
        WriteFourCC(outFile, "avc1");

        // 2. Átomo mdat (Media Data Box)
        // Calculamos el tamaño total de los datos
        uint64_t totalMediaDataSize = 0;
        std::vector<uint32_t> sampleSizes;
        sampleSizes.reserve(frameCount);

        for (const auto &f : frames) {
            sampleSizes.push_back(static_cast<uint32_t>(f.data.size()));
            totalMediaDataSize += f.data.size();
        }

        uint64_t mdatHeaderSize = 8;
        uint32_t mdatOffset = 32; // Offset inicial de mdat en el archivo
        uint32_t firstSampleOffset = mdatOffset + static_cast<uint32_t>(mdatHeaderSize);

        WriteU32(outFile, static_cast<uint32_t>(totalMediaDataSize + mdatHeaderSize));
        WriteFourCC(outFile, "mdat");

        // Guardamos los offsets de cada muestra para el átomo stco
        std::vector<uint32_t> sampleOffsets;
        sampleOffsets.reserve(frameCount);
        uint32_t currentOffset = firstSampleOffset;

        for (const auto &f : frames) {
            sampleOffsets.push_back(currentOffset);
            outFile.write(reinterpret_cast<const char*>(f.data.data()), f.data.size());
            currentOffset += static_cast<uint32_t>(f.data.size());
        }

        // 3. Átomo moov (Movie Box)
        // Construimos el átomo moov en un búfer en memoria para calcular su longitud exacta
        std::ostringstream moovStream(std::ios::binary);

        // mvhd (Movie Header)
        WriteU32(moovStream, 108);
        WriteFourCC(moovStream, "mvhd");
        WriteU32(moovStream, 0); // Versión 0, flags 0
        WriteU32(moovStream, 0); // Creation time
        WriteU32(moovStream, 0); // Modification time
        WriteU32(moovStream, timeScale);
        WriteU32(moovStream, totalDuration);
        WriteU32(moovStream, 0x00010000); // Tasa normal 1.0
        WriteU16(moovStream, 0x0100);     // Volumen completo
        WriteU16(moovStream, 0);          // Reservado
        WriteU32(moovStream, 0);
        WriteU32(moovStream, 0);
        // Matriz de identidad de video 3x3
        WriteU32(moovStream, 0x00010000); WriteU32(moovStream, 0); WriteU32(moovStream, 0);
        WriteU32(moovStream, 0); WriteU32(moovStream, 0x00010000); WriteU32(moovStream, 0);
        WriteU32(moovStream, 0); WriteU32(moovStream, 0); WriteU32(moovStream, 0x40000000);
        // Datos predefinidos
        for (int i = 0; i < 6; ++i) WriteU32(moovStream, 0);
        WriteU32(moovStream, 2); // Next track ID

        // trak (Track Box)
        std::ostringstream trakStream(std::ios::binary);

        // tkhd (Track Header)
        WriteU32(trakStream, 92);
        WriteFourCC(trakStream, "tkhd");
        WriteU32(trakStream, 0x00000007); // Flags: enabled, in movie, in preview
        WriteU32(trakStream, 0);
        WriteU32(trakStream, 0);
        WriteU32(trakStream, 1); // Track ID
        WriteU32(trakStream, 0);
        WriteU32(trakStream, totalDuration);
        WriteU32(trakStream, 0);
        WriteU32(trakStream, 0);
        WriteU16(trakStream, 0); // Layer
        WriteU16(trakStream, 0); // Alternate group
        WriteU16(trakStream, 0); // Volume (0 para video)
        WriteU16(trakStream, 0);
        // Matriz de video
        WriteU32(trakStream, 0x00010000); WriteU32(trakStream, 0); WriteU32(trakStream, 0);
        WriteU32(trakStream, 0); WriteU32(trakStream, 0x00010000); WriteU32(trakStream, 0);
        WriteU32(trakStream, 0); WriteU32(trakStream, 0); WriteU32(trakStream, 0x40000000);
        WriteU32(trakStream, width << 16);
        WriteU32(trakStream, height << 16);

        // mdia (Media Box)
        std::ostringstream mdiaStream(std::ios::binary);
        // mdhd (Media Header)
        WriteU32(mdiaStream, 32);
        WriteFourCC(mdiaStream, "mdhd");
        WriteU32(mdiaStream, 0);
        WriteU32(mdiaStream, 0);
        WriteU32(mdiaStream, 0);
        WriteU32(mdiaStream, timeScale);
        WriteU32(mdiaStream, totalDuration);
        WriteU16(mdiaStream, 0x55C4); // Idioma 'und' (indeterminado)
        WriteU16(mdiaStream, 0);

        // hdlr (Handler Reference)
        WriteU32(mdiaStream, 33 + 12);
        WriteFourCC(mdiaStream, "hdlr");
        WriteU32(mdiaStream, 0);
        WriteU32(mdiaStream, 0);
        WriteFourCC(mdiaStream, "vide");
        WriteU32(mdiaStream, 0);
        WriteU32(mdiaStream, 0);
        WriteU32(mdiaStream, 0);
        mdiaStream.write("VideoHandler\0", 13);

        // minf (Media Information Box)
        std::ostringstream minfStream(std::ios::binary);
        // vmhd (Video Media Header)
        WriteU32(minfStream, 20);
        WriteFourCC(minfStream, "vmhd");
        WriteU32(minfStream, 0x00000001);
        WriteU16(minfStream, 0);
        WriteU16(minfStream, 0);
        WriteU16(minfStream, 0);
        WriteU16(minfStream, 0);

        // dinf & dref (Data Information)
        WriteU32(minfStream, 36);
        WriteFourCC(minfStream, "dinf");
        WriteU32(minfStream, 28);
        WriteFourCC(minfStream, "dref");
        WriteU32(minfStream, 0);
        WriteU32(minfStream, 1); // 1 entrada
        WriteU32(minfStream, 12);
        WriteFourCC(minfStream, "url ");
        WriteU32(minfStream, 0x00000001); // Mismo archivo

        // stbl (Sample Table Box)
        std::ostringstream stblStream(std::ios::binary);
        // stsd (Sample Description)
        WriteU32(stblStream, 16 + 86);
        WriteFourCC(stblStream, "stsd");
        WriteU32(stblStream, 0);
        WriteU32(stblStream, 1); // 1 entrada
        // VisualSampleEntry (avc1)
        WriteU32(stblStream, 86);
        WriteFourCC(stblStream, "avc1");
        WriteU32(stblStream, 0);
        WriteU16(stblStream, 0);
        WriteU16(stblStream, 0);
        WriteU32(stblStream, 0);
        WriteU32(stblStream, 0);
        WriteU32(stblStream, 0);
        WriteU16(stblStream, static_cast<uint16_t>(width));
        WriteU16(stblStream, static_cast<uint16_t>(height));
        WriteU32(stblStream, 0x00480000); // 72 dpi
        WriteU32(stblStream, 0x00480000); // 72 dpi
        WriteU32(stblStream, 0);
        WriteU16(stblStream, 1); // Frame count
        char compressorName[32] = {0};
        compressorName[0] = 12;
        std::memcpy(compressorName + 1, "SV MultiCam", 11);
        stblStream.write(compressorName, 32);
        WriteU16(stblStream, 0x0018); // Profundidad 24 bits
        WriteU16(stblStream, 0xFFFF);

        // stts (Time-to-Sample)
        WriteU32(stblStream, 24);
        WriteFourCC(stblStream, "stts");
        WriteU32(stblStream, 0);
        WriteU32(stblStream, 1); // 1 bloque uniforme
        WriteU32(stblStream, frameCount);
        WriteU32(stblStream, frameDuration);

        // stss (Sync Sample Table - Keyframes)
        WriteU32(stblStream, 20);
        WriteFourCC(stblStream, "stss");
        WriteU32(stblStream, 0);
        WriteU32(stblStream, 1); // Primer fotograma es keyframe
        WriteU32(stblStream, 1);

        // stsc (Sample to Chunk)
        WriteU32(stblStream, 28);
        WriteFourCC(stblStream, "stsc");
        WriteU32(stblStream, 0);
        WriteU32(stblStream, 1);
        WriteU32(stblStream, 1); // Primer chunk
        WriteU32(stblStream, 1); // 1 muestra por chunk
        WriteU32(stblStream, 1); // Sample description index 1

        // stsz (Sample Sizes)
        WriteU32(stblStream, 20 + frameCount * 4);
        WriteFourCC(stblStream, "stsz");
        WriteU32(stblStream, 0);
        WriteU32(stblStream, 0); // Tamaños variables
        WriteU32(stblStream, frameCount);
        for (uint32_t s : sampleSizes) {
            WriteU32(stblStream, s);
        }

        // stco (Chunk Offsets)
        WriteU32(stblStream, 16 + frameCount * 4);
        WriteFourCC(stblStream, "stco");
        WriteU32(stblStream, 0);
        WriteU32(stblStream, frameCount);
        for (uint32_t off : sampleOffsets) {
            WriteU32(stblStream, off);
        }

        std::string stblData = stblStream.str();
        WriteU32(minfStream, static_cast<uint32_t>(stblData.size() + 8));
        WriteFourCC(minfStream, "stbl");
        minfStream.write(stblData.data(), stblData.size());

        std::string minfData = minfStream.str();
        WriteU32(mdiaStream, static_cast<uint32_t>(minfData.size() + 8));
        WriteFourCC(mdiaStream, "minf");
        mdiaStream.write(minfData.data(), minfData.size());

        std::string mdiaData = mdiaStream.str();
        WriteU32(trakStream, static_cast<uint32_t>(mdiaData.size() + 8));
        WriteFourCC(trakStream, "mdia");
        trakStream.write(mdiaData.data(), mdiaData.size());

        std::string trakData = trakStream.str();
        WriteU32(moovStream, static_cast<uint32_t>(trakData.size() + 8));
        WriteFourCC(moovStream, "trak");
        moovStream.write(trakData.data(), trakData.size());

        std::string moovData = moovStream.str();
        WriteU32(outFile, static_cast<uint32_t>(moovData.size() + 8));
        WriteFourCC(outFile, "moov");
        outFile.write(moovData.data(), moovData.size());

        outFile.flush();
        outFile.close();

        return true;
    } catch (const std::exception &ex) {
        outErrorMessage = std::string("Error al generar MP4: ") + ex.what();
        return false;
    }
}

} // namespace SVReplay
