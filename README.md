# Farming Simulator 12 Multiplayer Mod

This project is a native Android mod developed to add multiplayer support to the **Android version of Farming Simulator 12 (FS12)**.

Instead of directly modifying the game's APK file, the project interfaces with specific game functions at runtime via a native **`.so` shared library** running on Android. The main objective of the mod is to hook specific menu and gameplay functions of the single-player engine to build a multiplayer infrastructure capable of exchanging data between players over a network.

> **Important:** This project is specifically tailored for legacy Android devices and legacy Android ABI architectures. It must NOT be updated arbitrarily according to modern Android/NDK settings.

---

## Current Project Status

The GitHub Actions-based build system is fully operational.

When the source code is hosted on GitHub, the workflow sets up the required compilation environment and produces:

```text
libmultiplayermod.so
```

The compiled `.so` file has been tested on a real physical device and **successfully loads and runs** when properly injected into the game.

Therefore, preserving the current build system and target architecture in future modifications is critically important.

---

# Technical Target

The build setup of this project is prepared specifically around legacy Android ABI support.

Core Target:

```text
Architecture : ARMv5 / armeabi
Android API  : 15
NDK          : Android NDK r16b
C++          : GNU++11
STL          : GNU libstdc++
```

The target for this project MUST be:

```text
armeabi
```

It should NOT be replaced with:

```text
armeabi-v7a
arm64-v8a
x86
x86_64
```

In particular, **ARMv5** support is essential for compatibility with this legacy game.

Although ARMv5/armeabi was considered deprecated during the NDK r16 era, it could still be compiled by explicitly defining it. Since this support was removed in later NDK releases, an arbitrary upgrade to modern NDKs must not be made.

---

# Source Code

Main native source file:

```text
yeni.cpp
```

This file contains the core code of the project.

The code is written in C++ and compiled as a native shared library via the Android NDK.

The source code utilizes Android JNI, Android input system, OpenGL ES, EGL, POSIX socket APIs, multi-threading, and a runtime hooking mechanism.

Primary Android/native components utilized:

```cpp
jni.h
android/log.h
android/input.h
dlfcn.h
GLES2/gl2.h
EGL/egl.h
```

Additionally, standard Linux/POSIX socket structures are used on the networking side.

---

# Hook System

One of the most critical sections of the project is **hooking** the existing native functions of the game.

For this purpose, the project uses:

```cpp
#include "Substrate.h"
```

By retrieving the base address of the game's native library loaded in memory, target functions are hooked via specific function offsets.

Key memory addresses used in the current source code:

```cpp
renderMenuAddr = libBase + 0x00033974 + 1;
updateGUIAddr  = libBase + 0x0002f6a0 + 1;
gameUpdateAddr = libBase + 0x00057ee8 + 1;
inGameMenuAddr = libBase + 0x00032090 + 1;
```

These addresses are then hooked using `MSHookFunction`.

Additionally, an input hooking mechanism is implemented for the Android input system via:

```cpp
AInputQueue_getEvent
```

These offsets are tied to the current version of the game.

### Critical Note

These addresses MUST NOT be changed arbitrarily.

These hardcoded offsets:

```text
0x00033974
0x0002f6a0
0x00057ee8
0x00032090
```

correspond directly to the native binary structure of the target game.

When adding new features, existing hook addresses must be preserved.

If migrating to a different version of the game, offsets must not be altered without re-analyzing the binary.

---

# Multiplayer Infrastructure

The main C++ codebase contains the basic multiplayer/networking infrastructure.

The networking system operates using two main components.

## UDP Discovery

A UDP discovery system is used to find players/hosts on the LAN.

Discovery configuration:

```text
UDP Port: 8888
```

Inside the code, discovery packets use messages such as:

```text
FS14_PING
FS14_PONG
```

These are protocol names from the current codebase. If cleaned up in the future, changes must be made carefully without breaking the network protocol.

---

## TCP Synchronization

A TCP connection is also used for core data synchronization between players.

Used port:

```text
TCP Port: 8889
```

Packet structures are defined within the network system, and data is transmitted and received over TCP.

POSIX threads are utilized to execute networking tasks on a separate thread.

Thanks to this multi-threaded structure, rendering/update operations of the game loop remain isolated from network operations.

---

# GUI System

The mod includes an in-game multiplayer overlay UI.

For the interface, the mod utilizes:

```text
Dear ImGui
```

With the OpenGL ES 2 backend:

```text
imgui_impl_opengl3.cpp
```

Since the project runs on Android, the GUI side is adapted to the OpenGL ES context used by the game rather than desktop OpenGL.

Main ImGui files:

```text
imgui/
├── imgui.cpp
├── imgui.h
├── imgui_draw.cpp
├── imgui_tables.cpp
├── imgui_widgets.cpp
└── backends/
    ├── imgui_impl_opengl3.cpp
    ├── imgui_impl_opengl3.h
    └── imgui_impl_opengl3_loader.h
```

