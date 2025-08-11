#pragma once

#include <hyprutils/memory/WeakPtr.hpp>
using namespace Hyprutils::Memory;
#define SP CSharedPointer
#define WP CWeakPointer

#include "wayland.hpp"
#include "protocols/linux-dmabuf-v1.hpp"
#include "protocols/hyprland-toplevel-export-v1.hpp"
