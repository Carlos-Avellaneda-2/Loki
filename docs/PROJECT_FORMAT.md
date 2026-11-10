# Formato de proyecto `.loki` (versión 4)

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
- Mezcla de audio (Fase 8): campos opcionales que se omiten cuando valen lo mismo que por defecto.
  - Pistas: `"volumeDb"` (0), `"pan"` (0, rango −100…100) y `"solo"` (false).
  - Secuencias: `"masterVolumeDb"` (0).
  - Clips: `"fadeIn"` y `"fadeOut"`, tiempos `[num, den]` (sin fundido si faltan).
  - El volumen y el paneo de clip son efectos `loki.volume` (`level`, en dB) y `loki.panner` (`pan`).
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
- `tests/data/project_v2.loki` es el golden de la versión 2.
- `tests/data/project_v3.loki` es el golden de la versión 3.
- `tests/data/project_v4.loki` es el golden de la versión 4.

## Versión 2 (Fase 9, D19–D20)

La migración de v1 a v2 no cambia nada: un documento v1 ya es un v2 válido. Todo lo nuevo es opcional.

- Medios generados: `"generator": "loki.title"`, con `"path": ""`. El renderer dibuja la imagen; no hay archivo.
- Efectos con texto: `"strings": { "clave": "valor" }`, para parámetros que no se animan.
  - `loki.text`: `text`, `font`, `color` (`#rrggbb`).
  - `loki.lut`: `file` (ruta del `.cube`).
- Efectos nuevos:
  - `loki.color`: `exposure` (−4…4 EV), `contrast` (−100…100), `saturation` (0…200 %) y `temperature` (−100…100).
  - `loki.lut`: `intensity` (0…100 %).
  - `loki.text`: `size`, `positionX`, `positionY`, `shadow` (0/1), `shadowDistance` y `shadowOpacity`.
- Pistas: `"transitions": [{ "id", "type", "from", "to", "duration", "alignment" }]`.
  - `from`/`to`: el clip saliente y el entrante (`null` en una transición de un solo lado).
  - `alignment`: `center`, `start` o `end` respecto del corte.
  - `type`: `loki.crossDissolve`, `loki.dipToBlack`, `loki.wipe` o `loki.crossfade` (audio).
  - Una transición cuyos clips ya no existen o ya no se tocan queda inactiva y se ignora. Las ediciones del
    timeline la borran en el mismo paso de deshacer.

## Versión 3 (Fase 13, D28–D31)

La migración de v2 a v3 no cambia nada: todo lo nuevo es opcional.

- Clip:
  - `"speed": [num, den]`: segundos de origen por segundo de secuencia (`[1, 2]` = 50 %). Se omite en 100 %.
    `[0, 1]` es un cuadro congelado (Frame Hold) en `sourceIn`. Máximo 100 (10000 %).
  - `"reverse": true`: recorre `[sourceIn, sourceIn + duration × speed)` hacia atrás. Se omite si es falso.
  - Los keyframes siguen en tiempo del origen.
- Marcadores: `"markers": [{ "id", "time", "duration"?, "name"?, "comment"?, "color" }]`, ordenados por `time`.
  - En la secuencia: tiempo de la secuencia. En un medio (marcadores de clip): tiempo del origen.
  - `color`: 0 verde, 1 rojo, 2 violeta, 3 naranja, 4 amarillo, 5 blanco, 6 azul, 7 cian.
- Medios generados nuevos:
  - `"generator": "loki.nest"` con `"sequence": "<id>"`: una secuencia anidada. La secuencia debe existir, y
    ninguna secuencia puede contenerse a sí misma (directa o indirectamente).
  - `"generator": "loki.adjustment"`: capa de ajuste; sus efectos de color se aplican a las pistas de abajo.
- Una secuencia usada por un medio `loki.nest` no se puede borrar sin borrar antes ese medio.

## Versión 4 (Fase 14, D33–D37)

La migración de v3 a v4 no cambia nada: todo lo nuevo es opcional y va dentro de los efectos.

- `"masks"`: lista de máscaras del efecto (efectos con máscaras: opacity, color, lumetri, lut, gaussianBlur,
  sharpen).
  - `id`, `name`, `mode` (`add` / `subtract`), `inverted`.
  - `path`: vértices `[x, y, inX, inY, outX, outY]` en píxeles del medio; las manijas son relativas al vértice
    (0, 0 = esquina). Se usa cuando `pathKeys` está vacío.
  - `pathKeys`: `[{ "time", "path" }]` en tiempo del origen, ordenados, todos con la misma cantidad de vértices
    (máscara animada o seguida).
  - `parameters`: `feather` (px), `opacity` (%), `expansion` (px), con el mismo formato que los parámetros de
    efecto.
- `"curves"`: `{ "master" | "red" | "green" | "blue": [[x, y], ...] }`, al menos 2 puntos con x creciente, todo
  en 0…1. Una curva identidad se omite.
- `"samples"`: números de un análisis. Estabilizador (`loki.stabilizer`):
  `[1, rateNum, rateDen, startNum, startDen, ancho, alto, n, (dx, dy, rotación, escala) × n]`.
- Efectos nuevos: `loki.lumetri`, `loki.ultraKey` (texto `keyColor` `#rrggbb`), `loki.gaussianBlur`,
  `loki.sharpen`, `loki.stabilizer`.
- Transiciones nuevas: `loki.dipToWhite`, `loki.additiveDissolve`, `loki.push`, `loki.slide`, `loki.irisRound`,
  `loki.irisBox`, `loki.crossZoom`.

