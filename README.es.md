<div align="center">

# NFSMW Android Evolved

**Need for Speed: Most Wanted (2005) para Android ARM64**

APK nativo · Vulkan 1.1+ · controles táctiles · generación local de shaders

[Descargar la última versión](https://github.com/codepdbh/nfsmw-android/releases/latest) · [English](README.md)

</div>

Este proyecto adapta a Android el trabajo de recompilación de [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) y el SDK [ReXGlue](https://github.com/rexglue/rexglue-sdk). La aplicación usa código C++ recompilado, SDL3 y el renderizador nativo sobre Vulkan. No emula una Xbox 360 ni incluye los archivos del juego.

## Requisitos

- Android 8 o posterior y procesador ARM64 (ARMv8.0 o posterior).
- GPU con **Vulkan 1.1 o posterior** y *descriptor indexing*: de serie en Vulkan 1.2, o mediante la extensión `VK_EXT_descriptor_indexing` en controladores 1.1. Desde la v0.3.7 ya no hacen falta `shaderInt64` ni *buffer device address*. El launcher comprueba tu GPU antes de jugar.
- Una copia propia de **Need for Speed: Most Wanted (2005), Xbox 360, edición PAL España**, extraída y con `default.xex`, `NFS/` y `Movies/`. Este APK se compila para el ejecutable de esa edición; con otra edición el juego se cierra al arrancar y el launcher te avisa. Los archivos de la versión de PC, PS2 o una ISO sin extraer no sirven.
- Espacio en la memoria interna para el APK, la carpeta del juego y los archivos generados.

Probado en un **Samsung Galaxy S25 Ultra** (Adreno 830), un **Galaxy A55** (Xclipse 530) y varios Mali (G57, G615, G68, G720) según los informes de los testers.

**Todavía no funcionan** las GPU sin *descriptor indexing* ni siquiera con controladores modernos: Mali-G52/G72/G76 (Bifrost), Adreno 610 y PowerVR GE8320. El renderizador necesita otra gestión de texturas para ellas.

## Instalación y primer inicio

1. Descarga el APK de la última versión en [Releases](https://github.com/codepdbh/nfsmw-android/releases/latest) y ábrelo en el teléfono. Si Android lo solicita, permite **Instalar aplicaciones desconocidas** al navegador o al gestor de archivos.
2. Instala el APK y abre **Need for Speed Most Wanted**. Autoriza el acceso a archivos que solicita la app; en Android 11 o posterior aparece el ajuste de **acceso a todos los archivos**. Después vuelve al launcher.
3. Copia `default.xex`, `NFS/` y `Movies/` directamente dentro de `Memoria interna/nsfmw-androidevolved/`. También puedes pulsar **Elegir carpeta del juego** y seleccionar la carpeta extraída; la app la copia a ese destino.
4. Comprueba que el launcher marque los archivos como disponibles y pulsa **Jugar**. La primera vez genera `nfsmw_shaders.nfsp` a partir de tu copia y muestra el avance: desde unos segundos hasta un par de minutos en teléfonos modestos.
5. El juego se abre en horizontal. Para empezar, prueba 1280×720 y 60 FPS.

La carpeta debe quedar así:

```text
Memoria interna/nsfmw-androidevolved/
├── default.xex
├── NFS/
├── Movies/
├── nfsmw_shaders.nfsp           (generado por la app)
└── nfsmw_shaders.nfsp.version   (generado por la app)
```

No descargues ni compartas archivos del juego. La biblioteca de shaders se genera en el dispositivo a partir de tu copia.

### Actualizar desde una versión anterior

Instala el nuevo APK encima del anterior, **sin desinstalar ni borrar los datos de la app**. Las partidas y los ajustes se conservan. Si la versión nueva cambia los shaders, la app los vuelve a generar sola al pulsar **Jugar**; con la v0.3.7 ocurre una vez.

### Si no aparece Jugar

- Revisa el permiso de archivos y vuelve a abrir la app.
- Comprueba que `default.xex` esté directamente en `nsfmw-androidevolved/`, junto a `NFS/` y `Movies/`, sin una carpeta adicional en medio.
- Si la importación falla, revisa el espacio libre y selecciona la carpeta que contiene los tres elementos.

## Launcher, ajustes y controles

- **Gráficos:** resolución interna, límite de FPS, antialiasing, sombras, reflejos, resplandor del cielo, filtro de imagen y formato de pantalla.
- **Idioma de los textos** (v0.3.7): el disco PAL trae los textos de diez idiomas (español, inglés, francés, alemán, italiano, neerlandés, sueco, danés, finés y polaco). Elige uno en el launcher. Las voces y los vídeos siguen en el idioma del disco.
- **Volumen del juego**: de Normal a Máximo.
- **Controles táctiles:** dirección analógica o por inclinación, cruceta, A/B/X/Y, LB/RB, BACK, START, freno y acelerador. Desde el editor (engranaje) puedes moverlos, cambiar su tamaño, ocultarlos y ajustar su opacidad.
- **Mandos Bluetooth y USB**: funcionan como jugador 1 y ocultan los controles táctiles.
- **Enviar crash o log**: genera un ZIP con el informe de tu dispositivo para compartirlo por correo o en un issue de GitHub. Adjunta el ZIP al formulario. Consulta [la guía para testers](docs/android-testers.md).

## Novedades de la v0.3.7

- **Vulkan 1.1:** el renderizador nativo funciona en controladores 1.1 con `VK_EXT_descriptor_indexing`. Los shaders (SPIR-V 1.5) se convierten a SPIR-V 1.3 al cargarse; los 152 superan el validador oficial para Vulkan 1.1.
- **Sin `shaderInt64` ni buffer device address:** los shaders ya no leen constantes mediante punteros de 64 bits. Esto abre el juego a muchos Adreno 7xx y Mali-G57/G68 que se quedaban en *«el dispositivo Vulkan no tiene shaderInt64»*. La biblioteca de shaders se regenera una vez (PAL España: SHA-256 `b84602ca…`).
- **Idioma de los textos** seleccionable en el launcher.
- **Aviso de edición:** si tu `default.xex` no es PAL España, el launcher lo explica en lugar de cerrarse con *«No function registered at 8262E768»*.
- Los informes incluyen la edición del juego, `shaderInt64`, el tipo de *descriptor indexing* y la lista de extensiones Vulkan.

Comprobado en el Galaxy S25 Ultra, también simulando un controlador Vulkan 1.1 sin punteros de 64 bits. Los detalles están en la [guía para testers](docs/android-testers.md).

## Versiones anteriores

- **v0.3.6:** conversión en CPU de las texturas BC1–BC5 que el controlador no admite; protección para Mali en **Estabilidad gráfica**.
- **v0.3.5:** modo **Compatibilidad · experimental** (backend Xenos), corrección de memoria para kernels antiguos e informes desde el launcher.
- **v0.3.4:** corrección de bloques y reflejos incorrectos en Xclipse ([diagnóstico](docs/android-xclipse-diagnostic.md)).
- **v0.3.3:** audio de intros y cinemáticas corregido ([Audio en Android](docs/android-audio.md)).

## Compilar

Requisitos: Android SDK, NDK `28.2.13676358`, JDK 17 o posterior y PowerShell.

```powershell
.\build_android.ps1
```

El APK Release se genera en `android/app/build/outputs/apk/release/app-release.apk`, optimizado y para `arm64-v8a`.

## Proyecto y licencias

- `android/`: launcher, integración SDL y build de Android.
- `app/`, `sdk/`: aplicación recompilada y ReXGlue.
- `docs/`: notas del port y compilación.

Los archivos del juego y el código generado desde `default.xex` se mantienen fuera de Git. Consulta [`LICENSE`](LICENSE) y [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Proyecto de aficionados, sin afiliación con Electronic Arts; «Need for Speed» es una marca de Electronic Arts Inc.
