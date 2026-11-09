# Licencias

Objetivo: que Loki se pueda distribuir cumpliendo la **LGPL** de FFmpeg y Qt, sin código GPL.

## Dependencias y cómo se cumplen

| Dependencia | Licencia | Enlace | Cumplimiento |
|---|---|---|---|
| Qt 6 (Core, Gui, Widgets) | LGPL-3.0 | Dinámico (DLL precompiladas) | Se distribuyen las DLL tal cual; el usuario puede reemplazarlas. Aviso de licencia y enlace al código fuente de Qt. |
| Qt Advanced Docking System | LGPL-2.1 | Dinámico (`qtadvanceddocking-qt6.dll`) | Compilado desde la etiqueta v5.1.1 sin modificaciones; se distribuye la DLL junto con su aviso de licencia. |
| FFmpeg 8 (vcpkg) | LGPL-2.1+ | Dinámico (DLL de vcpkg) | Features: avcodec, avformat, swresample, swscale, nvcodec, qsv, amf, openh264 y zlib. **Sin** `gpl`, `nonfree` ni `version3`. Se documenta la configuración usada y se ofrece el código fuente. |
| OpenH264 (Fase 10) | BSD-2-Clause | Dinámico | Encoder H.264 por software de respaldo. Ver la nota de patentes más abajo. |
| spdlog / fmt | MIT | Estático o dinámico | Aviso de licencia en la distribución. |
| nlohmann/json | MIT | Solo cabeceras | Aviso de licencia. |
| miniaudio | Dominio público / MIT-0 | Solo cabeceras (una TU con la implementación) | — |
| libvpl (QSV) | MIT | Dinámico | Dependencia de la feature `qsv` de FFmpeg. |
| nv-codec-headers | MIT | Solo cabeceras | NVDEC/NVENC se cargan en tiempo de ejecución desde el driver. |
| AMF headers | MIT | Solo cabeceras | — |
| GoogleTest | BSD-3-Clause | Solo en tests | No se distribuye. |

## Codificación H.264

Orden de preferencia en tiempo de ejecución:

1. `h264_nvenc` (NVIDIA): no disponible en la GeForce 920MX de la máquina de desarrollo, que no tiene NVENC.
2. `h264_qsv` (Intel Quick Sync): disponible en la Intel HD 620 de la máquina de desarrollo.
3. `h264_amf` (AMD).
4. `libopenh264`: respaldo por software.

Los encoders por hardware se usan a través de las APIs del fabricante (nv-codec-headers, oneVPL, AMF). Todas son compatibles con una compilación LGPL de FFmpeg.

Audio AAC: encoder nativo `aac` de FFmpeg (LGPL).

HEVC solo por hardware (`hevc_nvenc`, `hevc_qsv`, `hevc_amf`); nunca `libx265` (D23). ProRes: `prores_ks`, nativo
de FFmpeg (LGPL). WAV: `pcm_s16le`.

## Prohibido sin aprobación explícita

- `libx264`, `libx265`, `libxvid`, `libvidstab` y `frei0r`, o cualquier componente que requiera `--enable-gpl`.
- `libfdk-aac` y cualquier componente que requiera `--enable-nonfree`.
- Enlazar Qt o FFmpeg de forma estática.

## Herramientas (no se distribuyen)

- El CLI de FFmpeg en `F:\dev\ffmpeg` (build LGPL de BtbN) solo se usa para generar los assets de prueba.
- Qt se instala con `aqtinstall`.

## Patentes

H.264 y AAC están sujetos a patentes en algunos países. Las licencias de software anteriores no cubren las patentes. Cisco paga las regalías de OpenH264 solo cuando el usuario descarga el binario oficial de Cisco en tiempo de ejecución, no cuando se compila desde el código fuente. Hay que revisar esto antes de distribuir comercialmente.
