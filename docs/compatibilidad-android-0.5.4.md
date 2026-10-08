# Compatibilidad Android 0.5.4

## Cambios y procedencia

Se revisaron los 1.359 issues abiertos de Most Wanted el 8 de octubre de 2026. No se modificaron ni cerraron reportes. Las correcciones se adaptan del [commit 3d9358e del fork de victorgbd](https://github.com/victorgbd/NFSMW-Recompiled-Mobile/commit/3d9358e13dd6a10fb4b82f64040cddd07b215362), con atribución y conservando nuestro launcher y controles. El fork usa como base nuestro commit `6df1501`: Vulkan 1.1, constantes por UBO sin `shaderInt64`, descompresión BC y protección de oclusión en Mali ya estaban en nuestra versión.

- **Cuatro conjuntos de descriptores:** si `maxBoundDescriptorSets == 4`, las texturas 3D y los cubos comparten un conjunto con bindings distintos; samplers y constantes cambian de conjunto. Se adaptan tanto Vulkan como las decoraciones de todos los shaders. Los dispositivos con cinco o más conservan su distribución. Los módulos se validan antes de modificarlos para evitar conversiones parciales. `nfsmw_nativo_cuatro_conjuntos=true` permite probar esta ruta en otro dispositivo.
- **Órdenes en ARM:** publicación con store release del contador y lectura acquire antes de ejecutar. Evita observar la orden antes de que sus datos y dirección de función estén escritos. La cola cerrada conserva la llamada original. Este hook se activa en Most Wanted Android, no en Carbon.
- **Samsung Xclipse propietario:** BC4/BC5 se convierten en CPU mediante nuestro decodificador existente. Se conserva BC nativo con drivers Mesa identificados. Esta medida procede del fork y sigue pendiente de confirmación en un Xclipse real.
- **Drivers externos:** importación, selección, eliminación y prueba de ZIPs Android ARM64 mediante adrenotools. El proceso de prueba es independiente y se destruye al terminar; los hooks de un driver no se mezclan con los de otro. La prueba y el juego usan el mismo cargador. Un fallo se muestra explícitamente y el usuario puede volver al sistema. No se activa turbo KGSL.
- **Aislamiento de símbolos:** las dependencias estáticas del cargador no exportan las variables internas de linkernsbypass con nombres de funciones de Android. Esto evita una colisión con `libvndksupport` detectada en la prueba del cargador del sistema.

## Qué muestran los diagnósticos

| Issue | Evidencia | Consecuencia |
| --- | --- | --- |
| [1370](https://github.com/codepdbh/nfsmw-android/issues/1370) | Mali-G52 MC2, Vulkan 1.3.278, faltan `runtimeDescriptorArray` y flags de descriptor indexing | Una versión alta de Vulkan no garantiza esas funciones; cuatro conjuntos y BC por CPU no bastan para ese driver. |
| [1256](https://github.com/codepdbh/nfsmw-android/issues/1256) | Mali-G615, funciones nativas disponibles, edición USA, `No function registered at 8262E768` | Fallo de edición del ejecutable, no prueba de incompatibilidad de GPU. |
| [1188](https://github.com/codepdbh/nfsmw-android/issues/1188) | Pixel 6, Mali-G78, juego funcionando con parpadeos del coche y retrovisor | El soporte de arranque no implica ausencia de errores visuales; requiere prueba específica. |
| [1116](https://github.com/codepdbh/nfsmw-android/issues/1116) | PowerVR BXM-8-256, Vulkan 1.1.170, diagnóstico nativo compatible | Caso útil para probar los cuatro conjuntos; no se confirma su carrera sin reproducir el cierre. |
| [1222](https://github.com/codepdbh/nfsmw-android/issues/1222) | Mali-G610, APK 0.3.6, diagnóstico antiguo exige Vulkan 1.2/Int64 | Repetir con 0.5.4: esos requisitos se retiraron antes de esta revisión. |

Gran parte de los reportes corresponde a APK 0.3.5; varios no tienen datos de reproducción o indican que falta `default.xex`. No se contabilizan como reproducciones verificadas del fallo actual.

## Usar drivers

1. En **Opciones gráficas → Driver Vulkan**, pulsa **Importar ZIP**.
2. Selecciona un paquete para **tu GPU y tu versión de Android**, con `meta.json` y una biblioteca ARM64. También se aceptan paquetes con una carpeta contenedora. El importador verifica rutas, arquitectura y límites de tamaño; no modifica los drivers del sistema.
3. Pulsa **Probar driver**. El resultado muestra GPU, versión de Vulkan, límite de conjuntos y funciones que faltan. Al jugar con un driver externo se repite la prueba.
4. Si falla o empeora la imagen, elige **Driver del sistema**. **Eliminar seleccionado** borra solamente ese paquete importado.

Ejemplo de `meta.json`:

```json
{"name":"Mi driver Vulkan","libraryName":"libvulkan_freedreno.so","minApi":28}
```

**Turnip corresponde a Adreno; PanVK corresponde a Mali.** No son intercambiables. Importar un paquete no añade soporte a hardware que el propio driver no admite. Los requisitos de kernel, gralloc, Android y GPU siguen aplicándose. No se incluyen ni descargan drivers automáticamente.

La [release 0.4.0 del fork](https://github.com/victorgbd/NFSMW-Recompiled-Mobile/releases/tag/v0.4.0) marca Xclipse/PowerVR como sin probar y Mali-G57 como probado con fallos. Sus notas posteriores de PanVK registran inicialización sin renderizado y errores de dispositivo; no se anuncia PanVK como solución confirmada. Mali-G52/G72/G76 y Adreno 610 con drivers sin descriptor indexing aún necesitan otra ruta de renderer o un driver que proporcione realmente esas funciones.

Para Snapdragon 888, 8 Gen 1 y Adreno 720: probar primero el driver del sistema y 1024×576, luego un driver compatible con ese modelo si hay cierres o fallos gráficos. Resolución, límite de FPS y compatibilidad del driver son cosas distintas; no se prometen FPS sin medirlos en esos teléfonos.

## Verificación

- APK Release ARM64 compilado con NDK 28.2 y ThinLTO.
- Tests del ZIP: paquete con carpeta, ELF ARM64, rechazo de PC, rutas externas y metadatos demasiado grandes.
- En ARM64: 200.000 publicaciones de órdenes entre productor y consumidor, y rechazo de SPIR-V truncado sin modificaciones parciales.
- Los 152 módulos de la biblioteca PAL española se adaptaron a cuatro conjuntos y SPIR-V 1.3; `spirv-val --target-env vulkan1.1` terminó sin errores.
- El usuario confirmó carrera con imagen, sonido y controles correctos en Galaxy S25 Ultra, SM-S938B. Es una comprobación de regresión; no sustituye pruebas en Mali, Xclipse, Snapdragon 888/8 Gen 1 o Adreno 720.

## Dependencias y licencias

Se conserva GPL-3.0 y la atribución de los arreglos del fork. El cargador usa [libadrenotools](https://github.com/bylaws/libadrenotools) `8fae8ce254dfc1344527e05301e43f37dea2df80` y [liblinkernsbypass](https://github.com/bylaws/liblinkernsbypass) `aa3975893d83ef1bc84c321ec60c65fbf1287887`, incluidos como código fuente con sus licencias BSD-2-Clause en `sdk/thirdparty/libadrenotools`. Los paquetes de drivers importados por el usuario no forman parte del APK ni del repositorio.

## Aviso opcional de actualizaciones

El launcher consulta únicamente la última release estable del repositorio oficial, en segundo plano. Compara los números de versión y no ofrece versiones iguales, anteriores, borradores ni betas. Usa una caché de seis horas; **Buscar actualizaciones** fuerza la consulta. El aviso ofrece actualizar abriendo el APK oficial en GitHub o continuar con **Más tarde**. No bloquea el juego, no instala APKs ni envía diagnósticos. Un fallo de conexión solo se muestra si la consulta fue manual.

Se probaron las comparaciones de versiones nuevas, iguales y antiguas, números con varias cifras, betas y etiquetas inválidas. El endpoint real de GitHub respondió correctamente con el APK oficial. El usuario confirmó también que **Probar driver** muestra Adreno 830 y las funciones disponibles. El cargador se probó en ARM con driver del sistema, una biblioteca ausente y un ELF ARM64 sin implementación Vulkan: los errores se devuelven como diagnóstico.
