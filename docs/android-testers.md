# Diagnosticos de testers (desde la compilacion 0.3.5)

El launcher incorpora **Enviar crash o log**. El telefono prepara un ZIP local
con la version de la app, modelo, Android, GPU/Vulkan, ajustes del launcher,
registros recientes de esta app y el historial de cierres que Android permite
consultar. Si esta disponible, incluye la traza de un cierre nativo o ANR.
Tambien conserva la ultima excepcion Java.

- **Correo** abre el selector para compartir el ZIP con el destinatario
  `daniebatuani@gmail.com`, asunto y texto inicial. Selecciona tu app de correo,
  describe lo sucedido y pulsa Enviar.
- **GitHub** pide guardar el ZIP y abre un nuevo issue en
  https://github.com/codepdbh/nfsmw-android/issues/new con los datos basicos
  rellenados. Inicia sesion si es necesario, describe como reproducir el fallo
  y adjunta el ZIP guardado antes de publicar el issue.
- **Guardar ZIP** permite guardar el informe sin abrir correo ni GitHub.

Si el juego se cierra, vuelve a abrir el launcher y usa el boton antes de
intentar varias partidas nuevas. Si se queda colgado, cierra la app desde
Android, vuelve a abrirla y genera el informe. Indica la edicion del juego y
los pasos para reproducir el problema. El registro no siempre contiene una
traza: depende de que Android la conserve y de como termino el proceso.

Los registros se acotan conservando el principio y el final. La app no adjunta
archivos del juego, partidas, identificadores del telefono ni registros de
otras apps. El ZIP no se envia automaticamente. GitHub no recibe adjuntos
desde un enlace: el tester adjunta el archivo en el formulario.

## Prueba de compatibilidad nativa en 0.3.6-experimental

El ZIP `NFSMW-diagnostico-20261001-180803-912.zip` corresponde a Xiaomi
2406APNFAG, Android 16 y Mali-G615 MC6 con Vulkan 1.3.247. Sus dos logs
nativos terminan rechazando `vertexPipelineStoresAndAtomics` antes de crear
el dispositivo; `gpu.json` marca BC1-5 ausentes y bloquea Nativo por BC1-3.
El logcat es de unos 24 minutos después y no contiene una traza del fallo.
No demuestra el cierre por oclusiones descrito por el otro tester del Pixel.

La compilación experimental separa los requisitos del renderer nativo de
los de Xenos y convierte BC1-5 a RGBA8/R8/RG8 cuando faltan. `gpu.json`
conserva las capacidades reales y añade `cpuTextureConversions`, los bits
de stores/atomics y `occlusionQueriesDefault`. Un formato BC ausente no
bloquea Nativo si existe su formato de conversión. Se conserva el rechazo
de las funciones de shaders realmente necesarias.

Para probar en Mali, selecciona **Renderizador → Nativo** y **Estabilidad
gráfica → Automática · protección Mali**, empieza en 1024×576 / 30 FPS y
deja pasar las intros, el menú y una carrera. Comprueba texturas, carretera,
coche, transparencias y reflejos. Envía un nuevo ZIP inmediatamente si falla,
indicando la última pantalla y los ajustes utilizados.

El ajuste automático evita todo el ciclo de consultas de oclusión en Mali
Android (creación, reset, begin/end y lectura de ambos pools). Se suprime
el destello solar y se decide el reflejo mediante lecturas de su textura.
Las consultas de tiempo/estadísticas son independientes y se conservan.
Es una alternativa conservadora basada en el reporte del tester; todavía
no se ha confirmado la causa de su cierre ni validado esta corrección en Mali.
**Máxima compatibilidad** hace lo mismo en cualquier GPU. **Efectos completos**
reactiva las oclusiones para comparar y puede provocar el cierre reportado.
La elección no modifica los otros ajustes del usuario.

Se comprobaron en PC y en el Redmi conectado los colores y alpha de BC1-5,
bordes no múltiplos de cuatro, orden de caras/mips, alineación de subidas y
rechazo de entradas inválidas. La prueba Vulkan en ese Redmi (Adreno 610,
Vulkan 1.1.128, sin BC) subió los cinco formatos convertidos a cubos de seis
caras y cuatro mips y obtuvo los mismos píxeles al leerlos de la GPU.
Esta prueba no usa archivos del juego y no valida una partida completa.
El Redmi sigue necesitando Xenos por las otras funciones nativas ausentes.

Pruebas independientes para desarrolladores:

```text
tools/tests/texturas_bc_test.cpp        # C++20, PC o Android ARM64
tools/tests/texturas_bc_vulkan_test.cpp # C++20, headers Vulkan y -lvulkan; Android ARM64
```

## Redmi Note 8 revisado

En el dispositivo conectado el 1 de octubre de 2026 se comprobaron:

- GPU Adreno 610, controlador Qualcomm del 25/09/2020, Vulkan 1.1.128.
- `shaderInt64 = false`; no anuncia `VK_EXT_descriptor_indexing` y faltan
  `runtimeDescriptorArray`, `descriptorBindingPartiallyBound`,
  `descriptorBindingSampledImageUpdateAfterBind` y
  `descriptorBindingUpdateUnusedWhilePending`.
- Anuncia buffer device address como extension, pero la interfaz nativa del
  SDK actualmente la habilita junto con el resto de funciones de Vulkan 1.2.
- BC1, BC2, BC3, BC4 y BC5 no estan soportados como imagenes muestreadas.
- El ejecutable y la biblioteca de shaders coinciden por SHA256 con la copia
  PAL Espana utilizada en las pruebas anteriores.
- El arranque llega a crear Vulkan, pero el renderer registra:
  `C6: el dispositivo Vulkan no tiene shaderInt64: no se dibuja`.
