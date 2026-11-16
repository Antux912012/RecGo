FROM registry.fedoraproject.org/fedora:latest

RUN dnf install -y --setopt=install_weak_deps=False \
    gcc \
    meson \
    ninja-build \
    vala \
    pkgconf-pkg-config \
    gtk4-devel \
    libadwaita-devel \
    glib2-devel \
    gstreamer1-devel \
    gstreamer1-plugins-base-devel \
    gstreamer1-plugins-bad-free-devel \
    pipewire-devel \
    libdrm-devel \
    mesa-libEGL-devel \
    mesa-libGL-devel \
    desktop-file-utils \
    appstream \
    rpm-build \
    tar \
    gzip \
    && dnf clean all

WORKDIR /workspace
