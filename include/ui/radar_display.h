#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/** Free the ~112KB off-screen frame sprite (240x240x16-bit), reclaiming it
 *  as one large contiguous heap block. Safe to call anytime — the next
 *  radarDisplayDraw()/radarDisplayRefreshAircraft() call recreates it
 *  lazily. Intended to be called right before a network fetch that needs
 *  a big contiguous allocation of its own (TLS handshake buffers), so the
 *  two never compete for the same headroom at the same time.
 */
void radarDisplayReleaseFrameBuffer();

}  // namespace ui
