# Arquitectura de Loki

Loki es un editor de video no lineal (NLE) para PC, inspirado en el flujo de trabajo de Adobe Premiere Pro.
Este documento describe la arquitectura objetivo. Las secciones marcadas con **(decisión pendiente)** se
confirmarán antes de la fase indicada.

## 1. Principios

1. **Tiempo racional.** Toda posición/duración de edición es `loki::Rational` (numerador/denominador `int64_t`,
   normalizado, estilo `AVRational`). Prohibido `double`/`float` para tiempos de edición. `double` solo se permite
   en la frontera de presentación (píxeles de la timeline, medidores) y nunca vuelve al modelo.
2. **El modelo es la fuente de verdad** y no depende de la UI. La UI observa el modelo y lo modifica solo vía
   `Command`.
3. **Undo/redo ilimitado** (desde la Fase 2): toda mutación del modelo es un `Command` reversible.
4. **Hilos separados:** UI, decodificación (pool + caché LRU), render GPU, audio. **El reloj de audio es el maestro.**
5. **Mismo motor para preview y export.** El export usa el mismo grafo de render/mezcla, solo cambia el sumidero
   (pantalla vs. encoder) y el reloj (tiempo real vs. "lo más rápido posible").
6. **Nunca bloquear el hilo de UI** con I/O o decodificación (presupuesto: < 50 ms por evento).
7. **RAII estricto:** sin `new`/`delete` crudos, sin globales mutables. Recursos FFmpeg en `std::unique_ptr` con
   deleters dedicados (`AVFormatContextPtr`, `AVCodecContextPtr`, `AVFramePtr`, `AVPacketPtr`, ...).
8. Código, identificadores y comentarios en **inglés**; documentación de usuario en **español**.

## 2. Módulos

Cada carpeta de `src/` es una biblioteca estática CMake (`loki_<módulo>`). El ejecutable `loki` solo contiene
`app/main.cpp` y enlaza las bibliotecas. Las dependencias permitidas son **acíclicas** y se fuerzan en CMake.

| Módulo      | Responsabilidad | Depende de | Dependencias externas |
|-------------|-----------------|------------|-----------------------|
| `util`      | Logging, `Rational`, timecode, hilos/colas, helpers | — | spdlog |
| `model`     | Project, Sequence, Track, Clip, MediaItem, Effect, Keyframe, serialización | util | nlohmann/json |
| `commands`  | `Command`, `UndoStack`, comandos concretos de edición | model, util | — |
| `media`     | Probe, demux/decode FFmpeg, hwaccel, caché de fotogramas, thumbnails, waveforms, proxies | util, model (solo tipos de MediaItem) | FFmpeg |
| `render`    | Conversión de color, renderer GL; luego interfaz `RenderBackend`, compositor y efectos | media (tipos de frame) | Qt GUI/OpenGL (OpenGL 3.3) |
| `audio`     | Salida miniaudio (`AudioDevice`); mezclador en Fase 8 | util | miniaudio |
| `playback`  | Reloj maestro, motor de reproducción, sincronía A/V, scrubbing, J/K/L | audio, media, model, util | — |
| `export`    | Pipeline de exportación (mismo motor), presets, encoders | playback (grafo), render, audio, media | FFmpeg |
| `ui`        | Paneles: Project, SourceMonitor, ProgramMonitor, Timeline, EffectControls, AudioMeters | todos los anteriores | Qt Widgets |
| `app`       | `main`, ventana principal (dock manager), tema, workspaces, acciones y atajos | ui | Qt Widgets, Qt ADS |

Reglas:

- `model`, `commands`, `util` **no enlazan Qt Widgets**. Objetivo: tampoco Qt Core (ver §4), para que sean
  testeables y utilizables desde hilos de export sin event loop.
- `ui` nunca muta el modelo directamente: construye un `Command` y lo empuja al `UndoStack`.
- `media`, `render`, `audio` y `playback` trabajan sobre **snapshots inmutables** del modelo (ver §5), nunca
  sobre los objetos vivos que edita la UI.

