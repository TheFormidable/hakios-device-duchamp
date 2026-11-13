# hakios-device-duchamp

[English](README.md) · Español

HakiOS para el **POCO X6 Pro 5G (duchamp)**, que también se vende como Redmi K70E: MediaTek Dimensity 8300 Ultra
(**MT6897**), GPU **Mali-G615 MC6**, almacenamiento UFS. Es un repositorio de dispositivo según la guía del proyecto
([Adapting HakiOS to your device](https://hackios-web.vercel.app/en/guide/your-device)), hecho a partir del primer port
MediaTek, [hakios-mt6768](https://github.com/oosexgang/hakios-mt6768).

**Estado: todavía no se ha arrancado en el teléfono.** Lo que está hecho: el loader del MT6768 adaptado a las direcciones de
este SoC (compila), una herramienta para la boot.img de cabecera v4 (GKI) de este teléfono, los scripts que leen del
teléfono lo que su device tree no dice, y la capa de dispositivo que `tools/hakios-device.py check duchamp` acepta.
[docs/porting-mt6897.md](docs/porting-mt6897.md) tiene el mapa de direcciones, qué cambia respecto al MT6768 y el orden de
trabajo.

## Qué cambia respecto al MT6768

- GIC en `0x0c400000`/`0x0c440000`, UART en `0x11001000`, watchdog en `0x1c00a000`, teclado en `0x1c00e000`.
- Almacenamiento **UFS** (no eMMC): el lector MSDC del loader de referencia no sirve; de momento la raíz es el RAM disk.
- GPU **Mali-G615** (Valhall con CSF, alimentada por GPUEB): el arranque de la Mali-G52 no sirve; en Mesa es panthor.
- PMIC por **SPMI** (MT6363 + MT6368), no PWRAP.
- Táctil FocalTech FT3683G o Goodix por SPI2, no el TD4160.
- boot.img **cabecera v4** con `Image.lz4` y sin direcciones: el loader se recoloca solo y reinicia Android en un límite de 2 MB.
- El framebuffer y ramoops de LK se leen del device tree de LK en cada arranque, en vez de estar fijos.

## En Windows

Doble clic en `windows\hakios-duchamp.cmd` (con el teléfono conectado, depuración USB y Magisk). Hace la copia de seguridad
(`backup\`) y el sondeo (`logs\probe\`), sin bash ni Python, y deja `hakios-probe.zip` para enviarlo. Solo lee el teléfono.

`windows\hakios-paso2.cmd`: flashea la imagen de la cadena (paso 2), pide un arranque y trae el registro; opción 3 restaura.

## En macOS (paso 3)

- `mac/hakios-kernel.sh` compila el kernel XNU (árbol común, rama `mtk`) con las Command Line Tools y deja
  `~/Desktop/hakios-kernel-duchamp.zip`. No toca el teléfono.
- `tools/mkimage.sh` (`XNU_KERNEL=` ese kernel) arma `xnu-flashed-slot_a.img`: el loader, el kernel y `hello` como disco en RAM.
- `mac/hakios-paso3.sh`, en la carpeta de la imagen y `backup/boot_a.img`: 1 flashea, 2 arranca XNU una vez y trae el
  registro a `logs/`, 3 restaura, 4 solo trae el registro.

## Uso (cuando estén comprobados los valores [phone] de platform.h)

```sh
export HAKIOS_SERIAL=...                 # adb devices
tools/backup-boot.sh                     # copia de boot_a y demás: así se restaura el teléfono
tools/duchamp-probe.sh                   # logs/probe/summary.txt -> mtkloader/platform.h
HAKIOS_ROOT=../.. mtkloader/build.sh     # mtkloader.bin
python3 mtkloader/build_flashed.py       # xnu-flashed-slot_a.img (sin payload: solo la cadena, paso 2)
XNU_KERNEL=... tools/mkimage.sh          # paso 3: loader + kernel + hello (md0) -> xnu-flashed-slot_a.img
XNU_SECS=45 XNU_ARGS="cpus=1" tools/xnu-cycle.sh     # lo mismo, y además flashea, arranca XNU una vez y trae el log
```

Solo se flashea `boot_a`. Nunca el slot b ni otra partición. Si el teléfono no vuelve: power + volumen abajo para fastboot y
`fastboot flash boot_a backup/boot_a.img`.

## Licencias

`kernel/` (aún vacío) es APSL 2.0; todo lo demás es MIT. Ver `NOTICE` y `LEGAL.md`.
