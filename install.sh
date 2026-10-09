#!/usr/bin/env bash
# Build and install QtFlix with an app-menu shortcut.
#   ./install.sh              install for this user into ~/.local (no sudo)
#   ./install.sh --system     install for all users into /usr/local (sudo)
#   ./install.sh --deb        build a .deb and install it with apt (sudo)
#   ./install.sh --uninstall  remove a ~/.local install (add --system for /usr/local)
set -e
cd "$(dirname "$0")"

prefix="$HOME/.local"
sudo=""
mode=install
for arg in "$@"; do
    case "$arg" in
        --system) prefix=/usr/local; sudo=sudo ;;
        --deb) mode=deb ;;
        --uninstall) mode=uninstall ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

refresh() {
    $sudo update-desktop-database -q "$prefix/share/applications" 2>/dev/null || true
    $sudo gtk-update-icon-cache -q -t "$prefix/share/icons/hicolor" 2>/dev/null || true
}

if [ "$mode" = uninstall ]; then
    $sudo rm -f "$prefix/bin/qtflix" "$prefix/share/applications/qtflix.desktop" \
        "$prefix/share/icons/hicolor/scalable/apps/qtflix.svg"
    refresh
    echo "Removed QtFlix from $prefix"
    exit 0
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
ninja -C build

if [ "$mode" = deb ]; then
    (cd build && cpack -G DEB)
    deb=$(ls -t build/qtflix_*.deb | head -n1)
    sudo apt install -y "./$deb"
    echo "Installed $deb. Launch QtFlix from your app menu."
    exit 0
fi

$sudo cmake --install build --strip
refresh
echo "Installed to $prefix. Launch QtFlix from your app menu (or run: qtflix)."
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "Note: $prefix/bin is not on your PATH." ;; esac
