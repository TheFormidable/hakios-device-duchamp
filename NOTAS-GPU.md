# hakiOS en el POCO X6 Pro: estado de la GPU y cómo seguir

Fecha: 12 de noviembre de 2026.

## Dónde está cada cosa

- `xnu/files/bsd/dev/redwood_mtkgpu.c`: el driver de la GPU dentro de XNU, tal como corrió en la corrida 25b. Es el último código probado.
- `xnu/duchamp-overlay.py`: mete los archivos de `xnu/files/` en el árbol de XNU (rama mtk de hakios-xnu-kernel) y engancha cada driver.
- `tools/embed-overlay.py`: vuelve a meter el overlay dentro de `mac/hakios-kernel.sh`. Córrelo después de cambiar cualquier cosa en `xnu/`.
- `mac/hakios-kernel.sh`: compila el kernel en tu Mac. Es la misma versión que usaste para la 25b.
- `mtkloader/` y `tools/mkimage.sh`: el loader y el armado de la imagen. `mtkloader/mtkgpu.h` tiene las sondas de la GPU desde el loader (sondas 1 a 4b).
- `windows/hakios-paso3.cmd`: flasheo y log (opción 1 y luego 2).
- `docs/porting-mt6897.md`: el registro de todas las corridas.
- El firmware `mali_csffw.bin` es de tu teléfono. No está en este paquete y no debe ir a ningún repositorio.

## Estado

- Corrida 25b (imagen 9f499853): el driver llegó a "came up to step 6 of 6" dentro de iOS 16.5. El GPUEB enciende la GPU, se carga el firmware, el MCU arranca (CSF 2.5.0, 8 grupos), se hace el setup global y una cola de prueba en el grupo 0 escribe `c0ffee01 600dcafe`. El tacto funciona al lado y no hay panic.
- Al terminar, la GPU queda encendida, el MCU corriendo y el grupo 0 terminado, libre para lo siguiente.
- Memoria de la GPU: 3 MB en 0x230000000, mapeados sin caché (solo accesos de 32 bits). Los registros están en 0x13000000, y solo se mapean los doorbells 0 y 1.

## Lo que falta para que iOS dibuje con la GPU

1. **User client de IOKit en el kernel** que hable la interfaz de panthor de Linux (las GPUs CSF), encima de redwood_mtkgpu.c. Lo que Mesa nuevo le pide al kernel:
   - DEV_QUERY (información de la GPU, de la CSIF, timestamp, prioridades, MMU)
   - VM_CREATE, VM_DESTROY, VM_BIND, VM_GET_STATE
   - BO_CREATE, BO_MMAP_OFFSET, BO_QUERY_INFO, BO_SET_LABEL, BO_SYNC, SET_USER_MMIO_OFFSET
   - GROUP_CREATE, GROUP_DESTROY, GROUP_SUBMIT, GROUP_GET_STATE
   - TILER_HEAP_CREATE, TILER_HEAP_DESTROY
   - los syncobjs de DRM (se pueden resolver en el lado de usuario, en redwood-drm)
2. **redwood-drm** (el sustituto de libdrm de la referencia): que mande las peticiones de panthor al user client en vez de las de panfrost.
3. **Mesa**: elegiste Mesa nuevo (rama principal), que ya soporta la G615 como `PAN_PROD_ID(11,8,3)` (MR 41803 y la corrección de MR 44111). Hay que pasar a ese Mesa los parches de Darwin y AIR de la referencia, que hoy están sobre 25.0.7 (`tools/mesa/` en hakios-xnu-kernel).
4. **El driver Metal de la referencia** (`userland/src/redwoodmetal.m`) encima de Mesa, con los boot-args que usa la referencia (`mali=1 metal=1 camtl=1`, ver `docs/metal.md`).

## Ideas del diseño que quedaron decididas

- Un address space de la GPU por cada VM (AS0 es del MCU). Cada VM toma un slot de AS mientras existe.
- Cada grupo toma un slot de CSG fijo mientras existe, sin rotación al principio.
- Los buffers de los programas son memoria del kernel write-combined, como en `RedwoodMali.cpp`, y se mapean al proceso con `clientMemoryForType`.
- Las tablas de páginas de las VMs van en memoria del kernel, con limpieza de caché después de escribir, como lo hace `redwood_mali.c` de la referencia (la GPU no es coherente).
- Las páginas de entrada y salida de cada cola tienen que estar en el address space del MCU. El ring buffer y los syncobjs de la cola van en la VM del grupo.
- Cada envío añade al ring buffer la misma secuencia que arma panthor (flush, CALL al stream del usuario, SYNC_ADD64 al syncobj de la cola).
- Sin interrupciones por ahora: un hilo del kernel que revisa los eventos (fallas, OOM del tiler) mientras haya trabajo pendiente.
- Para los doorbells 2 en adelante hay que mapear más registros (cada doorbell está en +0x80000 + 0x10000·n).

## Referencias

- User client de la referencia: `xnu/files/iokit/Kernel/RedwoodMali.cpp` y `xnu/files/bsd/dev/redwood_mali.c` en https://github.com/oosexgang/hakios-xnu-kernel (rama mtk). Ojo: `RedwoodMali` y `RedwoodMaliStart` ya existen en ese árbol, así que el nuevo necesita otros nombres.
- Linux panthor: `drivers/gpu/drm/panthor/` (`panthor_sched.c`, `panthor_mmu.c`, `panthor_heap.c`, `panthor_fw.h`) y la uAPI `include/uapi/drm/panthor_drm.h`.
- Fuentes del fabricante: https://github.com/mt6897-devs/android_kernel_device_modules_6.1, en `drivers/gpu/mediatek`.

## Continuación: user client etapa 1 (sin corrida de hardware)

Implementado `hakios_Panthor` con ABI explícita, BO_CREATE/BO_MMAP_OFFSET/BO_QUERY_INFO/GEM_CLOSE,
cuotas y mapeos CPU por conexión. Opt-in `mtkgpu_uc=1`, root; envío GPU todavía no soportado.
La terminación del grupo 0 ahora debe confirmarse antes de declarar éxito/publicar el servicio.
Ver `docs/panthor-user-client.md` para límites, pruebas y próxima etapa. La 25b sigue siendo la última prueba en teléfono.
