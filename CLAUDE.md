# CLAUDE.md — Loki (NLE estilo Premiere)

Memoria del proyecto entre sesiones. Actualizar al final de cada fase.

## Resumen
Editor de video no lineal para PC (Windows 10/11 x64 primero, código portable). Inspirado en la UI y flujo de
Premiere Pro. Se construye por fases; al terminar cada fase: compilar, tests verdes, commit (Conventional
Commits), actualizar este archivo, resumir y **esperar confirmación del usuario** antes de la siguiente fase.

Docs: `docs/ARCHITECTURE.md`, `docs/LICENSING.md`, `docs/BUILD.md`, `docs/PROJECT_FORMAT.md`.

## Stack
- C++20, CMake ≥ 3.25 + `CMakePresets.json`, vcpkg modo manifest (`vcpkg.json`, baseline fija).
- Qt 6.8.3 Widgets **precompilado** (aqtinstall local / install-qt-action en CI), encontrado vía `LOKI_QT_DIR`.
- Qt Advanced Docking System v5.1.1 vía `FetchContent` (DLL, LGPL-2.1).
- OpenGL 3.3 core vía `QOpenGLWidget` (Fase 6).
- nlohmann/json (proyecto), spdlog, GoogleTest, Qt Concurrent/Test/OpenGL/OpenGLWidgets.
- FFmpeg 8 vía vcpkg (features: avcodec avformat swresample swscale nvcodec qsv amf openh264 zlib) como
  target `loki::ffmpeg`; miniaudio header-only como `loki::miniaudio` (impl en audio/miniaudio_device.cpp).
- **Licencia: solo LGPL.** Prohibido enlazar libx264/libx265 u otro GPL sin preguntar. H.264: NVENC/QSV/AMF,
  fallback OpenH264.

## Comandos
```
. .\scripts\dev-env.ps1                    # entorno MSVC + Qt en PATH (por terminal)
cmake --preset windows-msvc-debug          # también: windows-msvc-release, windows-msvc-asan
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
.\build\windows-msvc-debug\bin\loki.exe
```
Binarios y DLLs (ADS, vcpkg) salen en `build/<preset>/bin`. Qt DLLs vía PATH (dev-env.ps1); los tests
reciben PATH de Qt y `QT_QPA_PLATFORM=offscreen` por propiedades de ctest.

## Convenciones
- Código/identificadores/comentarios en inglés; docs para el usuario en español.
- Namespace `loki::<módulo>`. Archivos `snake_case.h/.cpp`, tipos `PascalCase`, funciones/variables
  `camelCase`, miembros `m_` prefijo, constantes `kName`.
- Tiempo de edición SIEMPRE `loki::Rational`, nunca `double`.
- Toda mutación del modelo vía `Command` (undo/redo). `Project` es solo lectura; escribir = `ProjectMutator`
  (solo accesible desde `Command::mutate()` y `ProjectBuilder`). UI ejecuta vía `ProjectController::execute()`.
- Arrastres/sliders: pedir `ProjectController::newGestureId()` y pasarlo a MoveClip/trimClip/setParameterValue
  para fundir en un solo undo. GestureId 0 = nunca fundir.
- Ediciones de timeline: construir con `commands::EditPlan` (copia de pistas → diff → un `ReplaceClipsCommand`);
  no encadenar comandos sueltos. Drags del timeline: sombra + commit al soltar.
- Keyframes en tiempo del *source* del clip. IDs = UUID (`model::Id<Tag>` tipado).
- Atajos: nunca `QShortcut`/`keyPressEvent` sueltos; registrar con `ShortcutManager::create/bind` (id estable
  `<panel>.<acción>`, contexto de panel o "Application", tecla por defecto Premiere). Sin conflictos por contexto
  (hay test que lo verifica). Los tests Qt usan settings aislados (org "LokiTests").
- Cambio de esquema `.loki`: subir `kProjectFormatVersion` + migración + golden en `tests/data/`.
- RAII; sin `new`/`delete` crudos; sin globales mutables; FFmpeg en `unique_ptr` con deleter.
  Excepción: `new` de QObject/QWidget con padre Qt (el padre es dueño).
- Nada de I/O ni decode en el hilo de UI.
- Cada módulo de `src/` = biblioteca estática `loki_<módulo>` (alias `loki::<módulo>`), aplica
  `loki_set_warnings()` (/W4 /WX). Dependencias acíclicas (ARCHITECTURE.md §2).
