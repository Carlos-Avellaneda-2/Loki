# Rendimiento medido (loki_bench)

Máquina: i5-7200U (2C/4T), Intel HD 620 + GeForce 920MX. Medio: H.264 1080p30 sintético. Duración de cada prueba: 10 s.

## Reproducción (reloj de audio maestro, composición GL incluida)

| Secuencia | Mostrados | Perdidos | Cortes de audio | Desfase A/V máx. (fotogramas) | Render GL medio | Cumple |
|---|---|---|---|---|---|---|
| 2V+2A, 1080p30, Full | 298 | 1 | 0 | 0.90 | 6.2 ms | **NO** |
| 2V+2A, 1080p30, 1/2 | 299 | 0 | 0 | 0.80 | 2.7 ms | OK |
| 3V+4A, 1080p30, 1/2 | 299 | 0 | 0 | 0.80 | 3.2 ms | OK |

## Scrubbing (calidad 1/2, 2V+2A, 20 saltos; objetivo < 100 ms)

| Medios | Saltos | Medio | Máximo | Cumple |
|---|---|---|---|---|
| Originales (GOP 30) | aleatorios | 113 ms | 347 ms | **NO** |
| Originales (GOP 30) | cortos (arrastre) | 50 ms | 134 ms | **NO** |
| Proxies 720p (GOP 15) | aleatorios | 66 ms | 200 ms | **NO** |
| Proxies 720p (GOP 15) | cortos (arrastre) | 32 ms | 87 ms | OK |

## Exportación

2V+2A, 150 fotogramas 1080p30 a H.264: 8.7 s (17.3 fps, 0.58x tiempo real), GPU (OpenGL) + h264_qsv.

## Memoria (30 ciclos abrir/reproducir/saltar/cerrar)

Crecimiento de memoria privada entre el ciclo 5 y el 30: -130.7 MB (objetivo < 20 MB): OK

UI congelada > 50 ms: la mide `ui::StallDetector` dentro de la app (aviso en el log). Fugas: tests con ASan en debug (la suite completa corre en el preset `windows-msvc-asan`).
