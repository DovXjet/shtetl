#!/bin/sh
# Installs the Fedora packages needed to cross compile shtetl for Windows
# (tools/build-mingw.sh). Run with: sudo ./install-mingw-deps.sh
set -e

dnf install -y \
    cmake ninja-build git file binutils \
    qt6-qtbase-devel qt6-qtbase-private-devel \
    mingw64-filesystem \
    mingw64-gcc mingw64-gcc-c++ mingw64-binutils mingw64-crt \
    mingw64-winpthreads mingw64-headers \
    mingw64-qt6-qtbase mingw64-qt6-qtsvg mingw64-qt6-qttools \
    mingw64-vulkan-headers mingw64-vulkan-loader \
    mingw64-zlib \
    mingw32-nsis
