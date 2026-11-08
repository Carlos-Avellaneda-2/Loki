# Compilar Loki (Windows)

## Requisitos

Todo se instala en `F:\dev`, salvo lo que Windows obliga a poner en C:.

| Herramienta | Ubicación | Notas |
|---|---|---|
| Visual Studio 2022 Build Tools | `F:\dev\VS2022BuildTools` | Workload "C++ build tools". Incluye CMake, Ninja y ASan. El Windows SDK siempre se instala en `C:\Program Files (x86)\Windows Kits`. |
| vcpkg | `F:\dev\vcpkg` | Variable de usuario `VCPKG_ROOT`. Caché binaria en `F:\dev\vcpkg-cache` (`VCPKG_DEFAULT_BINARY_CACHE`). |
| Qt 6.8.3 MSVC 2022 x64 | `F:\dev\Qt\6.8.3\msvc2022_64` | Variable de usuario `LOKI_QT_DIR`. Instalado con `F:\dev\tools\aqt.exe`. |
| CLI de FFmpeg (LGPL) | `F:\dev\ffmpeg\bin` | Solo para generar los assets de prueba. |

## Compilar y probar

```powershell
. .\scripts\dev-env.ps1                 # entorno MSVC x64 y Qt en el PATH (una vez por terminal)
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
.\build\windows-msvc-debug\bin\loki.exe
```

Presets disponibles:

- `windows-msvc-debug`
- `windows-msvc-release`
- `windows-msvc-asan`: RelWithDebInfo con AddressSanitizer.

## Notas

- La primera configuración compila spdlog y gtest con vcpkg y tarda unos minutos. Las siguientes salen de la caché.
- Qt ADS se descarga y compila con `FetchContent` y queda como DLL en `build/<preset>/bin`.
- Los logs se escriben en `%LOCALAPPDATA%\Loki\Loki\logs\loki.log`.
- La disposición de paneles se guarda al salir, en el registro (`HKCU\Software\Loki\Loki`).
