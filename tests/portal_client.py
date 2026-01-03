#!/usr/bin/env python3
"""
portal_client.py - D-Bus client to test xdg-desktop-portal-hyprland ScreenCast

This script:
1. Creates a ScreenCast session via D-Bus
2. Selects sources (monitor)
3. Starts the stream
4. Captures frames from PipeWire using GStreamer
5. Saves a frame as PNG and validates dimensions

Usage:
    python3 portal_client.py [--output-dir DIR] [--expected-width W] [--expected-height H]

Exit codes:
    0 - PASS: Frame captured with expected dimensions
    1 - FAIL: Frame captured but dimensions wrong (rotation not applied)
    2 - ERROR: Could not capture frame or other error
"""

import argparse
import os
import subprocess
import sys
import time
import tempfile
from pathlib import Path

try:
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    import gi
    gi.require_version('GLib', '2.0')
    from gi.repository import GLib
except ImportError as e:
    print(f"ERROR: Missing dependency: {e}", file=sys.stderr)
    print("Install: pip install dbus-python PyGObject", file=sys.stderr)
    sys.exit(2)


PORTAL_BUS = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast"
REQUEST_IFACE = "org.freedesktop.portal.Request"


class ScreenCastClient:
    def __init__(self, output_dir: Path):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SessionBus()
        self.loop = GLib.MainLoop()
        
        self.portal = self.bus.get_object(PORTAL_BUS, PORTAL_PATH)
        self.screencast = dbus.Interface(self.portal, SCREENCAST_IFACE)
        
        self.session_handle = None
        self.pipewire_node_id = None
        self.stream_width = None
        self.stream_height = None
        self.frame_path = None
        self.error = None
        
        # Request counter for unique handle tokens
        self.request_count = 0
    
    def _get_request_token(self) -> str:
        self.request_count += 1
        return f"xdph_test_{os.getpid()}_{self.request_count}"
    
    def _get_session_token(self) -> str:
        return f"xdph_session_{os.getpid()}"
    
    def _wait_for_response(self, request_path: str, timeout: float = 30.0):
        """Wait for portal response signal."""
        result = {"response": None, "results": None}
        
        def on_response(response, results):
            result["response"] = response
            result["results"] = results
            self.loop.quit()
        
        self.bus.add_signal_receiver(
            on_response,
            signal_name="Response",
            dbus_interface=REQUEST_IFACE,
            path=request_path
        )
        
        # Add timeout
        GLib.timeout_add_seconds(int(timeout), lambda: (self.loop.quit(), True)[1])
        self.loop.run()
        
        if result["response"] is None:
            raise TimeoutError(f"Portal request timed out after {timeout}s")
        
        if result["response"] != 0:
            raise RuntimeError(f"Portal request failed with response code {result['response']}")
        
        return result["results"]
    
    def create_session(self) -> str:
        """Create a ScreenCast session."""
        print("[*] Creating ScreenCast session...")
        
        token = self._get_request_token()
        session_token = self._get_session_token()
        
        options = {
            "handle_token": token,
            "session_handle_token": session_token,
        }
        
        request_path = self.screencast.CreateSession(options)
        results = self._wait_for_response(request_path)
        
        self.session_handle = results.get("session_handle")
        print(f"[+] Session created: {self.session_handle}")
        return self.session_handle
    
    def select_sources(self):
        """Select sources for the session (will auto-select first monitor)."""
        print("[*] Selecting sources...")
        
        token = self._get_request_token()
        options = {
            "handle_token": token,
            "types": dbus.UInt32(1),  # 1 = MONITOR, 2 = WINDOW, 4 = VIRTUAL
            "multiple": False,
            "cursor_mode": dbus.UInt32(2),  # 2 = EMBEDDED
        }
        
        request_path = self.screencast.SelectSources(self.session_handle, options)
        self._wait_for_response(request_path)
        print("[+] Sources selected")
    
    def start(self):
        """Start the screencast session."""
        print("[*] Starting session...")
        
        token = self._get_request_token()
        options = {
            "handle_token": token,
        }
        
        request_path = self.screencast.Start(self.session_handle, "", options)
        results = self._wait_for_response(request_path)
        
        streams = results.get("streams", [])
        if not streams:
            raise RuntimeError("No streams returned from Start")
        
        # Extract stream info
        node_id, stream_props = streams[0]
        self.pipewire_node_id = node_id
        
        # Get stream dimensions from properties
        size = stream_props.get("size", (0, 0))
        if size:
            self.stream_width, self.stream_height = size
        
        print(f"[+] Stream started: node_id={self.pipewire_node_id}, size={self.stream_width}x{self.stream_height}")
        return self.pipewire_node_id
    
    def capture_frame(self, num_frames: int = 5) -> Path:
        """Capture frames from PipeWire stream using GStreamer."""
        print(f"[*] Capturing {num_frames} frames from PipeWire node {self.pipewire_node_id}...")
        
        self.frame_path = self.output_dir / "captured_frame.png"
        
        # GStreamer pipeline to capture frames
        # We capture a few frames to ensure we get a valid one
        gst_pipeline = (
            f"pipewiresrc path={self.pipewire_node_id} num-buffers={num_frames} ! "
            f"videoconvert ! "
            f"pngenc ! "
            f"multifilesink location={self.output_dir}/frame_%d.png"
        )
        
        try:
            result = subprocess.run(
                ["gst-launch-1.0", "-e"] + gst_pipeline.split(),
                capture_output=True,
                text=True,
                timeout=30
            )
            
            if result.returncode != 0:
                print(f"[-] GStreamer error: {result.stderr}", file=sys.stderr)
                # Try to continue if we got at least one frame
        except subprocess.TimeoutExpired:
            print("[-] GStreamer capture timed out", file=sys.stderr)
        except FileNotFoundError:
            raise RuntimeError("gst-launch-1.0 not found. Install gstreamer.")
        
        # Find the last captured frame
        frames = sorted(self.output_dir.glob("frame_*.png"))
        if not frames:
            raise RuntimeError("No frames captured")
        
        # Use the last frame (most likely to be complete)
        self.frame_path = frames[-1]
        print(f"[+] Captured frame: {self.frame_path}")
        return self.frame_path
    
    def get_frame_dimensions(self) -> tuple:
        """Get dimensions of captured frame using ImageMagick identify or Python PIL."""
        if not self.frame_path or not self.frame_path.exists():
            raise RuntimeError("No frame captured")
        
        # Try using 'identify' from ImageMagick
        try:
            result = subprocess.run(
                ["identify", "-format", "%w %h", str(self.frame_path)],
                capture_output=True,
                text=True,
                timeout=10
            )
            if result.returncode == 0:
                w, h = map(int, result.stdout.strip().split())
                return w, h
        except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
            pass
        
        # Fallback: try PIL
        try:
            from PIL import Image
            with Image.open(self.frame_path) as img:
                return img.size
        except ImportError:
            pass
        
        # Fallback: try file command and parse
        try:
            result = subprocess.run(
                ["file", str(self.frame_path)],
                capture_output=True,
                text=True,
                timeout=10
            )
            # Parse something like "PNG image data, 1920 x 1080, 8-bit/color RGBA"
            import re
            match = re.search(r'(\d+)\s*x\s*(\d+)', result.stdout)
            if match:
                return int(match.group(1)), int(match.group(2))
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
        
        raise RuntimeError("Could not determine frame dimensions")
    
    def cleanup(self):
        """Close the session."""
        if self.session_handle:
            try:
                session = self.bus.get_object(PORTAL_BUS, self.session_handle)
                session_iface = dbus.Interface(session, "org.freedesktop.portal.Session")
                session_iface.Close()
                print("[*] Session closed")
            except Exception as e:
                print(f"[!] Error closing session: {e}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Test portal screencopy rotation")
    parser.add_argument("--output-dir", type=Path, default=Path(tempfile.mkdtemp(prefix="xdph_test_")),
                        help="Directory to save captured frames")
    parser.add_argument("--expected-width", type=int, default=1080,
                        help="Expected frame width after rotation (default: 1080)")
    parser.add_argument("--expected-height", type=int, default=1920,
                        help="Expected frame height after rotation (default: 1920)")
    parser.add_argument("--allow-unrotated", action="store_true",
                        help="Don't fail if dimensions are unrotated (1920x1080)")
    args = parser.parse_args()
    
    print(f"=== XDPH Rotation Test ===")
    print(f"Output directory: {args.output_dir}")
    print(f"Expected dimensions: {args.expected_width}x{args.expected_height}")
    print()
    
    client = ScreenCastClient(args.output_dir)
    
    try:
        # Run the screencast flow
        client.create_session()
        client.select_sources()
        client.start()
        
        # Give the stream a moment to stabilize
        time.sleep(0.5)
        
        client.capture_frame()
        
        # Check dimensions
        actual_w, actual_h = client.get_frame_dimensions()
        print(f"\n=== RESULT ===")
        print(f"Captured frame dimensions: {actual_w}x{actual_h}")
        print(f"Expected dimensions: {args.expected_width}x{args.expected_height}")
        
        # Determine pass/fail
        if actual_w == args.expected_width and actual_h == args.expected_height:
            print("\n[PASS] Frame dimensions match expected (rotation applied correctly)")
            return 0
        elif actual_w == args.expected_height and actual_h == args.expected_width:
            # Dimensions are swapped - rotation not applied
            print("\n[FAIL] Frame dimensions are unrotated (raw buffer passed through)")
            if args.allow_unrotated:
                print("       (--allow-unrotated flag set, not failing)")
                return 0
            return 1
        else:
            print(f"\n[FAIL] Unexpected dimensions: got {actual_w}x{actual_h}")
            return 1
    
    except Exception as e:
        print(f"\n[ERROR] {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 2
    
    finally:
        client.cleanup()


if __name__ == "__main__":
    sys.exit(main())
