# Nightmare Lab

A multiplayer 3D game built with DirectX 12 and a custom C++ framework.
Originally developed as a graduation project.

> **Original repository:** https://github.com/rniman/Nightmare_Lab
>
> This fork focuses on incremental code cleanup, refactoring,
> and performance improvements — not feature additions.

---

## Tech Stack

| Category | Details |
|---|---|
| Language | C/C++ (C++20, MSVC v143) |
| Graphics | DirectX 12, HLSL (Shader Model 5.1) |
| Audio | FMOD |
| Platform | Win32 API |
| Networking | TCP (WsaAsyncSelect) |

## Project Structure

```
Client/
├── Client/          # Game client
│   ├── *.cpp / *.h  # Engine, gameplay, rendering
│   ├── *.hlsl       # Vertex / Pixel / Compute shaders
│   ├── cso/         # Compiled shader objects
│   ├── Asset/       # Textures, models, etc.
│   └── Sound/       # FMOD library (inc/, lib/)
└── Server/          # Game server
    └── *.cpp / *.h  # Networking, server-side game logic
```

## Goals

**Primary** — Code cleanup and refactoring to improve readability,
maintainability, and system boundaries.

**Secondary** — Frame rate optimization.

## Build

- Visual Studio 2022 (v143 toolset)
- Windows SDK 10.0
- Configuration: x64 Debug / Release

## Documentation

See the `docs/` directory for architecture notes, coding conventions,
and known issues. See `AGENTS.md` for AI-assisted development guidelines.