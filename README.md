# RecGo (Screen & Webcam Recorder for GNOME)

<p align="center">
  <img src="data/icons/io.github.antonio.RecGo_256x256.png" alt="RecGo Icon" width="128" height="128">
</p>

A modern, high-performance GTK4 / Libadwaita screen, application window, and webcam recorder tailored for the GNOME desktop.

## Features

- **HIG Compliant GTK4 & Libadwaita UI:** Seamlessly integrates with GNOME design standards, light/dark styling, and status indicators.
- **Wayland Display Capture:** Native multi-monitor, specific display, or single application window capture powered by XDG Desktop Portal (`org.freedesktop.portal.ScreenCast`) and PipeWire.
- **Draggable & Resizable Webcam Overlay:**
  - Freely drag the camera feed anywhere on the preview canvas.
  - Interactive corner resize handle, mouse-wheel scroll resizing, or bottom-bar slider.
  - Geometric shape masks (**Circle**, **Square**, **Rectangle**) with dynamic soft drop shadows to make the camera overlay pop out from the recording.
- **Multi-Preset Layouts:**
  - **Desktop + Webcam Picture-in-Picture (PiP)**
  - **Side by Side (50/50 split)**
  - **Screen Only**
  - **Camera Only**
- **Hardware Acceleration:**
  - Full **VA-API** (Intel Quick Sync & AMD Radeon), **Vulkan Video**, and **OpenGL** hardware acceleration.
  - Encoders supported: **H.264 / AVC**, **H.265 / HEVC**, **VP8**, **VP9**, and **AV1**.
- **Microphone Audio Recording:** Live synchronized audio capture with Opus or AAC encoding via PulseAudio/PipeWire.
- **Flatpak Ready:** Sandboxed distribution with containerized build scripts for reproducible builds.

## Building and Packaging

### Using the Podman Container (Recommended)

RecGo includes automated build scripts to compile inside containerized environments:

```bash
# Verify compilation inside the container
./build-in-podman.sh check

# Build debug or release binaries
./build-in-podman.sh build

# Package as a standalone Flatpak bundle
./package-flatpak.sh
```

### Installing the Flatpak

```bash
flatpak install --user RecGo.flatpak
flatpak run io.github.antonio.RecGo
```

## License

GPL-3.0-or-later
