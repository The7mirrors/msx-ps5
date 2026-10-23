<p align="center"><img src="ps5/app/sce_sys/icon0.png" width="160" alt="MSX PS5 icon"></p>

<h1 align="center">MSX PS5</h1>

<p align="center"><b>An MSX emulator for the PS5, with a 3D game shelf.</b><br>
blueMSX at its heart; boxes, cartridges and floppies you can turn in your hands.<br>
Plug in a <b>real USB floppy drive</b> and your disks play like on the real thing.</p>

<p align="center"><img src="docs/screenshots/shelf-boxes.jpg" width="860" alt="The game shelf with boxes"></p>

<table align="center">
<tr>
<td><img src="docs/screenshots/shelf-cartridges.jpg" width="420" alt="Cartridges on the shelf"><br><sub>Cartridges</sub></td>
<td><img src="docs/screenshots/cartridge-blow.jpg" width="420" alt="A cartridge turned to its contacts"><br><sub>Blow on the contacts</sub></td>
</tr>
<tr>
<td><img src="docs/screenshots/shelf-floppies.jpg" width="420" alt="Floppy disks on the shelf"><br><sub>Floppy disks</sub></td>
<td><img src="docs/screenshots/in-game.jpg" width="420" alt="Knightmare II running"><br><sub>Playing</sub></td>
</tr>
</table>

---

## What it does

