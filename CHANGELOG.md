# Changelog

All notable changes to **RecGo** are documented in this file.

## [0.3.1] - 2026-11-20

### Fixed
- **Flatpak Startup Crash (`SafeSettings`)**: Added `SafeSettings` (`src/safe-settings.vala`) wrapping `GLib.Settings` with `SettingsSchema.has_key()` checks and in-memory fallback defaults, preventing fatal `GLib-GIO-ERROR` aborts (`Settings schema 'io.github.Antux912012.RecGo' does not contain a key named 'webcam-border'`) when launched with an outdated Flatpak wrapper schema.

## [0.3.0] - 2026-11-20

### Added
- **Masked Webcam Recording (`recgo-video-fx.c`)**: Antialiased SDF masks for Circle, Rounded Square, and Rounded Rectangle shapes—along with configurable white borders and drop shadows—are now baked directly into the recorded video stream on both GPU (`glvideomixer`) and CPU (`compositor`) pipelines.
- **Webcam Image FX Suite**: Added a live Image FX popover with quick presets (*Natural*, *Low-Light Boost*, *Studio Warm*, *Crisp & Vivid*, *Black & White*), *Horizontal Mirror*, *Auto Low-Light Enhance*, *Digital Zoom*, *Brightness*, *Contrast*, *Exposure / Gamma*, *Color Warmth*, *Saturation*, *Sharpness*, and *Edge-Preserving Temporal Noise Reduction*—applied identically to live preview and recordings.
- **Older Hardware Performance Profiles**: Added *Performance Profile* (*Older Hardware / Power Saver 720p*, *Balanced*, *High Quality*) and *Target Frame Rate* (`24` / `30` / `60` FPS) in Preferences, plus AUR `PKGBUILD` / `.SRCINFO` packaging.

### Fixed & Optimized
- **Automatic GPU-to-CPU Fallback**: Recording automatically falls back from `glvideomixer` or hardware VA-API/Vulkan encoders to CPU `compositor` / software encoding (`x264enc`, `openh264enc`, `vp8enc`) if driver initialization fails.
- **Crash-Safe MP4 & Lower CPU Overhead**: Switched MP4 muxing to fragmented MP4 (`fragment-duration=1000`), capped preview bandwidth (`480x270@15fps` to `960x540@24fps`), eliminated double frame copies in `RecGoPreviewFeed`, and fixed `RecGoDeviceManager`, `PreferencesDialog`, and `PreviewWidget` lifecycle/UI bugs.

## [0.2.0] - 2026-11-16

### Added
- **Vala + C + Meson Architecture**: Replaced the Rust/Tokio/ashpd/zbus stack with native Vala, C, and Meson (`meson.build`, `Containerfile`, `package-deb.sh`, `package-rpm.sh`, `package-flatpak.sh`).
- **Multi-Camera Handling**: Live `GstDeviceMonitor` hotplug discovery and simultaneous multi-camera compositing across `Desktop + PiP`, `Side by Side`, and `Camera Only` grid layouts, with per-camera toggles in Preferences.
- **Multi-Microphone Handling**: Live `GstDeviceMonitor` audio source discovery and automatic `audiomixer` mixing when multiple microphones are enabled.
- **OpenGL GPU Compositor**: Added `glvideomixer` + `glupload` + `glcolorconvert` + `gldownload` compositing path with a Preferences toggle.
- **GNOME 51 Integration**: Updated GNOME Shell panel indicator (`extension/metadata.json`, `extension/extension.js`) for GNOME Shell 48–51 with live `PropertiesChanged` updates, native `GDBus` `ScreenCast` & `GlobalShortcuts` portals, and `org.gnome.Platform/x86_64/51` Flatpak target.

### Fixed & Optimized
- **Event-Driven Preview**: Replaced the 33ms UI polling timer with signal-driven `frame-ready` delivery reusing a single staging buffer.
- **Device Teardown & FD Safety**: Synchronously stops preview feeds before starting recording to prevent `/dev/video*` `EBUSY` races, and duplicates PipeWire FDs via `F_DUPFD_CLOEXEC`.
- **Clean Container Finalization**: Replaced the 3-second hardcoded EOS timeout with `gst_bus_add_watch` async `EOS`/`ERROR` handling and `mp4mux faststart=true`.
- **Persistent GSettings & UI Fixes**: Wired `GLib.Settings` for codec, layout, webcam shape/position/scale, countdown, and devices; fixed Record button reset when cancelling countdown via Stop; fixed Pango `&amp;` escaping in Preferences.

## [0.1.6] - 2026-11-13
- Bundled Papirus symbolic SVG icons via GResource.

## [0.1.5] - 2026-11-13
- Redesigned application icon and standardized `io.github.Antux912012.RecGo` application ID.

## [0.1.4] - 2026-11-13
- GNOME/Libadwaita HIG studio UI/UX redesign and AppStream screenshots.
