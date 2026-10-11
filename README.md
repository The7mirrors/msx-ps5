<p align="center"><img src="ps5/app/sce_sys/icon0.png" width="160" alt="MSX PS5 icon"></p>

<h1 align="center">MSX PS5</h1>

<p align="center"><b>An MSX emulator for the PS5, with a 3D game shelf.</b><br>
blueMSX at its heart; boxes, cartridges and floppies you can turn in your hands.</p>

---

## What it does

**MSX PS5** runs MSX, MSX2 and MSX2+ games on a jailbroken PS5 as a home-screen app, through the
[blueMSX](http://www.bluemsx.com/) emulator (its [libretro core](https://github.com/libretro/blueMSX-libretro)).

- **A 3D shelf** - every game stands on a glossy shelf as its **box**, its **cartridge** or its **floppy disk**
  (L3 switches the style). Boxes get a spine and a back made from the cover art, with the publisher, year and
  screenshots.
- **Turn it over** - L2 / R2 spin the game in front of you, with clicks in the controller and resistance in the
  triggers.
- **Blow on the cartridge** - swipe up on the touchpad to turn a cartridge to its contacts, then blow into the
  controller's microphone to clean them, just like back then.
- **Box art by itself** - covers, box backs and screenshots are downloaded from
  [libretro-thumbnails](https://github.com/libretro-thumbnails) (Square fetches one again).
- **Favorites** - Circle marks a game; the Favorites tab keeps them together.
- **Music and sounds** - ten original shelf tunes (ambient, chill, hip-hop, jazz and more) and CC0 sound effects,
  each with its own volume.
- **Playing** - CRT shaders and scanlines, screen size and aspect, save states, rewind (hold L2 + R2), fast
  forward (hold R2), the sprite limit removed, FM-PAC / MSX-MUSIC sound, controls for all games or for each game.

## Requirements

- A **jailbroken PS5** with a payload loader (for example the PS5 Payload Manager / `elfldr` on port 9021).
- **ShadowMountPlus** (or another homebrew registrar) so the installed app gets its home-screen tile.
- Your own MSX games (none are included).

## Install

1. Download **`MSXPS5-v1.0.zip`** from the [Releases](../../releases) page and unzip it to get **`MSXPS5.elf`**.
2. Send it with your payload loader. It installs the app to `/data/homebrew/PPSA99231` and stays running as the
   app's helper.
3. Copy your games to **`/data/msx/roms`** on the PS5 (with an FTP client or a web file manager):
   cartridges `.rom` `.mx1` `.mx2`, disks `.dsk`, tapes `.cas`, also inside `.zip`.
4. Open **MSX PS5** from the home screen (if the tile doesn't appear, let ShadowMountPlus rescan or restart the PS5).

After a reboot you don't need to send the payload again: the app sends its own helper to the ELF loader when
none answers.

### BIOS

Cartridge games run without any BIOS file: blueMSX falls back to the free **C-BIOS**. Disk and tape games need a
real MSX BIOS: copy blueMSX's **`Machines`** and **`Databases`** folders (with the BIOS ROMs you own) to
**`/data/msx/bios`**. This repository's `system/bluemsx` folder has the machine configurations and the C-BIOS
machines; the original BIOS ROMs are not included.

## Controls

| On the shelf | |
| --- | --- |
| Cross | play |
| D-pad left / right | browse (L1 / R1: ten at a time) |
| D-pad up / down | choose the system / Favorites |
| L2 / R2 | turn the game |
| L3 | boxes / cartridges / floppies |
| Triangle | settings |
| Square | download the cover again |
| Circle | favorite |
| Touchpad | cartridge: swipe up to see its contacts (then blow); floppy: swipe to slide the shutter |
| Options | quit |

In a game, press **L3 + R3** for the menu (resume, save / load state, reset, settings, back to the game list).
Hold **R2** to fast forward and **L2 + R2** to rewind. The MSX joystick and keys are mapped to the PS5 buttons in
Settings > CONTROLS, for all games or for one game.

## Build from source

Needs Linux (or WSL), the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) (v0.42 or newer) and
clang/lld 18:

```sh
cd ps5
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
# -> build/ps5/MSXPS5.elf
```

| Part | Where | What it is |
| --- | --- | --- |
| The emulator | `Src/`, `libretro.c` | blueMSX-libretro, with a few PS5 fixes marked "MSX PS5" |
| The app | `ps5/frontend/` | the shelf (`fe_shelf`, `fe_cart3d`, `fe_art`), menus, covers, sound and music |
| PS5 layer | `ps5/coreorbis/` | video, pad, microphone, audio, paths, crash log |
| Installer / helper | `ps5/installer/`, `ProsperoHelper.cpp` | installs the app and gives it access to `/data` |

The upstream core's own documentation is in [README-blueMSX-libretro.md](README-blueMSX-libretro.md).

## Credits

- **[blueMSX](http://www.bluemsx.com/)** by Daniel Vik and the blueMSX team, and the
  **[blueMSX-libretro](https://github.com/libretro/blueMSX-libretro)** core - the emulation.
- **[genplusgxPS5](https://github.com/MisterTemaki/genplusgxPS5)** by MisterTemaki - the PS5 front end this grew
  from (video, pad, text, menus, the installer/helper layout).
- **[ps5-native-app-boilerplate](https://github.com/blackbearreloaded)** by BlackBearReloaded - the native app
  build tools and startup code in `ps5/proto/native` (GPL-3.0).
- **[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)** - the toolchain and PS5 libraries.
- **[C-BIOS](https://cbios.sourceforge.net/)** - the free MSX BIOS (its license is in `system/bluemsx/Machines/*C-BIOS/cbios.txt`).
- **[libretro-thumbnails](https://github.com/libretro-thumbnails)** - box art and screenshots (downloaded, not included);
  game facts from blueMSX's msxromdb.
- **[Mbed TLS](https://github.com/Mbed-TLS/mbedtls)** (Apache-2.0) - HTTPS for the downloads.
- CRT shaders rewritten from libretro's [slang-shaders](https://github.com/libretro/slang-shaders) - see
  [ps5/THIRD_PARTY_SHADERS.md](ps5/THIRD_PARTY_SHADERS.md).
- Sound effects (CC0): [Kenney](https://kenney.nl) and freesound.org (micropolis, KRAFTWERK2K1, CrimsonGalaxies) -
  see `ps5/sounds/CREDITS.txt`. The shelf music is original.
- Fonts: **Roboto** (Apache 2.0), **PromptFont** by Yukari "Shinmera" Hafner (SIL OFL), **Font Awesome** brands
  (SIL OFL) - their licenses are in `ps5/frontend/assets/fonts`.

Made by **the7mirrors**, with the help of an AI assistant (Claude by Anthropic).

## License

The project as a whole is released under the **GNU General Public License v3.0 or later** (see [LICENSE](LICENSE)),
because the app links the GPL-3.0 startup code of ps5-native-app-boilerplate. The blueMSX sources keep their own
license (see [license.txt](license.txt)); the PS5 front end files carry MIT notices; the other components keep the
licenses named above.

*MSX PS5 is an unofficial homebrew project, not affiliated with or endorsed by Sony Interactive Entertainment or
MSX Licensing Corporation. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc.; "MSX" is
a trademark of MSX Licensing Corporation. No games or copyrighted BIOS files are included. Use at your own risk.*