- El kernel 4.14 ignora `MAP_FIXED_NOREPLACE` y puede devolver otra direccion.
  El runtime aceptaba esa direccion como una reserva fija correcta, dejando
  protegida la pila guest reutilizada. Se corrige comprobando la direccion
  devuelta, descartando la reserva desplazada y habilitando la region original
  cuando ya esta reservada. La prueba nativa fallo antes y paso despues en
  este telefono: commit fijo, colisiones y reutilizacion de paginas protegidas.
- Una excepcion no atendida ahora sigue el cierre normal de Android; antes
  el manejador retornaba y repetia la misma instruccion fallida sin terminar.
  Se verificaron en el telefono los casos atendido, delegado y fatal.

Se conserva el cambio del usuario a `-march=armv8-a`: permite generar codigo
para procesadores ARMv8.0, pero no resuelve las limitaciones del controlador.
El launcher ahora comprueba las funciones necesarias para el renderizador
nativo y ofrece **Probar compatibilidad** y acceso al informe. BC4/5 no se exigen
en esta comprobacion, porque el Galaxy A55 probado admite BC1/2/3 y carece de
BC4/5.

## Probar el modo de compatibilidad

En el launcher selecciona **Renderizador → Compatibilidad · experimental**,
o acepta **Probar compatibilidad** cuando se detecten funciones nativas
ausentes. Pulsa **Jugar**. Este modo no necesita generar la biblioteca de
shaders del renderizador nativo.

Utiliza el backend Xenos con descriptores convencionales, render targets FBO
y conversion de las texturas BC no soportadas. Desactiva las sustituciones
nativas de D3D, materiales, matrices, escenarios y render sin mosaico para
conservar el camino original del juego. La compilacion de shaders es sincrona.
Estas decisiones priorizan conseguir el arranque y pueden provocar pausas.

Con este modo y la correccion de memoria, el Redmi probado reproduce los
videos de inicio y el usuario confirma que llega a conducir y que el coche
gira con el joystick, aunque funciona con mucha lentitud. Tambien se observaron esperas prolongadas de la
GPU y pantallas negras posteriores: el soporte sigue siendo experimental.
No se ha validado todavia en una carrera completa ni en Helio G99/G200.
Los testers de esos dispositivos deben adjuntar su ZIP indicando el modo
utilizado y la ultima pantalla que funciono.

El joystick tactil puede utilizarse incluso con **Inclinar** activado:
mientras se mantiene el dedo, manda el joystick; al soltarlo vuelve la
inclinacion. Esto corrige que el ajuste de inclinacion bloqueara el arrastre.

## Vulkan 1.1 y controladores sin shaderInt64 (v0.3.7, en prueba)

Revisión de 112 ZIP de diagnóstico (0.3.5 y 0.3.6) y 961 issues, el 5 de octubre de 2026:

- 394 reportes con Vulkan 1.1, 467 con 1.3. Las GPU más frecuentes: Mali-G57, Mali-G52, Adreno 610.
- En Vulkan 1.2+ el bloqueo más común era `shaderInt64`: Adreno 720/740 (controlador 1.3.128) y Mali-G57/G68.
- `No function registered at 8262E768` / `8262E9A8` no es un fallo del port: el `default.xex` es de otra
  edición (ver `app/overrides.toml`). El launcher ahora lo detecta y lo explica antes de arrancar.
- Muchos cierres de la 0.3.5 (`vertexPipelineStoresAndAtomics`) eran del modo compatibilidad (Xenos), que
  los Mali no cumplen; el renderizador nativo no lo necesita desde la 0.3.6.

Cambios:

- **Shaders sin punteros de 64 bits.** `assets/shaders/shader_common.h` deja fijo el camino de constantes por
  UBO, declara los push constants como `uint2` y anula `vk::RawBufferLoad`. El SPIR-V ya no usa `Int64`, `PhysicalStorageBufferAddresses`
  ni el modelo de memoria `PhysicalStorageBuffer64` (las 152 entradas: solo `Shader`, `ImageQuery` y
  `RuntimeDescriptorArray`). La biblioteca cambia de huella: PAL España `b84602ca…` (antes `a27aea23…`, la de
  Switch). La app regenera las bibliotecas antiguas (`nfsmw_shaders.nfsp.version` = `sin-punteros-1`).
- **El renderizador no exige `shaderInt64` ni buffer device address**; si existen se siguen habilitando.
- **Vulkan 1.1.** El SDK pide `VK_EXT_descriptor_indexing` y activa las mismas cuatro funciones que en 1.2. El
  SPIR-V 1.5 de la biblioteca se convierte a 1.3 al crear cada módulo (`app/src/nfsmw_spirv_vulkan11.h`):
  versión y lista de interfaz del `OpEntryPoint`. Los 152 módulos convertidos pasan
  `spirv-val --target-env vulkan1.1` (`tools/tests/spirv_vulkan11_test.cpp`).
- `nfsmw_nativo_simular_vulkan11 = true` fuerza esa conversión y la ausencia de punteros en cualquier GPU,
  para probar la ruta en un teléfono moderno.
- La sonda del launcher exige Vulkan 1.1 + descriptor indexing (núcleo o extensión) en lugar de Vulkan 1.2,
  y el informe incluye `shaderInt64`, el tipo de descriptor indexing y la lista de extensiones del controlador.

Lo que sigue sin funcionar: Mali-G52/G72/G76 (Bifrost), Adreno 610 y PowerVR GE8320 no tienen descriptor indexing
ni siquiera con controladores 1.3. El renderizador nativo actualiza sus montones de texturas mientras graba
(`UPDATE_AFTER_BIND`, `PARTIALLY_BOUND`); para esas GPU haría falta otra gestión de descriptores.
