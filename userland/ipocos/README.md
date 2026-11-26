# Programas de iOS importados de iPocOS

De [iPocOS](https://github.com/SuperProgramer64/iPocOS) (commit `7f0308f`, `ios16/userland/` y `tools/`), el port de
SuperProgramer64 al mismo teléfono. Solo los archivos con `SPDX-License-Identifier: MIT`, copiados sin cambios. Ninguno
se compila todavía con los scripts de este repositorio: son para meterlos en la imagen del sistema a mano.

| Carpeta | Qué es |
| --- | --- |
| `log/` | `say_printf`: lo que imprime un programa (y el stderr de Mesa) va también al log que vuelve del teléfono (`kern.hakios.say`) |
| `perf/` | `cpu_top` (procesos e hilos que más CPU gastan), `perf_scan` (escáner de log acotado) |
| `ptp/` | `hakios_ptpd`: el carrete por USB como cámara PTP, de solo lectura (Fotos, Explorador, gphoto2) |
| `system/` | `hal_hide` (esconde los plug-ins HAL de Apple que gastaban CPU), `telephony_quiet` (calla el reintento de CommCenter de SpringBoard) |
| `gpu-tests/` | pruebas de la G615 desde iOS: `csf_probe`, `vk_fill_test`, `metal_fault_log`, `metal_discovery_test`, `m2v_*` |
| `usbmux/` | `usbmux_handshake.h`: recuperar la trama a medias tras reconectar el host |

No se trajo: `panthor-drm/` y `bench/mac_bench.c` (user client), `triangle/triangle_spv.h` y por eso
`vk_triangle_test.c` (sin licencia), ni nada del loader, el selector de dual boot o el instalador (sin licencia).
Ver [NOTICE](../../NOTICE).
