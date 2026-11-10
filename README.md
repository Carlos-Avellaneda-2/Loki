# Loki

Editor de video no lineal (NLE) para PC, inspirado en el flujo de trabajo de Premiere Pro.
C++20 · Qt 6 · FFmpeg (LGPL) · OpenGL.

- Compilar: [docs/BUILD.md](docs/BUILD.md)
- Arquitectura: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Formato de proyecto: [docs/PROJECT_FORMAT.md](docs/PROJECT_FORMAT.md)
- Rendimiento medido: [docs/PERFORMANCE.md](docs/PERFORMANCE.md)
- Licencias: [docs/LICENSING.md](docs/LICENSING.md)

---

# Manual de uso

## 1. Instalar y abrir

1. Ejecuta `Loki-<versión>-setup.exe` (se genera en `build\installer\`). Se instala solo para tu usuario y no pide
   permisos de administrador.
2. Al terminar hay un acceso directo **Loki** en el escritorio y en el menú Inicio. Haz doble clic y se abre.
3. Los archivos `.loki` se abren con doble clic.
4. Desde la línea de comandos, `loki.exe proyecto.loki` abre ese proyecto y `loki.exe video.mp4` lo importa.

**Instalar o actualizar con un solo comando** (desde la carpeta del código): compila, genera el instalador e instala
en `F:\Programas\Loki` con el acceso directo del escritorio. Cierra Loki antes.

```bash
powershell -ExecutionPolicy Bypass -File scripts\install-local.ps1
```

Para otra carpeta: `-Dir D:\Apps\Loki`. Para desinstalar: Configuración de Windows > Aplicaciones > Loki.

**Requisitos:**
- Windows 10/11 de 64 bits y una GPU con OpenGL 3.3.
- Con dos núcleos alcanza para 1080p30 con 2 pistas de video y 2 de audio. Para más pistas, usa la calidad 1/2 o
  proxies.
- Un equipo con 8 hilos o más y 12 GB de RAM entra solo en el modo de rendimiento **High** (sección 12): cachés más
  grandes, más capas de video y decodificación en paralelo.

## 2. La ventana

Loki se organiza en paneles que se pueden mover, apilar como pestañas o sacar a una ventana aparte (arrastra la
pestaña del panel). Window muestra u oculta cada panel.

| Panel | Para qué sirve |
|---|---|
| **Project** | Medios importados, carpetas (bins) y títulos. |
| **Effects** | Efectos de video y transiciones de video y audio. |
| **Source** (monitor de origen) | Ver un clip, marcar In/Out e insertarlo en la secuencia. |
| **Program** (monitor de programa) | Ver la secuencia montada. |
| **Timeline** | Montar: pistas de video (V1, V2…) y de audio (A1, A2…). |
| **Effect Controls** | Parámetros y keyframes del clip o la transición seleccionada. |
| **Audio Track Mixer** | Fader, paneo, mute y solo de cada pista, y el master. |
| **Audio Meters** | Nivel de lo que suena (pico y RMS). |

**Workspaces:** Window > Workspaces.
- **Editing**: el montaje general.
- **Color**: monitor de programa grande con Effect Controls al lado.
- **Audio**: el mezclador a la vista.
- **Save as New Workspace…** guarda tu propia disposición; **Delete Workspace** la borra.

## 3. Proyecto

| Acción | Menú | Atajo |
|---|---|---|
| Nuevo proyecto | File > New Project | Ctrl+Alt+N |
| Abrir | File > Open Project… | Ctrl+O |
| Guardar / Guardar como | File > Save / Save As… | Ctrl+S / Ctrl+Shift+S |
| Importar medios | File > Import… (o arrastrar archivos al panel Project) | Ctrl+I |
| Deshacer / Rehacer | Edit > Undo / Redo | Ctrl+Z / Ctrl+Shift+Z |

- El proyecto es un archivo `.loki` (JSON). Guarda la ruta absoluta de cada medio y también la relativa: si mueves
  la carpeta del proyecto junto con los medios, Loki los vuelve a encontrar.
- En el panel Project, clic derecho > Rename / Delete. Con el botón de carpeta creas bins. Los botones de lista y
  miniaturas cambian la vista.
- El panel Project muestra resolución, fps, duración y códec de cada medio.

### Secuencias y pistas

Un proyecto puede tener varias secuencias: por ejemplo, la versión 4K, la vertical para redes y el tráiler.

| Acción | Menú | Atajo |
|---|---|---|
| Nueva secuencia | Sequence > New Sequence… | Ctrl+N |
| Cambiar resolución, fps o nombre | Sequence > Sequence Settings… | |
| Agregar varias pistas de video y audio | Sequence > Add Tracks… | |
| Borrar la secuencia activa | Sequence > Delete Sequence | |

- **Presets de secuencia:**
  - HD 1080p (23.976 a 60 fps).
  - QHD 1440p (30 y 60).
  - 4K UHD (23.976 a 60) y DCI 4K.
  - 720p.
  - Vertical 1080×1920 (reels, stories).
  - Cuadrado 1080×1080.
  - **Custom:** cualquier tamaño hasta 8192 px y 120 fps.
- Al crear una secuencia eliges cuántas pistas de video (hasta 99) y de audio (hasta 99) trae. Después puedes
  agregar más con Add Tracks… o con clic derecho en la cabecera del timeline.
- El menú desplegable arriba a la izquierda del timeline cambia la secuencia que ves en el timeline y en el monitor
  de programa.

### Autoguardado y recuperación

- Loki escribe una copia de recuperación unos segundos después de cada cambio. Además, cada 5 minutos guarda una
  copia con fecha y conserva las últimas 10.
- Si Loki se cierra mal (corte de luz, cierre forzado), al abrirlo de nuevo pregunta si quieres recuperar los cambios.
  Si dices que sí, el proyecto se abre con su nombre original y marcado como no guardado: guárdalo con Ctrl+S. Si dices
  que no, la copia queda en la carpeta de autoguardado como `<proyecto>-recovered-<fecha>.loki`.
- Las copias están en `<carpeta de caché>\autosave`. El intervalo y la cantidad de copias se cambian en
  Edit > Preferences.

## 4. Monitor de origen: elegir el trozo de un clip

1. Doble clic en un medio del panel Project para abrirlo en Source.
2. Reproduce y navega:

| Tecla | Acción |
|---|---|
| Espacio | Reproducir / pausa |
| J / K / L | Retroceder / parar / avanzar; repetir J o L acelera (2×, 4×, 8×) |
| ← / → | Un fotograma atrás / adelante |
| I / O | Marcar In / Out |
| , (coma) | **Insertar** en la secuencia (desplaza lo que hay después) |
| . (punto) | **Sobrescribir** en la secuencia (reemplaza lo que hay) |

3. El clip entra en el cabezal del monitor de programa, en las **pistas destino**: los botones V y A azules a la
   izquierda del timeline. Haz clic en ellos para elegir otra pista o para insertar solo video o solo audio.

También puedes arrastrar un medio del panel Project al timeline. Al soltar sobrescribe; con Ctrl insertas.

## 5. Timeline

### Navegar

| Tecla | Acción |
|---|---|
| Espacio | Reproducir / pausa |
| ← / →, Shift+← / Shift+→ | 1 fotograma, 5 fotogramas |
| ↑ / ↓ | Punto de edición anterior / siguiente |
| Inicio / Fin | Principio / final de la secuencia |
| = / − | Zoom más / menos (también Alt + rueda del ratón) |
| \ | Ajustar toda la secuencia a la ventana |
| S | Activar o desactivar el imán (snap) |

Haz clic o arrastra en la regla para mover el cabezal.

### Cabecera de cada pista

- **V / A**: pista destino (dónde entran los clips desde Source o al pegar).
- **S** (solo audio): escuchar solo esa pista.
- **L**: bloquear la pista.
- **E / M**: ocultar el video de la pista / silenciar el audio.
- Clic derecho en la cabecera: añadir o borrar pistas.

### Herramientas

| Tecla | Herramienta | Uso |
|---|---|---|
| V | Selección | Mover clips, recortar arrastrando los bordes |
| C | Cuchilla | Cortar el clip donde haces clic (Shift: todas las pistas) |
| B | Ripple | Recortar un borde y cerrar o abrir el hueco moviendo lo que sigue |
| N | Roll | Mover el punto de corte entre dos clips vecinos |
| Y | Slip | Cambiar qué parte del medio se ve, sin mover el clip |
| U | Slide | Mover un clip entre sus vecinos, que se ajustan |

Los botones de herramienta están arriba a la derecha del timeline.
- Arrastrar con la herramienta Selección mueve los clips vinculados (video y su audio) juntos. Con Alt arrastras
  solo el clip.
- Ctrl+clic y Shift+clic suman clips a la selección.

### Edición

| Atajo | Acción |
|---|---|
| Supr / Retroceso | Borrar (deja hueco) |
| Shift+Supr | Ripple delete (borra y cierra el hueco) |
| Ctrl+K | Cortar en el cabezal las pistas seleccionadas (Ctrl+Shift+K: todas) |
| Ctrl+C / Ctrl+X | Copiar / cortar |
| Ctrl+V / Ctrl+Shift+V | Pegar sobrescribiendo / pegar insertando, en el cabezal |
| Ctrl+L | Vincular / desvincular clips |
| I / O | Marcas In / Out de la secuencia |
| Shift+I / Shift+O | Ir al In / Out |
| Ctrl+Shift+X | Borrar las marcas |
| ; | Lift: quitar el tramo In–Out dejando hueco |
| ' | Extract: quitar el tramo In–Out cerrando el hueco |

Clic derecho sobre un clip ofrece Enable/Disable, Delete, Ripple Delete, Link/Unlink, Copy, Cut y Paste.

### Atajos personalizados

En Edit > Keyboard Shortcuts… se cambia cualquier atajo. Si dos comandos del mismo panel quedan con la misma
tecla, el diálogo lo marca y no deja guardar. **Restore Defaults** vuelve a los atajos originales.

## 6. Monitor de programa

- Muestra la secuencia en el cabezal.
- **Full / 1/2 / 1/4**: resolución de la vista previa. Bájala si se pierden fotogramas; no afecta a la exportación.
- **Proxy**: cambia entre los proxies y los originales (sección 10).
- Las teclas I, O, ; y ' también funcionan con el foco en este monitor.

## 7. Efectos de video, color y títulos

### Movimiento y opacidad

Todos los clips de video tienen estos efectos en Effect Controls (selecciona el clip):
- **Motion**: posición, escala, rotación y punto de anclaje.
- **Opacity**: opacidad y modo de fusión (Normal, Multiply, Screen, Overlay, Add, Darken, Lighten, Difference).

### Keyframes (animación)

1. Pon el cabezal donde empieza la animación.
2. Activa el cronómetro (⏱) del parámetro: crea un keyframe con el valor actual.
3. Mueve el cabezal y cambia el valor: se crea otro keyframe.
4. ◀ y ▶ saltan de un keyframe a otro, y ◆ agrega o quita uno.
5. En la franja de keyframes puedes arrastrar los rombos. Con clic derecho eliges la interpolación: lineal,
   bezier (suave) o hold (salto).

**Reset** devuelve el efecto a sus valores por defecto.

### Corrección de color y LUT

1. Abre el panel **Effects** (pestaña junto a Project).
2. Arrastra **Color Correction** o **LUT** sobre un clip, o selecciona clips y haz doble clic en el efecto.
3. En Effect Controls:
   - **Color Correction**: exposición (EV), contraste, saturación y temperatura (cálido/frío).
   - **LUT**: con **…** eliges un archivo `.cube`; **Intensity** mezcla el LUT con la imagen original.
4. **Remove** quita el efecto.

### Títulos

1. File > New Title (Ctrl+T). El título aparece en el cabezal, en la primera pista de video libre por encima de la
   pista destino, y dura 5 segundos.
2. Con el título seleccionado, en Effect Controls > **Text**: texto, fuente, color, tamaño, posición y sombra.
3. Recórtalo o muévelo como cualquier clip. Motion y Opacity también funcionan con los títulos.

## 8. Transiciones

| Transición | Tipo |
|---|---|
| Cross Dissolve | Video: fundido de un clip al otro |
| Dip to Black | Video: el saliente va a negro y luego aparece el entrante |
| Wipe | Video: barrido de izquierda a derecha |
| Constant Power | Audio: crossfade |

- **Aplicar:** arrastra la transición desde el panel Effects hasta un corte o un borde de clip. El lado del clip
  donde sueltas decide el borde. Otra forma: pon el cabezal cerca de un corte y pulsa **Ctrl+D** para video o
  **Ctrl+Shift+D** para audio (actúa sobre la pista destino).
- En un corte entre dos clips la transición es doble: un clip se funde con el otro. En un borde libre funde con
  nada, como un fade in/out.
- **Editar:** haz clic en la transición en el timeline. Effect Controls muestra la duración (en fotogramas) y la
  alineación (centrada en el corte, empieza en el corte o termina en el corte). **Remove** o Supr la borran.
- Si el clip no tiene media extra después del corte, durante la transición se congela su último fotograma.
- Si mueves o borras un clip y el corte desaparece, la transición se borra con esa misma edición; Ctrl+Z la
  devuelve.

## 9. Audio

- **Volumen de un clip:**
  - Arrastra hacia arriba o abajo la línea amarilla del clip de audio.
  - Ctrl+clic en la línea agrega un keyframe.
  - También lo puedes cambiar en Effect Controls > Volume.
- **Paneo del clip:** Effect Controls > Panner (−100 izquierda, 100 derecha).
- **Fundidos:** arrastra los cuadraditos de las esquinas superiores del clip de audio, el izquierdo para fade in y
  el derecho para fade out.
- **Audio Track Mixer:** fader de −∞ a +6 dB, perilla de paneo, M (mute) y S (solo) por pista, y el fader master.
  Cada arrastre se deshace en un solo paso.
- **Audio Meters:** pico y RMS de lo que suena. La luz roja de arriba indica saturación (0 dBFS); haz clic en el
  medidor para apagarla.

## 10. Proxies (edición fluida con medios pesados)

1. En el panel Project, selecciona los medios, clic derecho > **Create Proxy (720p)**. Se generan en segundo plano y
   el progreso aparece en la barra del panel.
2. Activa el botón **Proxy** de cualquiera de los monitores: la vista previa usa los proxies donde existen.
3. Desactívalo para ver los originales.

La exportación **siempre** usa los originales. El sonido sale siempre del original.

## 11. Exportar

1. File > Export Media… (Ctrl+M).
2. Elige:
   - **Preset**:

     | Preset | Resultado |
     |---|---|
     | H.264 - Match Sequence | Tamaño y fps de la secuencia, .mp4 |
     | H.264 - 1080p30 / 1080p60 / 1440p60 / 4K UHD 30 / 4K UHD 60 | Esa resolución y fps, .mp4 |
     | H.264 - 720p | .mp4 liviano |
     | HEVC (hardware) - Match Sequence / 4K UHD 30 / 4K UHD 60 | Solo si tu GPU tiene codificador HEVC (NVIDIA, Intel o AMD recientes) |
     | ProRes 422 HQ (.mov) | Archivo intermedio de alta calidad, con audio PCM |
     | WAV - Audio Only | Solo la mezcla de audio |

   - **Output File**: dónde se guarda.
   - **Range**: toda la secuencia, o de la marca In a la Out.
3. **Export**. La barra muestra el progreso y puedes seguir usando Loki mientras exporta. **Cancel** detiene la
   exportación y borra el archivo a medias.

Loki elige solo el codificador H.264: NVIDIA (NVENC), Intel (QSV), AMD (AMF) o, si no hay ninguno, OpenH264 por
software. Las transiciones, el color, los LUT, los títulos y la mezcla de audio se exportan igual que se ven en el
monitor de programa.

## 12. Preferencias

Edit > Preferences…:
- **Media Cache folder**: dónde se guardan miniaturas, formas de onda, proxies y autoguardados. **Clean Cache** la
  vacía.
- **Auto Save**: activar o desactivar, cada cuántos minutos y cuántas versiones conservar.
- **Playback performance:** aplica al reiniciar Loki. La pantalla muestra cuántos hilos y cuánta RAM detectó.

  | Modo | Para | Qué cambia |
  |---|---|---|
  | **Auto** | Por defecto | Elige High con 8 hilos o más y 12 GB de RAM; si no, Standard |
  | **Standard** | Portátiles de 2 a 4 núcleos | Caché de 384 MB, 8 capas de video abiertas, decodifica capa por capa |
  | **High** | Equipos potentes | Caché de 1/8 de la RAM (hasta 4 GB), 32 capas, más fotogramas por adelantado, varias capas a la vez en varios núcleos |

## 13. Problemas frecuentes

| Problema | Solución |
|---|---|
| La reproducción se corta o pierde fotogramas | Baja la calidad del monitor a 1/2 o 1/4, o crea proxies |
| El scrubbing va lento con medios 4K / H.264 largos | Crea proxies y activa **Proxy** |
| Un medio aparece desconectado | Se movió o se borró el archivo: vuelve a ponerlo en su ruta o mueve la carpeta del proyecto junto con los medios |
| Un LUT no se aplica | El archivo debe ser `.cube` 3D (`LUT_3D_SIZE`); los LUT 1D no son compatibles |
| Loki se cerró de golpe | Ábrelo de nuevo y acepta recuperar los cambios |
| Quiero ver qué pasó | El registro `loki.log` está en la carpeta `logs`, dentro de `%LOCALAPPDATA%\Loki` (incluye avisos si la interfaz se congeló más de 50 ms) |