```mermaid
graph TD
    app --> ui
    ui --> playback
    ui --> commands
    ui --> media
    export --> playback
    export --> render
    export --> audio
    export --> media
    ui --> render
    playback --> audio
    playback --> media
    render --> media
    commands --> model
    media --> model
    playback --> model
    model --> util
    media --> util
    render --> util
    audio --> util
    commands --> util

    subgraph ext [Dependencias externas]
        Qt[Qt 6 Widgets + ADS]
        FF[FFmpeg LGPL]
        GL[OpenGL 3.3 core]
        MA[miniaudio]
        JS[nlohmann/json]
        SP[spdlog]
    end
    ui -.-> Qt
    media -.-> FF
    export -.-> FF
    render -.-> GL
    audio -.-> MA
    model -.-> JS
    util -.-> SP
```

## 3. Tiempo

- `loki::Rational { int64_t num; int64_t den; }` siempre normalizado (`den > 0`, `gcd == 1`), aritmética con
  detección de overflow (intermedios de 128 bits portables: MSVC no tiene `__int128`, usar helper propio),
  comparación exacta por multiplicación cruzada.
- Conversión a/desde `AVRational` en la frontera de `media`.
- La secuencia tiene `frameRate` (p. ej. `30000/1001`) y `sampleRate` de audio. Las ediciones de video se
  cuantizan a frame de secuencia; las de audio pueden ser a nivel de muestra.
- Timecode (`HH:MM:SS:FF`, drop-frame para 29.97/59.94) solo para presentación/entrada.

## 4. Modelo y comandos (decidido en Fase 2: D1–D4)

- **D1 – Modelo en C++ puro.** `src/model` no depende de Qt. `Project` notifica cada cambio con un
  `ChangeEvent` (tipo, id, contenedor) a los `ProjectObserver` registrados. El adaptador
  `ui::ProjectController : QObject` convierte esos eventos en señales Qt, es dueño del `Project` y del
  `UndoStack`, expone las acciones Undo/Redo y carga/guarda en un hilo de trabajo (`QtConcurrent`).
- **D2 – Solo los Commands mutan.** La API pública de `Project` es de solo lectura (referencias `const`). La
  única escritura es `ProjectMutator`, cuyo constructor es privado y solo accesible a `commands::Command`
  (vía `Command::mutate()`) y a `ProjectBuilder`. `ProjectBuilder` solo crea proyectos nuevos (cargar, plantillas,
  tests). Toda operación del mutator valida invariantes y, si falla, lanza `ModelError` sin cambiar nada.
- **D3 – Formato `.loki`.** JSON con `format`/`version`, tiempos `[num, den]`, IDs UUID v4 y ruta absoluta +
  relativa de los medios. Detalle en `docs/PROJECT_FORMAT.md`.
- **D4 – Sin solapes.** Los clips de una pista están ordenados por `start` y no se solapan. Insertar/sobrescribir
  (Fase 5) resolverán los solapes con macros de comandos.

```
Project
 ├─ MediaItem[]        (id, nombre, ruta, proxy, bin, MediaInfo)
 ├─ Bin[] (árbol)      (id, nombre, parent)
 └─ Sequence[]
     ├─ settings       (resolución, frameRate, sampleRate)
     ├─ videoTracks[] / audioTracks[]  (id, TrackState: nombre, locked, hidden, muted)
     │    └─ Clip[]    (id, media, start, sourceIn, duration, enabled, link)
     │         └─ Effect[] → Parameter[] → Keyframe[] (tiempo en el *source*, valor, Hold/Linear/Bezier)
     └─ (Transition[] en Fase 9, con migración de formato)
```

- Los tiempos de los keyframes están en el tiempo del *source* del clip, así siguen al contenido al mover, recortar
  o dividir.
- `Command`: `redo()`/`undo()`/`text()`; la primera ejecución valida y tiene garantía fuerte (si falla, no cambia
  nada). `MacroCommand` agrupa y revierte a los hijos ya aplicados si uno falla.
- `UndoStack`: ilimitado, macros anidadas (`MacroGuard` RAII), estado "limpio" (guardado) y `Point` +
  `markCleanAt()` para guardados asíncronos.
- **Fusión por gesto:** comandos con el mismo `GestureId` distinto de cero (un arrastre, un slider) se funden en un
  solo paso de undo; con `GestureId` 0 nunca se funden. La UI pide ids con `ProjectController::newGestureId()`.
- Los clips de pistas bloqueadas no se pueden editar. Se comprueba solo en la primera ejecución, así undo/redo
  siempre reproducen fielmente.
