# Changelog

All notable changes to **RecGo** are documented in this file.

## [0.3.2] - 2026-11-25

### Added
- **Multi-Screen Recording (`portal.vala`, `recgo-recorder.c`, `preview-widget.vala`)**: Enabled `multiple: true` in XDG Desktop Portal ScreenCast sessions (`CaptureSession`), tiling multiple selected monitors/displays in `glvideomixer` / `compositor` and rendering a live multi-display preview grid.
- **Cam + Window App Recording Improvements**: Invalidate stale screen sessions when switching between Monitor and Application Window capture, preserve window aspect ratios with `videoscale add-borders=true` and `draw_texture_contain()`, and attach a `GST_EVENT_CAPS` probe on the OpenGL path for dynamic window resizes.
- **Simplified In-App Edit Mode (Kdenlive-Inspired)**: Added `RecGoEditor` (`src/gst/recgo-editor.c`), `TimelineWidget` (`src/timeline-widget.vala`), and `EditorView` (`src/editor-view.vala`) with a Project Bin, `Gtk.MediaFile` preview monitor and transport, V1/A1 timeline with playhead scrubbing, In/Out trim handles, razor split markers, and hardware/software trimmed export.
- **New Vector Application Icon & Complete Papirus Theme Bundle**: Replaced `io.github.Antux912012.RecGo.svg` and all PNG resolutions (`32x32` through `512x512`), and bundled `list-add-symbolic.svg` and `media-playback-start-symbolic.svg` in GResource.

### Fixed & Cleaned Up
- **Multi-Microphone Mixing & Live Mute**: Standardized `audiomixer` (`48 kHz` F32LE, `audiorate`, `volume` per mic) with live `recgo_recorder_set_mic_muted()` wired to the header microphone toggle, and fixed unchecking the last mic/camera in Preferences (`"none"` sentinel).
- **UI & Codebase Cleanup**: Removed leftover Rust v0.1.x files, simplified `data/style.css` to use native Libadwaita `.pill` buttons, removed duplicated Countdown/Encoders Preferences UI, refactored `append_pre_encode` into a table (adding `svtav1enc`), and ref-counted `comp` inside `ScreenSlotProbeData`.

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
