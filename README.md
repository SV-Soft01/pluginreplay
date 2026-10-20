# SV MultiCam Replay - Plugin Nativo para OBS Studio

Plugin nativo en C++ para **OBS Studio 32.1.2 (Windows 64 bits)** diseñado para transmisiones deportivas profesionales en vivo. Permite crear búferes de repetición circular independientes para cualquier número de fuentes de video o escenas seleccionadas, y guardarlas simultáneamente con un único atajo de teclado en archivos MP4 perfectamente sincronizados.

---

## Características Principales

- **Búferes Circulares Independientes:** Cada cámara o escena seleccionada cuenta con su propio búfer en memoria que conserva continuamente los últimos N segundos (5s, 10s, 15s, 30s o personalizado).
- **Captura Universal de Fuentes y Escenas:** Compatible con tarjetas de captura (Cam Link, Elgato, Blackmagic), cámaras web USB (UVC), fuentes NDI, reproductores de video y **escenas completas**.
- **Permanencia Activa Fuera de Programa:** Mantiene las fuentes capturando y actualizando su búfer incluso cuando no están en la escena activa de Programa.
- **Atajo Único Global:** Una sola pulsación (ej. tecla `1` o `F8`) guarda simultáneamente los clips de todas las cámaras activas.
- **Sincronización Multicámara:** Todos los clips comparten la misma ventana temporal exacta $[T - N, T]$ indexada con el reloj maestro de alta resolución de OBS (`os_gettime_ns()`).
- **Nomenclatura Organizada:** Los archivos se guardan con identificador de jugada correlativo y marca temporal:
  - `replay_0001_20261020_011530_Cam1.mp4`
  - `replay_0001_20261020_011530_Cam2.mp4`
  - `replay_0001_20261020_011530_Cam3.mp4`
- **Operación Asíncrona sin Congelamientos:** La codificación y multiplexación a MP4 se realiza en un hilo desacoplado; la transmisión en vivo y la interfaz de OBS permanecen al 100% fluidas a 60 fps.
- **Panel Integrado en OBS (Dock Qt 6):** Interfaz limpia y acoplable dentro de OBS con monitor de estado por cámara, indicador de memoria RAM estimada y botón de prueba.
- **Persistencia Completa:** Las cámaras seleccionadas, la duración, la carpeta y el prefijo se guardan automáticamente entre sesiones.

---

## Requisitos de Compilación

1. **Sistema Operativo:** Windows 10 / 11 (64 bits).
2. **Compilador:** Visual Studio 2022 (MSVC v143 con soporte C++17/C++20).
3. **CMake:** Versión 3.22 o superior.
4. **Qt 6:** Qt 6.5+ para MSVC 2022 x64 (utilizado por OBS Studio 30/31/32).
5. **OBS Studio SDK:** Cabeceras y bibliotecas de `libobs` y `obs-frontend-api` de OBS Studio 32.1.2.

---

## Instrucciones de Compilación (CMake + Visual Studio 2022)

### 1. Clonar o Ubicar el Código Fuente
Abre una terminal de PowerShell en la carpeta del plugin:
```powershell
cd c:\Users\Dellg\OneDrive\Desktop\pluginobs
```

### 2. Generar el Proyecto con CMake
Ejecuta CMake especificando las rutas a tu instalación de OBS Studio y Qt 6:
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DQt6_DIR="C:/Qt/6.6.0/msvc2022_64/lib/cmake/Qt6" `
  -DOBS_DIR="C:/obs-studio/build_x64"
```

### 3. Compilar en Modo Release
```powershell
cmake --build build --config Release
```

El archivo binario compilado será:
`build/Release/sv-multicam-replay.dll`

---

## Instalación en OBS Studio 32.1.2

1. Cierra OBS Studio si está en ejecución.
2. Copia el archivo `sv-multicam-replay.dll` en la carpeta de plugins de OBS:
   ```text
   C:\Program Files\obs-studio\obs-plugins\64bit\sv-multicam-replay.dll
   ```
3. Si creas traducciones o archivos adicionales de datos, cópialos en:
   ```text
   C:\Program Files\obs-studio\data\obs-plugins\sv-multicam-replay\
   ```
4. Abre OBS Studio. El panel **SV MultiCam Replay** aparecerá automáticamente acoplado en tu interfaz.

---

## Guía de Uso para Transmisiones Deportivas

### 1. Selección de Cámaras
- En el panel **SV MultiCam Replay**, presiona **"Escanear Fuentes de OBS"**.
- Marca con una casilla las cámaras que intervendrán en el sistema multicámara (ej. `Cámara Principal`, `Cámara Lateral`, `Cámara de Gol`).

### 2. Configurar Duración y Carpeta
- Ajusta el control deslizante a la duración deseada (ej. `10 segundos`).
- Pulsa **"Examinar..."** y selecciona tu disco de alta velocidad (preferiblemente SSD / NVMe).
- Configura el prefijo de archivo (ej. `final_copa`).

### 3. Asignar el Atajo de Teclado
- En OBS Studio, ve a **Ajustes -> Atajos**.
- Busca **"SV MultiCam: Guardar Repetición Multicámara"**.
- Asigna tu tecla de preferencia (por ejemplo, `1`, `F8` o un pedal/botón USB).
- Pulsa **Aceptar**.

### 4. Guardar Repeticiones en Vivo
- Durante el partido, cuando ocurra una jugada relevante, presiona tu tecla de atajo asignada (o el botón verde **"GUARDAR REPETICIÓN AHORA"** en el panel).
- El plugin generará de inmediato un archivo MP4 individual por cada cámara seleccionada, todos indexados con el mismo número de jugada y compartiendo el mismo intervalo temporal.
