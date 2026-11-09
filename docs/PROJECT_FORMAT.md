# Formato de proyecto `.loki` (versión 1)

Archivo JSON en UTF-8. Lo escribe `model::saveProjectFile` y lo lee `model::loadProjectFile`
(`src/model/project_json.*`).

El guardado es atómico: se escribe un archivo temporal, se vuelca a disco y se renombra encima del original. Un
corte de luz deja el archivo viejo o el nuevo, nunca uno a medias.

## Reglas generales

| Concepto | Representación |
|---|---|
| Tiempo | `[numerador, denominador]` enteros, p. ej. `[1001, 30000]`. Siempre exacto, nunca decimal. |
| ID | UUID v4 en minúsculas (`"8f1c…-…"`). `null` = sin referencia (raíz, sin link). |
| Ruta | UTF-8 con `/`. `path` es absoluta. `relativePath` (opcional) es relativa a la carpeta del proyecto. |
| Números reales | `double` JSON, ida y vuelta exacta. |

Al abrir, si `path` no existe pero `relativePath` sí, se usa la relativa. Así se re-enlazan los medios cuando se
movió la carpeta del proyecto junto con ellos.

## Estructura

```json
{
  "format": "loki-project",
  "version": 1,
  "bins":  [{ "id": "…", "name": "Footage", "parent": null }],
  "media": [{
    "id": "…", "name": "interview", "path": "D:/clips/a.mp4", "relativePath": "../clips/a.mp4",
    "proxyPath": "D:/proxies/a_720p.mp4", "bin": "…",
    "info": { "hasVideo": true, "hasAudio": true, "width": 1920, "height": 1080,
              "frameRate": [30000, 1001], "duration": [60, 1], "sampleRate": 48000,
              "audioChannels": 2, "videoCodec": "h264", "audioCodec": "aac" }
  }],
  "sequences": [{
    "id": "…", "name": "Sequence 01",
    "settings": { "width": 1920, "height": 1080, "frameRate": [30, 1], "sampleRate": 48000 },
    "videoTracks": [{ "id": "…", "name": "V1", "locked": false, "hidden": false, "muted": false,
      "clips": [{ "id": "…", "media": "…", "name": "interview",
                  "start": [0, 1], "sourceIn": [0, 1], "duration": [5, 1],
                  "enabled": true, "link": null,
                  "effects": [{ "id": "…", "type": "loki.opacity", "enabled": true,
                    "parameters": [{ "key": "opacity", "value": 100.0,
                      "keyframes": [{ "time": [0, 1], "value": 0.0, "interpolation": "bezier",
                                      "easeIn": 0.333, "easeOut": 0.333 }] }] }] }] }],
    "audioTracks": []
  }]
}
```

- `videoTracks[0]` es V1, la pista de abajo.
- Los `time` de los keyframes están en el tiempo del *source* del clip.
- `interpolation` puede ser `hold`, `linear` o `bezier`.
- Una secuencia puede tener `"markIn"` y `"markOut"` (tiempos `[num, den]`, opcionales, Out exclusivo). Si
  faltan, la secuencia no tiene marcas; los archivos v1 anteriores siguen siendo válidos (D13).

## Validación

Al cargar se comprueban todas las invariantes:

- IDs únicos.
- Referencias existentes.
- Ningún ciclo en el árbol de bins.
- Clips ordenados y sin solapes.
- `duration > 0`, `sourceIn >= 0` y `start >= 0`.
- Ajustes de secuencia positivos.
- Keyframes con tiempos estrictamente crecientes e influencias en `[0, 1]`.

Si algo falla se lanza `ProjectFormatError` con la ruta JSON del problema, por ejemplo
`/sequences/0/videoTracks/1/clips/0/start: missing`.

## Versiones y migraciones

- `version` mayor que la soportada: error "saved by a newer Loki". Nunca se intenta abrirlo a medias.
- Versiones anteriores: `migrateProjectJson` aplica `kMigrations[v-1]` (vN a vN+1) en cadena hasta la actual.
- Para cambiar el esquema:
  1. Subir `kProjectFormatVersion`.
  2. Añadir el paso de migración.
  3. Añadir un archivo "golden" de la versión anterior en `tests/data/` con su test.
- `tests/data/project_v1.loki` es el golden de la versión 1 y debe seguir abriendo siempre.