- Tests: GoogleTest en `tests/<módulo>/`, registrados con `loki_add_test_executable` (gtest_add_tests).
  Ejecutables: loki_util_tests, loki_model_tests, loki_commands_tests, loki_app_tests (Qt, offscreen).
  Helpers en `tests/support/` (TempDir, model_fixtures, MediaAssets = medios sintéticos generados con libav:
  luma de cada fotograma = `expectedLuma(i)`). Datos en `tests/data/` (`LOKI_TEST_DATA_DIR`).
  Motor de playback testeable con `ManualAudioDevice` + `TimeSource` falso.
  `loki_gl_tests` corre en plataforma nativa (GPU real); se omite si no hay OpenGL 3.3.
  Tests con Qt enlazan `tests/qt_test_main.cpp`. Assets pequeños generados con FFmpeg en `tests/assets`.
- Commits: Conventional Commits, autor = usuario (git config del repo). **Sin `Co-Authored-By` ni otra
  atribución**: el usuario debe figurar como único colaborador.
- Monitores de video: `ui::VideoView` usa `QOpenGLWindow` nativo + `createWindowContainer`, NO
  `QOpenGLWidget` (con QOpenGLWidget la ventana se veía en blanco en Intel HD 620). Las capturas GDI de
  PowerShell no ven contenido GL nativo: para verificar el render usar `grabFramebuffer()`.
- Cambios al diseño original por limitaciones de la máquina: proponer y esperar aprobación antes de aplicarlos.

## Entorno local (máquina del usuario)
- CPU i5-7200U (2C/4T), 16 GB RAM, GPU Intel HD 620 + NVIDIA GeForce 920MX (2 GB, sin NVENC).
- C: con poco espacio: **todo se instala en F:\dev** (pedido del usuario).
  - VS 2022 Build Tools `F:\dev\VS2022BuildTools` (cache/shared en F:\dev\VSCache, F:\dev\VSShared)
  - vcpkg `F:\dev\vcpkg` (`VCPKG_ROOT`), caché binaria `F:\dev\vcpkg-cache`
  - Qt `F:\dev\Qt\6.8.3\msvc2022_64` (`LOKI_QT_DIR`), aqt en `F:\dev\tools\aqt.exe`
  - FFmpeg CLI LGPL `F:\dev\ffmpeg\bin`

## Decisiones aprobadas (2026-11-08)
- C1: Qt precompilado (no vcpkg); ADS vía FetchContent.
- C2: criterio de rendimiento en dos niveles: local 1080p30 2V+2A tiempo real; 3V+4A en hardware de
  referencia o con preview 1/2.
- C3: dependencias de vcpkg se agregan por fase.
- Docking: Qt Advanced Docking System.

## Estado de fases
- [x] Paso 0 — inspección, CLAUDE.md, docs/ARCHITECTURE.md
- [x] 1. Fundaciones — CMake+vcpkg+presets, Qt+ADS shell 6 paneles, tema oscuro, spdlog, gtest (17 tests), CI Windows, ASan OK
- [x] 2. Modelo y comandos — Rational/timecode/UUID, modelo puro, Commands+UndoStack, JSON v1, ProjectController, 139 tests (ASan OK)
- [x] 3. Medios — probe, decoders (hwaccel+fallback), FrameCache, MediaCache, thumbnails, waveforms, Project panel
- [x] 4. Reproducción — PlaybackClock (audio master), PlaybackEngine, J/K/L, scrub, Source Monitor GL
- [x] 5. Línea de tiempo + Monitor de Programa — EditPlan/ReplaceClipsCommand (insert/overwrite/razor/move/trim/delete), sources de playback (archivo/secuencia), TimelineView, Program Monitor, 251 tests
- [x] 6. Composición GPU y efectos — effect registry, CompositeFrame (render_core), GlCompositor ping-pong RGBA16F + 8 blend modes, Effect Controls con keyframes, 269 tests (+4 GPU)
- [x] 7. Edición avanzada — ripple/roll/slip/slide, ripple delete, lift/extract con marcas In/Out de secuencia, link/unlink, ShortcutManager + diálogo de atajos, 285 tests
- [ ] 8. Audio
- [ ] 9. Transiciones, color, títulos
- [ ] 10. Exportación
- [ ] 11. Robustez y rendimiento

- D1–D4 (Fase 2): modelo C++ puro + adaptador Qt; solo Commands mutan; formato .loki JSON (tiempos [num,den],
  UUID, ruta abs+rel); sin solapes en pista.

- D11–D14 (Fases 6/7): composición = descripción pura (render_core) + GlCompositor en el monitor (mismo
  compositor para export); Movimiento/Opacidad fijos guardados al primer cambio, escalado por defecto ninguno;
  marcas In/Out de secuencia opcionales (v1); ripple delete solo desplaza pistas con el tramo libre.
- D5–D10 (Fases 3/4): FFmpeg compilado una vez con todas las features LGPL; monitores en OpenGL (copia de hw
  frames a RAM, sin zero-copy); miniaudio adelantado a Fase 4; assets de test generados en runtime con libav;
  marcas In/Out opcionales en MediaItem (v1 compatible); caché configurable (usuario: F:\LokiCache).

## Decisiones pendientes
- (ninguna)
