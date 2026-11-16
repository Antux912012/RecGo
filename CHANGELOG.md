# Changelog

All notable changes to **RecGo** are documented in this file.

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
