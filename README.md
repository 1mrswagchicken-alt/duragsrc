# Durag Executor

<p align="center">
  <img src="https://duragexec.vercel.app/favicon.ico" alt="Durag logo" width="96">
</p>

<p align="center">
  <strong>Durag — The Vortex Executor</strong><br>
  A lightweight C++ DLL project focused on reliability, maintainability, and a clean codebase.
</p>

<p align="center">
  <a href="https://github.com/1mrswagchicken-alt/duragsrc/stargazers">
    <img src="https://img.shields.io/github/stars/1mrswagchicken-alt/duragsrc?style=for-the-badge" alt="Stars">
  </a>
  <a href="https://github.com/1mrswagchicken-alt/duragsrc/network/members">
    <img src="https://img.shields.io/github/forks/1mrswagchicken-alt/duragsrc?style=for-the-badge" alt="Forks">
  </a>
  <a href="https://github.com/1mrswagchicken-alt/duragsrc/issues">
    <img src="https://img.shields.io/github/issues/1mrswagchicken-alt/duragsrc?style=for-the-badge" alt="Issues">
  </a>
  <a href="https://github.com/1mrswagchicken-alt/duragsrc/blob/main/LICENSE">
    <img src="https://img.shields.io/github/license/1mrswagchicken-alt/duragsrc?style=for-the-badge" alt="License">
  </a>
</p>

---

## About

**Durag** is a C++ project built around a native DLL architecture for **Vortex**.

The project is based on the concepts and documentation published through the Durag documentation site and project website:

- **Documentation:** https://durag.gitbook.io/durag-docs
- **Website:** https://duragexec.vercel.app/

According to the project site, Durag is designed as a lightweight DLL and includes functionality such as movement controls, entity interaction, and signature-based resilience across updates.

> **Note:** This repository is an independent project and is not affiliated with, endorsed by, or officially connected to Vortex or its developers.

---

## Features

### Movement

- Fly / noclip
- Camera-relative movement
- Adjustable fly speed
- Pitch-aware vertical movement
- Configurable speed multiplier

### Entity Tools

- Entity/player tracking
- Adjustable orbit radius
- Adjustable orbit speed
- Teleport to coordinates
- Teleport to the nearest player

### Architecture

- Native C++ DLL
- Signature-scanning based implementation
- Modular source/header layout
- MinHook integration
- Lua 5.4 integration

The public project site describes the DLL as using hardware breakpoints, Winsock packet interception, and DXGI `Present` hooking.

---

## Project Structure

```text
durag_executor/
├── bin/                    # MinHook runtime/build assets
├── include/                # MinHook headers
├── lib/                    # MinHook libraries
├── lua54/                  # Lua 5.4 files
├── x64/                    # Visual Studio build output
│
├── durag_executor.cpp      # Main implementation
├── durag_script.cpp        # Script functionality
├── durag_script.h
├── script_api.h
├── sig_scanner.h
├── hwid_spoof.h
│
├── durag_executor.vcxproj
├── durag_executor.vcxproj.filters
└── README.md
```

Build-generated directories such as `x64/`, `Debug/`, and `Release/` should generally stay out of version control.

---

## Requirements

- Windows
- Visual Studio 2022 or compatible MSVC toolchain
- C++ development workload
- Windows SDK
- Lua 5.4
- MinHook

Make sure the required include and library paths are configured in the Visual Studio project before building.

---

## Building

1. Clone the repository:

```bash
git clone https://github.com/1mrswagchicken-alt/duragsrc.git
cd duragsrc
```

2. Open:

```text
durag_executor.vcxproj
```

3. Select the appropriate configuration, typically:

```text
Release | x64
```

4. Build the project through Visual Studio.

The resulting DLL will be placed in the configured Visual Studio output directory.

---

## Documentation

For the project's documented concepts and feature information, see:

**Durag Docs**  
https://durag.gitbook.io/durag-docs

**Durag Website**  
https://duragexec.vercel.app/

---

## Contributing

Contributions are welcome.

Before opening a pull request:

- Keep changes focused.
- Follow the existing C++ style.
- Avoid committing generated build output.
- Document non-obvious implementation details.
- Test your changes in a clean build.

Please open an issue first for large architectural changes so they can be discussed before implementation.

---

## Contributors

Thanks to everyone who contributes to the project.

<a href="https://github.com/1mrswagchicken-alt/duragsrc/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=1mrswagchicken-alt/duragsrc" alt="Contributors">
</a>

### Contribution Statistics

[![Contributors](https://img.shields.io/github/contributors/1mrswagchicken-alt/duragsrc?style=flat-square)](https://github.com/1mrswagchicken-alt/duragsrc/graphs/contributors)
[![Commit Activity](https://img.shields.io/github/commit-activity/m/1mrswagchicken-alt/duragsrc?style=flat-square)](https://github.com/1mrswagchicken-alt/duragsrc/graphs/commit-activity)
[![Last Commit](https://img.shields.io/github/last-commit/1mrswagchicken-alt/duragsrc?style=flat-square)](https://github.com/1mrswagchicken-alt/duragsrc/commits/main)

---

## License

Add the project's license here before publishing this repository.

For example:

```text
MIT License
```

or replace this section with the license you choose.

---

## Disclaimer

Durag is provided for educational and research purposes.

The project is **not affiliated with or endorsed by Vortex or its developers**. Software that interacts with third-party applications or games may violate their terms of service or trigger security/anti-cheat measures. Use the software only where you have permission to do so.

---

<p align="center">
  <sub>Built with C++ • Maintained by the Durag community</sub>
</p>
