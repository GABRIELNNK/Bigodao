# Bigodão: WL SM3 — Release Notes

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