**MSX PS5** runs MSX, MSX2 and MSX2+ games on a jailbroken PS5 as a home-screen app, through the
[blueMSX](http://www.bluemsx.com/) emulator (its [libretro core](https://github.com/libretro/blueMSX-libretro)).

- **Real floppy disks (USB floppy drive)** - plug a USB floppy drive into the PS5 and put a disk in: the game on
  it **starts by itself**, no menu needed. Even from the home screen: the small `floppywatch` payload opens MSX PS5
  when a game disk goes in. Disks formatted on a PC work too - drop `.rom`, `.dsk`, `.cas` or `.zip` files on a
  1.44 MB floppy and each one appears on the **Floppy drive** shelf, drawn as a floppy and with its cover from your
  collection. MSX disks of their own (720 KB) are read whole. **Square** copies the disk's game into your
  collection, and asks before it overwrites a copy you already have. Nothing is ever written to the floppy.
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
- **The Arabic MSX collection** - games and programs from Al Alamiah, Barq and Al-Mithali get a **Sakhr** shelf of
  their own, with their own cartridges, opening clamshell boxes and the Arabic MSX2 machine.
- **Themes** - Cover glow, Sakhr blue, MSX classic, Synthwave and Al Alamiah backdrops behind the shelf.
- **Music and sounds** - ten original shelf tunes (ambient, chill, hip-hop, jazz and more) and CC0 sound effects,
  each with its own volume.
- **Playing** - CRT shaders and scanlines, screen size and aspect, save states, rewind (hold L2 + R2), fast
  forward (hold R2), the sprite limit removed, FM-PAC / MSX-MUSIC sound, controls for all games or for each game.

## Requirements

- A **jailbroken PS5** with a payload loader (for example the PS5 Payload Manager / `elfldr` on port 9021).
  Tested on firmware **13.00**.
- **ShadowMountPlus** (or another homebrew registrar) so the installed app gets its home-screen tile.
- Your own MSX games (none are included); an internet connection on the PS5 for the box art.
- Optional: a **USB floppy drive** for real disks (tested with a TEAC USB floppy drive). Plug it straight into the
  PS5, not through a hub; if it has two USB plugs, connect both.

## Install

1. Download the latest **`MSXPS5-v*.zip`** from the [Releases](../../releases) page and unzip it. Inside are
   **`MSXPS5.elf`** (the app), **`floppywatch.elf`** (the floppy watcher, optional) and a **`bios`** folder.
2. Send `MSXPS5.elf` with your payload loader. It installs the app to `/data/homebrew/PPSA99231` and stays running
   as the app's helper.
3. Open **MSX PS5** once from the home screen (if the tile doesn't appear, let ShadowMountPlus rescan or restart the
   PS5). The app makes its folders in **`/data/msx`**: `roms`, `bios`, `covers`, `saves`, `states`...
4. Copy the BIOS and your games (below), then open MSX PS5 again.

After a reboot you don't need to send the payload again: the app sends its own helper to the ELF loader when
none answers.

## Setup

### Copying files to the PS5

Use any way you already have to put files in `/data` on your PS5, for example:

- **FTP** - load an FTP server payload on the PS5, then connect from your computer with an FTP client such as
  FileZilla (host: the PS5's IP address, port: the one the payload shows, usually 2121).
- **A web file manager** payload - open its address in your computer's browser and upload from there.

### 1. BIOS (do this first)

Copy the **contents** of the zip's `bios` folder (the `Machines` and `Databases` folders) to **`/data/msx/bios`**:

```
/data/msx/bios/Machines/...
/data/msx/bios/Databases/...
```

That is enough for **cartridge games**: they run on the free **C-BIOS** that comes with it.

**Disk (`.dsk`) and tape (`.cas`) games** need a real MSX BIOS, which can't be shared here. Put the BIOS ROMs you
own in the matching machine's folder, `/data/msx/bios/Machines/<machine name>/` (its `config.ini` names the files
it needs), or copy the `Machines` folder of a blueMSX installation that has them over the zip's one.
Settings > EMULATION > **MSX machine** chooses MSX, MSX2, MSX2+ or MSX turbo R (Auto picks one by the game).

### 2. Games

Copy your games to **`/data/msx/roms`** (subfolders are fine):

- cartridges: `.rom` `.mx1` `.mx2`
- disks: `.dsk`
- tapes: `.cas`
- any of them inside a `.zip`

The shelf lists them by name; games are recognised by their contents, so file names don't have to be exact.

### 3. Covers

Box art, box backs and screenshots are **downloaded by themselves** from
[libretro-thumbnails](https://github.com/libretro-thumbnails) - the PS5 only needs an internet connection.

- Settings > LIBRARY > **Download covers** turns it on or off.
- On the shelf, **Square** downloads the selected game's cover again.
- Downloaded art is kept in `/data/msx/covers`, so it is fetched only once.
- **Your own cover:** put a `.png` or `.jpg` with the same name as the game file in **`/data/msx/covers`**
  (for example `/data/msx/covers/Knightmare.png` for `Knightmare.rom`), or next to the game file.
  Your own art always wins over a download.

### 4. Floppy disks

- **A PC-formatted floppy** (1.44 MB, FAT): copy game files onto it on your computer - `.rom`, `.mx1`, `.mx2`,
  `.dsk`, `.cas` or `.zip`, folders are fine. Naming a file like the game in your collection (for example
  `Penguin Adventure.rom`) gives it that game's cover.
- **An MSX disk** (720 KB, made on an MSX) is read whole, as a disk image.
- Disks sold as **"2HD Mac"** come formatted for a Macintosh: format them on a PC first (File Explorer: right-click
  the floppy > Format > FAT).
- **Starting from the home screen:** send `floppywatch.elf` with your payload loader, and add it to the loader's
  autoload list so it runs after every restart. When a disk with a game goes in, it opens MSX PS5 (not over another
  game or app that is open: you get a notification instead). Inside MSX PS5 the game starts by itself either way.
- The game starts once per disk: quit it and you get the shelf; take the disk out and put it back to start it again.

## Controls

| On the shelf | |
| --- | --- |
| Cross | play |
| D-pad left / right | browse (L1 / R1: ten at a time) |
| D-pad up / down | choose the system / Favorites / Floppy drive |
| L2 / R2 | turn the game |
| L3 | boxes / cartridges / floppies |
| Triangle | settings |
| Square | download the cover again (on the Floppy drive shelf: add the disk's game to your collection) |
| Circle | favorite |
| Touchpad | cartridge: swipe up to see its contacts (then blow); floppy: swipe to slide the shutter |
| Options | quit |

In a game, press **L3 + R3** for the menu (resume, save / load state, reset, settings, back to the game list).
Hold **R2** to fast forward and **L2 + R2** to rewind. The MSX joystick and keys are mapped to the PS5 buttons in
Settings > CONTROLS, for all games or for one game.

**Blowing on a cartridge** uses the controller's built-in microphone, so it has to be on: if the mic button's
light is orange, press it once (the bottom line on the screen tells you).

## Updating

Send the new `MSXPS5.elf` the same way. Your games, BIOS, covers, settings, saves (`/data/msx/saves`) and save
states (`/data/msx/states`) are kept.

## Troubleshooting

- **"No games found"** - the games aren't in `/data/msx/roms`, or have an extension the shelf doesn't list.
- **A disk or tape game asks for the MSX BIOS** - see [BIOS](#1-bios-do-this-first): those need the real BIOS ROMs.
- **No covers** - check the PS5's internet connection and Settings > LIBRARY > Download covers; a few games simply
  have no art in libretro-thumbnails (add your own).
- **The floppy isn't seen** - plug the drive straight into the PS5 (no hub) with both plugs if it has two; a disk
  formatted for a Mac must be formatted on a PC first; an old disk may simply be worn out.
- **The tile isn't on the home screen** - let ShadowMountPlus rescan, or restart the PS5 and send the payload again.
- **Something else** - the app writes its log to `/data/msx/logs/boot.log`; please attach it to an
  [issue](../../issues).

## Build from source

Needs Linux (or WSL), the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) (v0.42 or newer) and
clang/lld 18:

```sh
cd ps5
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
# -> build/ps5/MSXPS5.elf
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk bash tools/floppywatch/build.sh
# -> tools/floppywatch/floppywatch.elf
```

| Part | Where | What it is |
| --- | --- | --- |
| The emulator | `Src/`, `libretro.c` | blueMSX-libretro, with a few PS5 fixes marked "MSX PS5" |
| The app | `ps5/frontend/` | the shelf (`fe_shelf`, `fe_cart3d`, `fe_art`), menus, covers, sound and music |
| PS5 layer | `ps5/coreorbis/` | video, pad, microphone, audio, paths, crash log |
| Installer / helper | `ps5/installer/`, `ProsperoHelper.cpp` | installs the app and gives it access to `/data` |
| Floppy drives | `ps5/frontend/fe_floppy.c`, `ps5/tools/floppywatch/` | reads USB floppies (raw MSX disks, PC FAT12/16 disks); the watcher payload |

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