The version of these files must be kept compatible with the existing project setup.

**ImGui files must NOT be updated to newer versions automatically.**

API breaking changes and ABI/compatibility issues may occur.

---

# Android GUI / JNI

JNI is used within the code for communication with the Android Java layer.

Various operations on the Android side are performed via JNI, including:

* Android UI interactions
* Soft keyboard inputs
* Toast-like notifications
* Invocations to Java methods

Therefore, when modifying native code, JNI calls must remain compatible with Android API level 15.

---

# Input System

The mod intercepts Android's native input system.

Primary API used:

```cpp
AInputQueue_getEvent
```

Through this mechanism, Android touch, keyboard, and input events are captured and routed into the ImGui interface.

Because input behavior in legacy Android versions can differ from modern Android, existing input handling logic must be preserved when modifying code.

---

# OpenGL

The mod connects to the game's rendering context via function hooks.

Graphics APIs utilized:

```text
OpenGL ES 2
EGL
```

Headers included:

```cpp
#include <GLES2/gl2.h>
#include <EGL/egl.h>
```

Although ImGui's OpenGL3 backend file is used here, it is configured specifically for the OpenGL ES environment on Android.

---

# Texture System

For custom buttons and images within the GUI, the project contains:

```text
buton_texture.h
```

In addition, for image loading operations:

```text
stb_image.h
```

is used.

This pipeline is responsible for loading and rendering graphical UI elements inside the multiplayer menu.

---

# Multiplayer Menu

New multiplayer control panels are integrated into the existing game interface via hooked menu functions.

A major goal is embedding multiplayer options natively inside the game's menu system.

On the GUI side, infrastructure exists to manage:

```text
Host
Client
LAN discovery
Connection
Player info
Network status
```

The UI rendering logic executes directly inside the native render/update loop of the game.

---

# Game Update Hook

One of the main game update loops is hooked:

```cpp
gameUpdateAddr = libBase + 0x00057ee8 + 1;
```

This hook is crucial for synchronizing the multiplayer system with the main game loop.

Update hooks like this are used to process incoming network data or update multiplayer game states in lockstep with the engine.

---

# Menu Hooks

The project hooks into the following main menu and in-game functions:

```text
renderMenu
updateGUI
gameUpdate
inGameMenu
```

The goal is overlaying multiplayer functionality directly on top of the game's native GUI and update pipeline.

Hooking is implemented using Substrate.

---

# Network Packets

A packet structure is defined for networking.

The purpose of these packets is establishing a core protocol to exchange multiplayer state between client and host.

Packet size, layout, and alignment must be carefully preserved during development.

Key considerations include:

```text
struct
alignment
padding
integer size
byte order
```

Modifying packet structures on one side can cause protocol mismatch with other clients.

Therefore, if the network protocol is changed, both client and host code must be updated simultaneously.

---

# Threading

Networking tasks run on separate threads.

This design is essential to prevent socket blocking operations from stalling the game rendering/update thread.

General architecture:

```text
Game Thread
     |
     +---- GUI / Hook / Update
     
Network Thread
     |
     +---- UDP Discovery
     +---- TCP Communication
```

Care must be taken regarding race conditions on shared variables requiring thread safety.

---

# Current Automatic Game Transition

The code contains a client-side mechanism for transitioning into game mode.

However, in its current state, it simply notifies the user:

```text
"Transitioning to game. Please start the game manually."
```

Establishing a network connection is not the same as automatically transitioning the game state into a live multiplayer match.

In the future, if automatic match launching or full game-state synchronization is developed, native game state functions must first be thoroughly analyzed.

---

# Build System

The project uses GitHub Actions.

Workflow file location:

```text
.github/workflows/build.yml
```

The build pipeline sets up the legacy Android NDK environment and compiles the project using CMake.

Build pipeline flow:

```text
GitHub Repository
        ↓
GitHub Actions
        ↓
Android NDK r16b
        ↓
CMake
        ↓
ARMv5 / armeabi
        ↓
libmultiplayermod.so
```

The generated `.so` file can be downloaded as an Actions artifact and tested inside the Android game environment.

---

# CMake

CMake configuration file:

```text
CMakeLists.txt
```

Alongside the main C++ code, CMake links ImGui source files.

Primary build target:

```text
multiplayermod
```

Output artifact:

```text
libmultiplayermod.so
```

---

# Repository Structure

The layout of the project repository:

