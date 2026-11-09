# Rendimiento medido (loki_bench)

Máquina: i5-7200U (2C/4T), Intel HD 620 + GeForce 920MX. Medio: H.264 1080p30 sintético. Duración de cada prueba: 10 s.

## Reproducción (reloj de audio maestro, composición GL incluida)

| Secuencia | Mostrados | Perdidos | Cortes de audio | Desfase A/V máx. (fotogramas) | Render GL medio | Cumple |
|---|---|---|---|---|---|---|
| 2V+2A, 1080p30, Full | 298 | 1 | 0 | 0.80 | 6.4 ms | **NO** |
| 2V+2A, 1080p30, 1/2 | 299 | 0 | 0 | 0.80 | 2.8 ms | OK |
| 3V+4A, 1080p30, 1/2 | 294 | 4 | 3 | 1.40 | 4.3 ms | **NO** |

## Scrubbing (calidad 1/2, 2V+2A, 20 saltos; objetivo < 100 ms)

| Medios | Saltos | Medio | Máximo | Cumple |
|---|---|---|---|---|
| Originales (GOP 30) | aleatorios | 107 ms | 341 ms | **NO** |
| Originales (GOP 30) | cortos (arrastre) | 50 ms | 148 ms | **NO** |
| Proxies 720p (GOP 5) | aleatorios | 33 ms | 205 ms | **NO** |
| Proxies 720p (GOP 5) | cortos (arrastre) | 24 ms | 78 ms | OK |

## Exportación

2V+2A, 150 fotogramas 1080p30 a H.264: 7.9 s (19.0 fps, 0.63x tiempo real), GPU (OpenGL) + h264_qsv.

## Memoria (30 ciclos abrir/reproducir/saltar/cerrar)

Crecimiento de memoria privada entre el ciclo 5 y el 30: -181.4 MB (objetivo < 20 MB): OK

UI congelada > 50 ms: la mide `ui::StallDetector` dentro de la app (aviso en el log). Fugas: tests con ASan en debug (la suite completa corre en el preset `windows-msvc-asan`).