- Copiar un `Project` copia los datos pero no los observadores: sirve como snapshot barato para otros hilos.

## 5. Concurrencia y flujo de datos

| Hilo | Trabajo | Comunicación |
|------|---------|--------------|
| UI (main) | Eventos, pintado de widgets, aplicación de Commands | Publica snapshot nuevo tras cada Command |
| Decode pool (N = núcleos − 2) | Demux/decode, hwaccel, escalado, thumbnails, waveforms, proxies | Colas de peticiones con prioridad; resultados a caché LRU |
| Render | Contexto GL propio (compartido con QOpenGLWidget), composición, efectos | Pide frames a la caché; entrega textura al monitor |
| Audio (callback miniaudio) | Mezcla en tiempo real, **reloj maestro** | Ring buffer lock-free alimentado por el mezclador; sin locks ni alloc en el callback |

- **Snapshot del modelo:** tras cada Command, la UI publica un `std::shared_ptr<const SequenceSnapshot>`
  (copia estructural barata). Los hilos de playback/render/export leen el puntero atómicamente. Sin locks sobre el
  modelo vivo.
- **Sincronía A/V:** el `MasterClock` avanza con las muestras efectivamente consumidas por el dispositivo de audio
  (corrigiendo la latencia reportada). El render presenta el frame cuyo PTS corresponde al reloj; si va tarde, lo
  descarta (cuenta "dropped") en vez de desincronizar. Sin audio, reloj de sistema monotónico.
- **Scrubbing:** peticiones de "último gana" (se cancelan peticiones obsoletas), seek al keyframe más cercano +
  decode hacia delante, prioridad máxima en el pool; objetivo < 100 ms.

```mermaid
sequenceDiagram
    participant UI as Hilo UI
    participant PB as Playback
    participant DP as Decode pool
    participant C as Caché LRU
    participant R as Render GPU
    participant A as Audio (miniaudio)

    UI->>UI: Command.redo() y publica snapshot
    UI->>PB: play()
    PB->>DP: prefetch(frames t..t+N)
    DP->>C: frames decodificados
    A->>PB: reloj maestro (muestras consumidas)
    PB->>R: render(t_reloj, snapshot)
    R->>C: get(frame)
    R-->>UI: textura lista (QOpenGLWidget::update)
    PB->>A: buffers de mezcla
```

## 6. Render

- Interfaz `RenderBackend` (crear texturas, subir planos YUV/RGBA, ejecutar pasada de efecto, leer a CPU para
  export). Implementación MVP: `GLRenderBackend` (OpenGL 3.3 core). Migración futura: QRhi / Vulkan / D3D11 sin
  tocar compositor ni efectos (los efectos declaran parámetros + shader por backend).
- Espacio de trabajo: RGBA 16F lineal; conversión YUV→RGB en shader (BT.601/709 según metadatos, rango
  limitado/completo).
- Orden por pista: decode → conversión de color → efectos de clip (Movimiento, Opacidad, color, LUT) →
  transición → mezcla con modo de fusión sobre la pista inferior.
- Calidad de reproducción (completa, 1/2, 1/4) = resolución del framebuffer de composición.

## 7. Media (Fase 3)

- `media::probeMedia` (libavformat) lee resolución, fps, duración, códecs, audio y si es imagen fija. Se ejecuta en
  el pool, nunca en la UI.
- `media::VideoDecoder`: un decodificador por hilo y precisión de fotograma.
  - `frameAt(t)` devuelve el último fotograma con `pts <= t`. Si `t` está a menos de 2 s hacia delante sigue
    decodificando; si no, busca el keyframe anterior y decodifica hasta `t`.
  - `framesUpTo(t)` decodifica un GOP completo; sirve para reproducir hacia atrás.
  - Aceleración por hardware en orden D3D11VA, CUDA/NVDEC y DXVA2. El primer fotograma se prueba y, si falla, se
    vuelve a software. Los fotogramas de hardware se copian a RAM (D6).
  - Salida en YUV420P o NV12 (la GPU hace la conversión) o RGBA (miniaturas), con escalado opcional según la calidad
    de reproducción.
  - Tiempos relativos al inicio del medio (0 = primer fotograma), igual que `Clip::sourceIn`.
