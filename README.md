[!NOTE]
### Experimental Fix for Rotated/Vertical Monitors

This fork of `xdg-desktop-portal-hyprland` contains an experimental workaround for screen and region sharing on **rotated/vertical monitor setups**, where fullscreen streams may appear sideways or "Select region..." streams appear misoriented and mislocated.

It introduces a **"Force software transform (compatibility)"** option in the share picker. When enabled, it uses a client-side renderer to correctly orient the stream.

This solution is almost entirely vibe-coded. Heavy refactoring took place to mitigate context blowout on vibe-tennis and keep the robots coherent.

**Please be aware of the trade-offs:**
*   **Performance:** This method has higher CPU/GPU overhead than the native path.
*   **Initial Stream Delay:** Viewers may briefly see a green screen or experience a delay when first joining a stream. This is a known artifact of this specific workaround.

This is a **temporary solution**. The fundamental fix belongs upstream in the compositor (Hyprland/wlroots). This fork is provided as-is to help users in the meantime.

# xdg-desktop-portal-hyprland

An [XDG Desktop Portal](https://github.com/flatpak/xdg-desktop-portal) backend
for Hyprland.

## Installing

First, make sure to install the required dependencies:

```
gbm
hyprland-protocols
hyprlang
hyprutils
hyprwayland-scanner
libdrm
libpipewire-0.3
libspa-0.2
sdbus-cpp
wayland-client
wayland-protocols
```

Then run the build and install command:

```sh
git clone --recursive https://github.com/hyprwm/xdg-desktop-portal-hyprland
cd xdg-desktop-portal-hyprland/
cmake -DCMAKE_INSTALL_LIBEXECDIR=/usr/lib -DCMAKE_INSTALL_PREFIX=/usr -B build
cmake --build build
sudo cmake --install build
```

## Nix

> [!CAUTION]
> XDPH should not be used from this flake directly!
>
> Instead, use it from the [Hyprland flake](https://github.com/hyprwm/Hyprland).

There are two reasons for the above:

1. Hyprland depends on XDPH, but XDPH also depends on Hyprland. This results in
   a cyclic dependency, which is a nightmare. To counter this, we use the
   Nixpkgs Hyprland package in this flake, so that it can be later consumed by
   the Hyprland flake while overriding the Hyprland package.
2. Even if you manually do all the overriding, you may still get it wrong and
   lose out on the Cachix cache (which has XDPH as exposed by the Hyprland
   flake).

## Running, FAQs, etc.

See
[the Hyprland wiki](https://wiki.hyprland.org/Hypr-Ecosystem/xdg-desktop-portal-hyprland/)