```text
fs12multiplayerolacak-/
│
├── yeni.cpp
├── buton_texture.h
├── stb_image.h
├── Substrate.h
├── CMakeLists.txt
│
├── imgui/
│   ├── imgui.cpp
│   ├── imgui.h
│   ├── imgui_draw.cpp
│   ├── imgui_tables.cpp
│   ├── imgui_widgets.cpp
│   ├── imconfig.h
│   ├── imgui_internal.h
│   ├── imstb_rectpack.h
│   ├── imstb_textedit.h
│   ├── imstb_truetype.h
│   │
│   └── backends/
│       ├── imgui_impl_opengl3.cpp
│       ├── imgui_impl_opengl3.h
│       └── imgui_impl_opengl3_loader.h
│
├── libs/
│   └── armeabi/
│       └── libsubstrate.so
│
└── .github/
    └── workflows/
        └── build.yml
```

---

# Critical Guidelines for AI and Developers

Any developer or AI assistant working on this repository MUST adhere to the following rules:

## 1. Do Not Break the Working Build System

The repository currently builds `libmultiplayermod.so` successfully.

Do NOT modify working configurations arbitrarily:

```text
build.yml
CMakeLists.txt
NDK settings
ABI settings
```

Always understand the existing build pipeline first.

---

## 2. Preserve ARMv5 Support

Target Architecture:

```text
ARMv5 / armeabi
```

Switching to `armeabi-v7a` or `arm64-v8a` breaks compatibility with the target game binary.

---

## 3. Do Not Upgrade NDK Version Arbitrarily

For compatibility with this legacy Android game, the project relies on:

```text
NDK r16b
```

Before migrating to a modern NDK, legacy ARMv5 toolchain requirements must be carefully evaluated.

---

## 4. Do Not Change Hook Offsets Arbitrarily

The following offsets are critical to this specific game build:

```text
0x00033974
0x0002f6a0
0x00057ee8
0x00032090
```

These correspond directly to specific native functions in the game's binary.

Do not attempt to update these offsets unless you have re-analyzed the game binary yourself.

---

## 5. Do Not Update ImGui Versions Arbitrarily

All ImGui source files must belong to the same compatible release family:

```text
imgui.cpp
imgui.h
imgui_internal.h
imgui_draw.cpp
imgui_tables.cpp
imgui_widgets.cpp
```

Updating only part of ImGui will introduce build breaks or runtime instability.

---

## 6. Preserve Substrate ABI Compatibility

The project uses:

```text
Substrate.h
libsubstrate.so
```

Specifically:

```text
libs/armeabi/libsubstrate.so
```

is essential for the target ABI. Do NOT replace it with arbitrary binaries.

---

# Development Workflow

When introducing new features, follow this recommended sequence:

```text
1. Inspect yeni.cpp
2. Identify hook targets
3. Analyze current networking protocol
4. Inspect GUI architecture
5. Analyze game update loop
6. Implement code changes
7. Build via GitHub Actions
8. Test generated libmultiplayermod.so
```

Note that compilation success does not guarantee runtime success:

```text
Compile Success
≠
Runtime Success
```

Always perform runtime verification on an actual Android device with the game injected.

---

# Future Roadmap

Potential future improvements:

```text
- Real player position synchronization
- Player custom names
- Vehicle state synchronization
- Full game state synchronization
- Host/client connection lobby
- LAN & Internet matchmaking
- Advanced server protocol
- Disconnect / Reconnect handling
- Packet verification & checksums
- Automated multiplayer match startup
- Enhanced ImGui overlay UI
```

All new features must maintain strict compatibility with ARMv5 / NDK r16b / Android API 15.

---

# Project Objective

The core objective of this repository is leveraging native reverse-engineering techniques to bring a modern multiplayer experience to a legacy Android game engine.

Key Approach:

```text
Game Native Binary
        ↓
Runtime Hooking
        ↓
Native Mod Shared Library
        ↓
GUI + Networking
        ↓
Multiplayer Infrastructure
```

This is not a standalone Android app—it is a native runtime mod.

---

# Verified Output

Successful build output:

```text
libmultiplayermod.so
```

When injected into the game process, this library loads and executes hooks, UI, and network threads.

After any code modifications, verify that:

```text
GitHub Actions → Build → libmultiplayermod.so
```

completes successfully, followed by real device verification.

---

# Contributing

Contributors are expected to respect the architecture and preserve compatibility:

```text
ARMv5 / armeabi
Android API 15
NDK r16b
Native Hook Offsets
```

---

# Repository Info

GitHub:

https://github.com/erensariisik03-sudo/fs12multiplayerolacak-

Git clone:

```bash
git clone https://github.com/erensariisik03-sudo/fs12multiplayerolacak-.git
```

---

## Final Note

This project is under active development. The `.so` compilation pipeline and native injection layer are working. Further progress requires deeper reverse engineering of the game engine's state structures and sync routines.

Before modifying critical code, review:

```text
README.md
CMakeLists.txt
.github/workflows/build.yml
yeni.cpp
```
