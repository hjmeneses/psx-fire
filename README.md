# PSX Fire

*English below.*

Fuente para OBS Studio con el fuego de la pantalla de título del DOOM de PlayStation, simulado en tiempo real con el algoritmo y la paleta de 37 colores originales, tal como los documentó Fabien Sanglard en [“How DOOM fire was done”](https://fabiensanglard.net/doom_fire_psx/).

- La simulación corre en la CPU, un byte por celda; la paleta, la transparencia y el escalado los aplica un shader en la GPU.
- Fondo transparente con fundido configurable, para ponerlo encima de la cámara o de cualquier escena.
- Viento, enfriamiento, pasos por segundo y tamaño de pixel.
- Encender y apagar con apagado gradual, reencender desde cero, y hotkeys para ambos.
- Opción **Orden clásico (PSX)** para recorrer la cuadrícula exactamente como el código original.
- No simula nada mientras la fuente no se ve.

**Estado:** versión 0.1.0, probada en OBS 33 en Windows.

## Instalación (Windows)

Copia la carpeta `psx-fire` (con `bin\64bit` y `data`) a `C:\ProgramData\obs-studio\plugins\` y reinicia OBS. En una escena: **+ → Fuego PSX**.

## Compilar

Requiere CMake 3.28+ y Visual Studio 2022 con C++.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --install build_x64 --config RelWithDebInfo --prefix release
```

---

## English

An OBS Studio source with the fire from the title screen of the PlayStation port of DOOM, simulated in real time with the original algorithm and 37-colour palette, as documented by Fabien Sanglard in [“How DOOM fire was done”](https://fabiensanglard.net/doom_fire_psx/).

- The simulation runs on the CPU, one byte per cell; palette, transparency and scaling are applied by a GPU shader.
- Transparent background with an adjustable fade, so it can sit over a camera or any scene.
- Wind, cooling, steps per second and pixel size.
- Turn on/off with a gradual die-down, reignite from scratch, and hotkeys for both.
- **Classic order (PSX)** option to traverse the grid exactly like the original code.
- No simulation work while the source is not visible.

**Status:** version 0.1.0, tested on OBS 33 on Windows.

**Install (Windows):** copy the `psx-fire` folder (with `bin\64bit` and `data`) to `C:\ProgramData\obs-studio\plugins\` and restart OBS. In a scene: **+ → PSX Fire**.

**Build:** see the commands above (CMake 3.28+, Visual Studio 2022 with C++).

Made with Claude (AI) as a coding partner, designed and tested by the author.

## License

GPL-2.0-or-later, like OBS Studio. See [LICENSE](LICENSE).
