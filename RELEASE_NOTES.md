# Bigodão: WL SM3 — Release Notes

## Enemies beyond the screen edge, gyroscope camera and new defaults (v.1.6)

This release shows enemies outside the original Game Boy screen in widescreen and 3D, adds gyroscope control for the 3D camera, and changes some defaults so the game runs smoother out of the box.

---

### 👾 Enemies beyond the screen edge

* **Widescreen (2D):** enemies that are already active are now drawn on the widescreen sides at their real position. Before, they disappeared at the edge of the original 160-pixel screen or showed up as "ghosts" on the opposite side.
* **3D mode:** enemies waiting further away in the level, to the left, right, above or below, now appear in the 3D view as still images at their spawn position. They come to life as usual when Wario gets close.
* **The game logic is untouched:** spawning, enemy behavior and collision work exactly as on the Game Boy. Earlier attempts that widened the spawn range caused missing enemies, checkpoint doors that never appeared, flickering and spikes that fell too early, so that approach was dropped.
* New option **Settings > 3D > Enemies beyond the screen** (on by default) to turn these images on or off.
* **Fix:** bats and other enemies that pick their facing direction when they spawn no longer face the wrong way when they spawn near the right edge of the wide view.

### 🎮 Gyroscope camera (new)

* On **Android phones** and **controllers with a motion sensor** (DualShock 4, DualSense, Switch Pro…), tilting the device turns and tilts the 3D camera. It eases back to center when you hold still.
* It **works together with the right analog stick** and with the camera that follows Wario.
* Settings in **3D > Camera**: on/off (on by default), sensitivity, and invert turn / tilt. The menu shows which sensor was found: the controller or the device itself.

### ⚙️ New defaults

* **V-Sync is now on by default on Android.** Without it, the 3D mode ran at about 56 fps on phones. With it, it holds a steady ~60 fps. On PC it stays off, and the game keeps pacing itself at the Game Boy's 59.7 fps.
* **Internal resolution now defaults to 1x** on every platform (it was 4x). It is lighter, especially in 3D. HD tile packs still need 2x or more, and the Mods menu tells you when to raise it.

### 📖 Documentation

* New **Controls** section in the README with the default keyboard and controller bindings.
* The gyroscope camera is described in the README's 3D Mode section.

### 📌 Notes

* Existing users keep their saved internal resolution. The new defaults apply to new installs and to **Reset display**.
* Known issue: on the Steam Deck, 3D mode with a high internal resolution (4x) can stutter. Use 1x or 2x in 3D for now. An optimization is planned.

---

## GPU renderer, 3D mode and new settings menu (v.1.5)

This release moves the screen composition to the GPU, adds an optional **3D mode** that turns the Game Boy screen into real depth layers, and replaces the settings menu with a new one in English and Portuguese.

---

### 🖥️ GPU renderer

* **The screen is now composed by the GPU (OpenGL)** by default: center, widescreen sides, HD tiles and filters. It looks the same as the CPU path and runs lighter on Android and Steam Deck.
* **Post-processing filters** (LCD Grid, Scanlines, Scale2x, HQ2x, xBRZ, Pixel-Perfect) now also run on the GPU.
* **120 fps mode** (in-between frames) works together with the GPU screen. The game still runs at 60: physics and music keep their speed.
* New install defaults: OpenGL video, 4x internal resolution, Linear filter and fullscreen.
* **Widescreen sides without scenery** (title, COURSE, boss, map transitions) can be black, a blurred edge or a blurred center.

### 🧊 3D mode (new)

Turn it on in **Settings > 3D > 3D mode: Layers**. Needs the OpenGL video and Screen composition: GPU.

* **Real volume in the levels:** solid blocks are extruded, water is a volume with the wave surface repeated in depth, doors and ladders get their own plane, and sprites (Wario, enemies, barrels) are extruded too.
* **The level continues beyond the screen** while the camera is tilted or turned, with HD tiles there too.
* **Front layer:** a copy of the terrain blocks in front of everything, with a black outline, for a "new layer" look. Breakable and item blocks are left out.
* **Camera:**
  * tilt (also from below), turn and depth settings;
  * right analog stick moves the camera and springs back when released;
  * the camera follows Wario, with no jerk when he reaches the top of the screen;
  * the turn mirrors to the side Wario is walking to.
* **Adjustable sprite thickness and front layer distance.**
* **The HUD stays fixed and flat** on top, and the level tiles behind it are kept in the 3D layers.
* **3D outside the levels** (title, save slots, maps, Course Clear, bonus games, treasure room, Time Up, game over):
  * tiles are marked by hand as floor, half floor, water or door/ladder, with a built-in editor (click on the game) and a **Freeze the game** button for scenes that can't be paused;
  * marks are kept per screen (`g00`, `m44`, `g80`–`g85`…) in `mods/3d_marcas.txt`, and a default set of marks ships with the game;
  * on the map screens, sprites are extruded like in the levels.
* The 3D view always uses the Nearest filter (Linear caused artifacts).
* Many fixes for water (dithered skies, still water with dotted bodies), coins, plants, bumped blocks and caves.

### ⚙️ New settings menu

* **Category list on the left** (Display, 3D, Audio, Controls, Game, Mods, Developer) with the settings grouped on the right. The menu remembers the last category opened.
* **Language:** English (default) or Portuguese, chosen at the top of the menu.
* **Menu themes with Wario's colors:** Wario classic (default), Gold, Cream (light), Yellow (light) and the Original dark blue.
* The window title now reads **"Bigodão - Wario Land: Super Mario Land 3"**.

### 📌 Notes

* Settings are saved in `runtime_prefs.ini` as before. Existing users keep their saved 3D values; the new defaults apply to new installs and to **Reset display**.
* 3D mode and the GPU renderer need OpenGL 3.3 (or OpenGL ES 3.0 on Android). Without it the game falls back to the CPU renderer in 2D.
