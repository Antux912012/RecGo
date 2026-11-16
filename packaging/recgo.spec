Name:           recgo
Version:        0.2.0
Release:        1%{?dist}
Summary:        GPU-accelerated screen, window, and multi-webcam recorder for GNOME 51

License:        GPL-3.0-or-later
URL:            https://github.com/Antux912012/RecGo
Source0:        recgo-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  vala
BuildRequires:  pkgconfig(gtk4) >= 4.14
BuildRequires:  pkgconfig(libadwaita-1) >= 1.5
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(gstreamer-1.0) >= 1.20
BuildRequires:  pkgconfig(gstreamer-app-1.0) >= 1.20
BuildRequires:  pkgconfig(gstreamer-video-1.0) >= 1.20

Requires:       gtk4 >= 4.14
Requires:       libadwaita >= 1.5
Requires:       gstreamer1
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Requires:       gstreamer1-plugins-bad-free
Requires:       pipewire-gstreamer

%description
RecGo is a native GTK4/Libadwaita screen, window, and multi-webcam recorder
built with Vala, C, and Meson for GNOME 51. Features VA-API/Vulkan/OpenGL
hardware encoding, multi-cam compositing, multi-mic audiomixer support,
and GNOME Shell integration.

%prep
%autosetup -n recgo-%{version}

%build
%meson
%meson_build

%install
%meson_install

%files
%license LICENSE
%doc README.md
%{_bindir}/recgo
%{_datadir}/applications/io.github.Antux912012.RecGo.desktop
%{_datadir}/glib-2.0/schemas/io.github.Antux912012.RecGo.gschema.xml
%{_datadir}/metainfo/io.github.Antux912012.RecGo.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/io.github.Antux912012.RecGo.*

%changelog
* Mon Nov 16 2026 Antonio De Gaetano <antux912012@users.noreply.github.com> - 0.2.0-1
- Migrate to Vala, C, and Meson optimized for GNOME 51
- Add multi-camera and multi-microphone handling