- `media::AudioDecoder` convierte a float32 intercalado a una frecuencia fija (swresample). El seek tiene precisión
  de muestra: descarta o rellena hasta el instante pedido.
- `media::FrameCache`: LRU con límite en bytes, thread-safe. Clave = (fuente, calidad, índice de fotograma).
- `media::MediaCache`: caché en disco (`MediaCache/<tipo>/<clave>.<ext>`). La clave es un hash FNV-1a de ruta,
  tamaño y fecha de modificación. Guarda miniaturas JPG y waveforms `.lkwf` (formato binario propio con
  picos min/max en int16). Se configura en Preferencias; en la máquina de desarrollo está en `F:\LokiCache`.
- `util::TaskPool`: pool con prioridades (Interactive > High > Normal > Background) y cancelación cooperativa. Por
  defecto usa núcleos − 2 hilos.
- UI: `ui::MediaServices` agrupa pool, miniaturas, waveforms e importación (probe en paralelo y un único
  `MacroCommand` de undo). `ui::ProjectPanel` / `ProjectItemModel` muestran bins y medios.

## 7b. Reproducción (Fase 4)

- `audio::AudioDevice`: miniaudio en la app (48 kHz estéreo, periodo de 10 ms) y `ManualAudioDevice` en los tests.
  El callback solo lee un `util::SpscRing` sin locks.
- `playback::PlaybackClock`:
  - A ×1 con audio: tiempo = ancla + (muestras consumidas por el dispositivo − latencia) / frecuencia.
  - Con J/K/L a otras velocidades, hacia atrás o sin audio: reloj monotónico × velocidad (`TimeSource` inyectable).
- `playback::PlaybackEngine`:
  - Hilo de medios propio. La UI solo envía peticiones (la última gana) y consulta `currentFrame()`/`position()`
    cada 10 ms.
  - Hacia delante mantiene ~8 fotogramas decodificados por adelantado. Si va más de 0,5 s por detrás, salta al
    reloj en vez de decodificar todo lo pendiente.
  - Hacia atrás usa `framesUpTo` por GOP y la caché.
  - El fotograma mostrado lo elige el reloj; los que llegan tarde se descartan y se cuentan en `PlaybackStats`.
  - Al llegar al final (o al inicio en reversa) se pausa.
- `render::GlFrameRenderer`: OpenGL 3.3 (shaders `#version 330 core`, contexto de compatibilidad). Sube los planos como texturas R8/RG8/RGBA8 y el shader convierte
  YUV a RGB con la matriz BT.601/709 y el rango limitado/completo de `render::yuvToRgb`. Esa misma función es la
  referencia en CPU que usan los tests.
- `ui::VideoView`: no es un `QOpenGLWidget`. Es una ventana nativa `QOpenGLWindow` incrustada con
  `createWindowContainer`. Con un `QOpenGLWidget`, toda la ventana principal se veía en blanco en la Intel
  HD 620 (no se confirmó la causa). Con la ventana nativa, el resto de la UI sigue dibujándose por la ruta
  normal (raster).
- `ui::SourceMonitorPanel`:
  - `VideoView` y `ScrubBar`.
  - Timecode con drop-frame cuando corresponde.
  - Calidad completa, 1/2 o 1/4.
  - Teclas: Espacio, J/K/L, ←/→ (Shift = 5 fotogramas), I/O, Shift+I/O, Home/End y Ctrl+Shift+X.
  - Las marcas In/Out se guardan en el `MediaItem` como comandos con undo (D9). Out es exclusivo.

## 8. Export

- Mismo grafo que playback, reloj "offline" (frame a frame), sumidero = encoder.
- Encoders H.264: `h264_nvenc` → `h264_qsv` → `h264_amf` → `libopenh264` (fallback software). AAC: encoder
  nativo `aac` de FFmpeg. Sin GPL (ver `docs/LICENSING.md`, a crear en Fase 1).
- Corre en hilo propio, con progreso y cancelación cooperativa.

## 9. Persistencia y robustez

- Proyecto `.loki` (JSON, `version`), escritura atómica (archivo temporal + rename).
- Autoguardado configurable + archivo de recuperación; al arrancar, si existe recuperación más nueva que el
  proyecto, se ofrece restaurarla.
- Workspaces: `QMainWindow::saveState` / ADS `saveState` por workspace (Edición, Color, Audio).
