/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team
          (See CREDITS.MD for more details)

(You may also contact divingkxt or powermax2286 on Discord)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * DingusPPC-Pippin modifications
 * Copyright (c) 2026 Iain Bennett
 * Licensed under the same terms as above.
 * Change log:
 *   2026-11-15 - Q219 baseline (Q48 to Q219, before version control): raw indexed frames and pooled frame updates.
 * Full history: PIPPIN.md and Git history.
 */

/** @file Display/screen abstraction (implemented on each platform). */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <core/hostevents.h>
#include <core/memaccess.h>
#include <devices/video/indexed_snapshot.h>

#include <cstdint>
#include <functional>
#include <memory>

/* HACK: the stuff below is a VERY BASIC platform abstraction hardcoded for SDL2
   for now: SDL2 framebuffer's endianness matches the host's CPU endianness. */
#define FB_WRITE    WRITE_DWORD_NE_A
#define FB_READ     READ_DWORD_NE_A

class VideoCtrlBase;

class Display {
public:
    enum {
        not_full_screen,
        full_screen_int,
        full_screen,
        full_screen_no_bars,
    };

    Display();
    ~Display();

    void set_video_ctrl(VideoCtrlBase* video_ctrl) {
        this->video_ctrl = video_ctrl;
    }

    // Configures the display for the given width/height.
    // Returns true if this is the first time the screen has been configured.
    bool configure(int width, int height);

    void configure_dest();
    void configure_texture();
    void update_window_size();

    // Clears the display
    void blank();


    // Update the host framebuffer display. If the display adapter does its own
    // dirty tracking, fb_known_to_be_changed will be set to true, so that the
    // implementation can take that into account.
    void update(std::function<void(uint8_t *dst_buf, int dst_pitch)> convert_fb_cb,
                std::function<void(uint8_t *dst_buf, int dst_pitch)> cursor_ovl_cb,
                bool draw_hw_cursor, int cursor_x, int cursor_y,
                bool fb_known_to_be_changed);

    bool raw_frames_enabled() const;
    void update_raw(std::unique_ptr<const pippin_indexed::Snapshot> snapshot,
                    bool draw_hw_cursor, int cursor_x, int cursor_y);

    // Called in cases where the framebuffer contents have not changed, so a
    // normal update() call is not happening. Allows implementations that need
    // to do per-frame bookkeeping to still do that.
    void update_skipped();

    void handle_events(const WindowEvent& wnd_event);
    void setup_hw_cursor(std::function<void(uint8_t *dst_buf, int dst_pitch)> draw_hw_cursor,
                         int cursor_width, int cursor_height);
    void update_window_title();
    void toggle_mouse_grab();
    void update_mouse_grab(bool will_be_grabbed);
private:
    void update_pooled(std::function<void(uint8_t*, int)> convert_fb_cb,
                       std::function<void(uint8_t*, int)> cursor_ovl_cb,
                       bool draw_hw_cursor, int cursor_x, int cursor_y);
    void present_pending_pooled_frame(uint64_t generation);
    unsigned handle_events_main(const WindowEvent& wnd_event);
    void apply_guest_dirty(unsigned dirty);
    void present_pending_frame(uint64_t generation);
    void render_frame(bool draw_hw_cursor, int cursor_x, int cursor_y);
    class Impl; // Holds private fields
    std::unique_ptr<Impl> impl;
    VideoCtrlBase* video_ctrl = nullptr;
    int full_screen_mode = not_full_screen;
    int full_screen_mode_forward = not_full_screen;
    int full_screen_mode_reverse = not_full_screen;
    bool use_scale_full_screen = false;
    bool is_set_full_screen = false;
    double scale_full_screen;
    double scale_window;
};

#endif // DISPLAY_H
