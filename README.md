#  Bigodão: WL SM3

> **A static recompilation study and native PC port of *Wario Land: Super Mario Land 3* with native widescreen support.**

[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows-blue)](https://github.com)
[![License](https://img.shields.io/badge/Status-Research%20%2F%20Study-orange)](#-about-the-project)

---

## 📌 About the Project

**Bigodão: WL SM3** is a personal study project focused on **static decompilation and recompilation** techniques applied to classic Game Boy hardware binaries. 

By extracting and recompiling game code into modern C/C++, this project enables *Wario Land: Super Mario Land 3* to run natively on Linux and Windows with enhanced performance and **native widescreen rendering** without standard emulator stretched distortion.

---

## 🔑 Requirements & Hashes

To build and run this port, you must provide your own legally acquired Game Boy ROM and patch file matching the SHA-256 hashes below:

### 🎮 ROM & Patch Information

| Component | File Name | Version | SHA-256 Hash |
| :--- | :--- | :--- | :--- |
| **Base ROM** | `Wario Land - Super Mario Land 3 (World)` | Original | `ac1682f17abcf590311a233289ee325214c2d71ab3a5aa175004002d85075e56` |
| **IPS Patch** | `Wario Land - Super Mario Land 3 DX (World).ips` | v1.2 (by **korxo**) | `599d9f59090df3e1e77c1678e069290a2cd9ce59c71cf58d66c4dc3db8b7facd` |

> ℹ️ **How memory patching works:**  
> The executable validates the base ROM via its SHA-256 hash and applies the IPS patch (defined in `tools/wl_sm3_dx_v12_patch.json`) directly **in memory**. Your original ROM on disk remains untouched.  
> 
> *When packaging binary releases, keep the executable, `tools/wl_sm3_dx_v12_patch.json`, and the IPS patch together in the same folder.*

---

## 🛠️ Build Instructions

### 📋 Prerequisites

Before starting, ensure you have installed:
* 🛠️ **C++ Compiler** (GCC / Clang / MSVC)
* ⚙️ **CMake** (v3.15+)
* ⚡ **Ninja** build system
* 🐍 **Python 3**

---

### 1️⃣ Step 1: Extract & Recompile Game Code

From the project root directory, build the recompilation tool and process the game source:

```bash
# 1. Build the gb-recompiled tool
cmake -G Ninja -S gb-recompiled -B gb-recompiled/build
ninja -C gb-recompiled/build

# 2. Recompile the game ROM into C source files
gb-recompiled/build/bin/gbrecomp hack1.2_WL-SM3.gb -o compiler_src/game_code/game_code_hack
```

---

### 2️⃣ Step 2: Native Linux Build

To compile the native executable on Linux:

```bash
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

> 💡 *Note: The build process automatically applies the widescreen FOV patch (`tools/widescreen_hack_fov_30_patch.json`) during CMake configuration.*

---

### 3️⃣ Step 3: Windows Cross-Compilation (MinGW)

To cross-compile for Windows from a Linux environment:

1. **Download and prepare SDL2 dependencies:**
   ```bash
   cd compiler_src
   wget https://github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-devel-2.30.0-mingw.tar.gz
   tar -xvf SDL2-devel-2.30.0-mingw.tar.gz
   cd ..
   ```

2. **Configure & Build:**
   ```bash
   mkdir build_win
   cd build_win
   cmake -DCMAKE_TOOLCHAIN_FILE=../mingw-toolchain.cmake ..
   cmake --build .
   ```

3. **Copy Required Runtime Libraries:**
   Copy `libgcc_s_seh-1.dll` and `libstdc++-6.dll` from your MinGW distribution to the output folder next to the generated executable and SDL2 DLLs.

---

## 🙏 Credits & Acknowledgments

Without the work of these incredible projects and individuals, this project wouldn't be possible:

* 🛠️ [**gb-recompiled**](https://github.com/arcanite24/gb-recompiled) by [arcanite24](https://github.com/arcanite24) – Static recompilation toolsuite for Game Boy binaries.
* 🔍 [**WL Disassembly**](https://github.com/Kak2X/wl) by [Kak2X](https://github.com/Kak2X) – Reverse engineering and disassembly of *Wario Land*.
* 🎨 **korxo** – Special thanks for the fantastic work on the Version 1.2 IPS patch.