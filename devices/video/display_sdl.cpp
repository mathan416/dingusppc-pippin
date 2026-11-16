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
 *   2026-11-15 - Q219 baseline (Q48 to Q219, before version control): raw indexed frame presentation from the CPU-owner thread.
 *   2026-11-16 - Review fixes: warn when raw frames are requested with the frame pool on; bounded window resize loop; fractional per-axis DPI scale.
 *   2026-11-17 - v1.0.0 review: frame pool is off unless SHEEP_FRAME_POOL=on (matches the tested setup); resize warning counts attempts correctly.
 * Full history: PIPPIN.md and Git history.
 */

#include <devices/video/display.h>
#include <devices/video/videoctrl.h>
#include <devices/video/taos.h>
#include <devices/video/frame_pixel_pool.h>
#include <main_cpu_owner.h>
#include <SDL.h>
#include <loguru.hpp>
#include <cmath>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

bool g_auto_grab_mouse = false;

static const char * get_full_screen_mode_string(int scale_mode) {
#define onemode(x) case Display::x: return #x ;
    switch(scale_mode) {
        onemode(not_full_screen)
        onemode(full_screen_int)
        onemode(full_screen)
        onemode(full_screen_no_bars)
        default: return "unknown";
    }
}

class Display::Impl {
public:
    struct CursorImage {
        int width = 0, height = 0;
        std::vector<uint32_t> pixels;
    };
    struct Frame {
        uint64_t generation = 0;
        int width = 0, height = 0;
        bool draw_cursor = false;
        int cursor_x = 0, cursor_y = 0;
        std::vector<uint32_t> pixels;
        std::shared_ptr<const CursorImage> cursor;
        std::unique_ptr<const pippin_indexed::Snapshot> indexed;
    };
    struct PooledFrame {
        uint64_t generation = 0;
        bool draw_cursor = false;
        int cursor_x = 0, cursor_y = 0;
        std::shared_ptr<const CursorImage> cursor;
        pippin_frame_pool::FramePixelPool::ReadyLease pixels;
    };
    // A main-thread callback holds this lifetime token, never a guest callback
    // or a pointer into VRAM. At most one frame waits while another presents.
    struct Mailbox {
        std::mutex mutex;
        std::shared_ptr<const Frame> latest;
        std::unique_ptr<PooledFrame> pool_latest;
        Display* owner = nullptr;
        bool scheduled = false;
        uint64_t generation = 0, submitted = 0, replaced = 0;
        uint64_t presented = 0, blanked = 0, stale = 0;
    };
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    // Startup option is immutable; pool-off retains the Q156 allocation path.
    bool pool_enabled = false;
    bool raw_enabled = false;
    uint64_t raw_submitted = 0, raw_presented = 0, raw_bytes = 0, raw_fallback = 0;
    bool pool_qualified = false;
    std::unique_ptr<pippin_frame_pool::FramePixelPool> pixel_pool;
    uint64_t pool_modes = 0;
    uint64_t old_acquired = 0, old_published = 0, old_reads = 0;
    uint64_t old_released = 0, old_exhausted = 0;
    void configure_pool(int width, int height) {
        if (pixel_pool && pixel_pool->width() == width && pixel_pool->height() == height) return;
        if (pixel_pool) {
            const auto counts = pixel_pool->stats();
            // The main configure barrier has completed any old read lease.
            if (counts.states[unsigned(pippin_frame_pool::FramePixelPool::SlotState::free)] != 3)
                throw std::logic_error("Frame-pixel mode change with an active lease");
            old_acquired += counts.acquired; old_published += counts.published;
            old_reads += counts.reads; old_released += counts.released;
            old_exhausted += counts.exhausted;
        }
        pixel_pool = std::make_unique<pippin_frame_pool::FramePixelPool>(width, height);
        ++pool_modes;
    }
    // Guest owner only (initial values are set before the worker starts).
    int frame_w = 0, frame_h = 0;
    bool guest_blanked = false;
    std::shared_ptr<const CursorImage> guest_cursor;
    // Main owner only.
    std::shared_ptr<const CursorImage> uploaded_cursor;

    uint32_t        disp_wnd_id = 0;
    SDL_Window*     display_wnd = 0;
    SDL_Renderer*   renderer = 0;
    double          renderer_scale_x; // scaling factor from guest OS to host OS
    double          renderer_scale_y;
    double          default_scale_x;
    double          default_scale_y;
    SDL_Texture*    disp_texture = 0;
    SDL_Texture*    cursor_texture = 0;
    SDL_Rect        cursor_rect; // destination rectangle for cursor drawing
    bool            show_host_cursor = true; // desired SDL host pointer visibility
    bool            guest_cursor_drawn = false; // guest is drawing its own cursor
    bool            was_grabbed = false; // last reported grab state
    bool            manual_grab = false; // grab toggled on with Ctrl+G
    int             display_w;
    int             display_h;
    double          drawable_w;
    double          drawable_h;
    SDL_Rect        dest_rect;
};

Display::Display(): impl(std::make_unique<Impl>()) {
    const char* raw_option = std::getenv("SHEEP_RAW_FRAME");
    if (raw_option && std::strcmp(raw_option, "on") && std::strcmp(raw_option, "off"))
        throw std::invalid_argument("SHEEP_RAW_FRAME must be on or off");
    impl->raw_enabled = raw_option && !std::strcmp(raw_option, "on");
    const char* pool_option = std::getenv("SHEEP_FRAME_POOL");
    if (pool_option && std::strcmp(pool_option, "on") && std::strcmp(pool_option, "off"))
        throw std::invalid_argument("SHEEP_FRAME_POOL must be on or off");
    // Off unless asked for: every release build was verified with raw frames and the pool off.
    impl->pool_enabled = pippin_cpu_owner::enabled() && pool_option && !std::strcmp(pool_option, "on");
    if (impl->raw_enabled && impl->pool_enabled)
        LOG_F(WARNING, "SHEEP_RAW_FRAME=on has no effect while the frame pool is on; set SHEEP_FRAME_POOL=off");
    impl->mailbox->owner = this;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
}

Display::~Display() {
    // run() joins the guest owner and drains main jobs before device teardown.
    if (pippin_cpu_owner::active() && !pippin_cpu_owner::is_main_thread())
        ABORT_F("Display destroyed away from the SDL owner");
    {
        std::lock_guard<std::mutex> lock(impl->mailbox->mutex);
        impl->mailbox->owner = nullptr;
        impl->mailbox->latest.reset();
        impl->mailbox->pool_latest.reset();
    }
    if (pippin_cpu_owner::enabled()) {
        LOG_F(INFO, "CPU-OWNER-RAW-FRAME enabled=%u submitted=%llu presented=%llu bytes=%llu fallback=%llu",
            unsigned(impl->raw_enabled && !impl->pool_enabled),
            (unsigned long long)impl->raw_submitted, (unsigned long long)impl->raw_presented,
            (unsigned long long)impl->raw_bytes, (unsigned long long)impl->raw_fallback);
        const auto& q = *impl->mailbox;
        LOG_F(INFO, "CPU-OWNER-DISPLAY submitted=%llu presented=%llu replaced=%llu blanked=%llu stale=%llu",
            (unsigned long long)q.submitted, (unsigned long long)q.presented,
            (unsigned long long)q.replaced, (unsigned long long)q.blanked,
            (unsigned long long)q.stale);
    }

    if (pippin_cpu_owner::enabled()) {
        const auto counts = impl->pixel_pool ? impl->pixel_pool->stats() : pippin_frame_pool::FramePixelPool::Stats{};
        LOG_F(INFO, "CPU-OWNER-FRAME-POOL enabled=%u modes=%llu large_allocations=%llu acquisitions=%llu publishes=%llu reads=%llu releases=%llu exhausted=%llu",
            unsigned(impl->pool_enabled), (unsigned long long)impl->pool_modes,
            (unsigned long long)(impl->pool_modes * 3),
            (unsigned long long)(impl->old_acquired + counts.acquired),
            (unsigned long long)(impl->old_published + counts.published),
            (unsigned long long)(impl->old_reads + counts.reads),
            (unsigned long long)(impl->old_released + counts.released),
            (unsigned long long)(impl->old_exhausted + counts.exhausted));
    }

    if (impl->cursor_texture) {
        SDL_DestroyTexture(impl->cursor_texture);
        impl->cursor_texture = 0;
    }

    if (impl->disp_texture) {
        SDL_DestroyTexture(impl->disp_texture);
        impl->disp_texture = 0;
    }

    if (impl->renderer) {
        SDL_DestroyRenderer(impl->renderer);
        impl->renderer = 0;
    }

    if (impl->display_wnd) {
        SDL_DestroyWindow(impl->display_wnd);
        impl->display_wnd = 0;
    }
}

const double scale_step = std::pow(2.0, 1.0/8.0);

bool Display::configure(int width, int height) {
    if (pippin_cpu_owner::active() && pippin_cpu_owner::is_worker_thread()) {
        impl->frame_w = width;
        impl->frame_h = height;
        impl->guest_blanked = false;
        // Only Taos has the audited full-frame 8/15-bit conversion contract.
        impl->pool_qualified = impl->pool_enabled && dynamic_cast<TaosVideo*>(video_ctrl) != nullptr;
        {
            std::lock_guard<std::mutex> lock(impl->mailbox->mutex);
            ++impl->mailbox->generation;
            impl->mailbox->latest.reset();
            impl->mailbox->pool_latest.reset();
            impl->mailbox->scheduled = false;
        }
        bool initialized = false;
        pippin_cpu_owner::call_main([this, width, height, &initialized] {
            initialized = this->configure(width, height);
        });
        if (impl->pool_qualified) impl->configure_pool(width, height);
        return initialized;
    }
    // Startup and the serial control both configure on the main thread.
    // While active, frame_w/frame_h belong exclusively to the guest worker.
    if (!pippin_cpu_owner::active()) {
        impl->frame_w = width;
        impl->frame_h = height;
        impl->pool_qualified = impl->pool_enabled && dynamic_cast<TaosVideo*>(video_ctrl) != nullptr;
        if (impl->pool_qualified) impl->configure_pool(width, height);
    }
    bool is_initialization = false;

    impl->display_w = width;
    impl->display_h = height;

    if (!impl->display_wnd) { // create display window
        impl->display_wnd = SDL_CreateWindow(
            "",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            impl->display_w, impl->display_h,
            SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI
        );

        impl->disp_wnd_id = SDL_GetWindowID(impl->display_wnd);
        if (impl->display_wnd == NULL)
            ABORT_F("Display: SDL_CreateWindow failed with %s", SDL_GetError());

        impl->renderer = SDL_CreateRenderer(impl->display_wnd, -1, SDL_RENDERER_ACCELERATED);
        if (impl->renderer == NULL)
            ABORT_F("Display: SDL_CreateRenderer failed with %s", SDL_GetError());

        SDL_RendererInfo info;
        SDL_GetRendererInfo(impl->renderer, &info);
        LOG_F(INFO, "Renderer \"%s\" max size: %d x %d", info.name, info.max_texture_width, info.max_texture_height);

        int w, h;
        double scale = 1.0;
        while (1) {
            SDL_GetRendererOutputSize(impl->renderer, &w, &h);
            if (w == 0 || h == 0) {
                scale /= scale_step;
                LOG_F(INFO, "Invalid renderer size. Reducing scale to %.3f.", scale);
                SDL_SetWindowSize(impl->display_wnd,
                    std::round(scale * impl->display_w),
                    std::round(scale * impl->display_h));
            } else
                break;
        }
        impl->default_scale_x = std::round(double(w) / std::round(scale * impl->display_w));
        impl->default_scale_y = std::round(double(h) / std::round(scale * impl->display_h));
        impl->drawable_w = scale * impl->display_w * impl->default_scale_x;
        impl->drawable_h = scale * impl->display_h * impl->default_scale_y;
        this->scale_window = scale * impl->default_scale_x;
        this->configure_dest();

        update_window_title();
        is_initialization = true;
    } else { // resize display window
        this->update_window_size();
        this->configure_dest();
    }

    configure_texture();

    return is_initialization;
}

void Display::update_window_size() {
    if (this->full_screen_mode != not_full_screen)
        return;

    int w, h;
    double w_err, h_err;
    // Bounded: a backend that refuses or clamps the requested size must not hang the SDL owner.
    for (int attempt = 0;; ++attempt) {
        impl->drawable_w = this->scale_window * impl->display_w;
        impl->drawable_h = this->scale_window * impl->display_h;

        SDL_SetWindowSize(impl->display_wnd,
            std::round(impl->drawable_w / impl->default_scale_x),
            std::round(impl->drawable_h / impl->default_scale_y));

        SDL_GetRendererOutputSize(impl->renderer, &w, &h);

        w_err = std::abs(impl->drawable_w - w);
        h_err = std::abs(impl->drawable_h - h);

        LOG_F(INFO, "update_window_size try drawable: %4.0f x %-4.0f  display: %4d x %-4d  scale: %.3f  err: %4.0f x %-4.0f",
            impl->drawable_w, impl->drawable_h, impl->display_w, impl->display_h, this->scale_window, w_err, h_err);

        if (w_err < 4 && h_err < 4)
            break;
        if (attempt >= 15) {
            LOG_F(WARNING, "update_window_size: window size not honoured after %d attempts; keeping %d x %d", attempt, w, h);
            impl->drawable_w = w;
            impl->drawable_h = h;
            break;
        }

        this->scale_window /= scale_step;
    }

    LOG_F(INFO, "update_window_size     drawable: %4.0f x %-4.0f  display: %4d x %-4d  scale: %.3f",
        impl->drawable_w, impl->drawable_h, impl->display_w, impl->display_h, this->scale_window);

    this->update_mouse_grab(false); // make sure the mouse is still inside the window
}

void Display::configure_dest() {
    bool should_set_full_screen = this->full_screen_mode > not_full_screen;
    if (this->is_set_full_screen != should_set_full_screen) {
        if (should_set_full_screen) {
            int w, h;
            SDL_SetWindowFullscreen(impl->display_wnd, SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_GetRendererOutputSize(impl->renderer, &w, &h);
            impl->drawable_w = w;
            impl->drawable_h = h;
        } else {
            SDL_SetWindowFullscreen(impl->display_wnd, 0);
            this->update_window_size();
        }
        this->is_set_full_screen = should_set_full_screen;
    }

    double scalex = impl->drawable_w / impl->display_w;
    double scaley = impl->drawable_h / impl->display_h;
    double scale_full = std::min(scalex, scaley);
    double scale_full_int = floor(scale_full);
    double scale_full_no_bars = std::max(scalex, scaley);
    double scale;

    if (this->full_screen_mode == not_full_screen) {
        scale = this->scale_window = scale_full;
        this->full_screen_mode_forward = full_screen_int;
        this->full_screen_mode_reverse = full_screen_no_bars;
    } else {
        if (!use_scale_full_screen || this->scale_full_screen == 0.0) {
            switch (this->full_screen_mode) {
                case full_screen_int:
                    this->scale_full_screen = scale_full_int;
                    break;
                case full_screen:
                    this->scale_full_screen = scale_full;
                    break;
                case full_screen_no_bars:
                    this->scale_full_screen = scale_full_no_bars;
                    break;
            }
        }
        scale = this->scale_full_screen;

        if (scale == scale_full) {
            // scale_full is preferable to scale_full_int
            this->full_screen_mode = full_screen;
        }

        if (scale >= scale_full_no_bars)
            this->full_screen_mode_forward = not_full_screen;
        else if (scale >= scale_full)
            this->full_screen_mode_forward = full_screen_no_bars;
        else if (scale >= scale_full_int)
            this->full_screen_mode_forward = full_screen;
        else
            this->full_screen_mode_forward = full_screen_int;

        if (scale <= scale_full_int)
            this->full_screen_mode_reverse = not_full_screen;
        else if (scale <= scale_full)
            this->full_screen_mode_reverse = full_screen_int;
        else if (scale <= scale_full_no_bars)
            this->full_screen_mode_reverse = full_screen;
        else
            this->full_screen_mode_reverse = full_screen_no_bars;
    }

    impl->dest_rect.w = std::round(scale * impl->display_w);
    impl->dest_rect.h = std::round(scale * impl->display_h);
    impl->dest_rect.x = std::round((impl->drawable_w - impl->dest_rect.w) / 2.0),
    impl->dest_rect.y = std::round((impl->drawable_h - impl->dest_rect.h) / 2.0),
    impl->renderer_scale_x = scale;
    impl->renderer_scale_y = scale;

    LOG_F(INFO, "configure_dest         drawable: %4.0f x %-4.0f  display: %4d x %-4d  scale: %.3f"
        " (int: %.3f  full: %.3f  nobars: %.3f)  mode: %s  forward: %s  reverse: %s",
        impl->drawable_w, impl->drawable_h, impl->display_w, impl->display_h, impl->renderer_scale_x,
        scale_full_int, scale_full, scale_full_no_bars,
        get_full_screen_mode_string(this->full_screen_mode),
        get_full_screen_mode_string(this->full_screen_mode_forward),
        get_full_screen_mode_string(this->full_screen_mode_reverse)
    );
}

void Display::configure_texture() {
    if (impl->disp_texture)
        SDL_DestroyTexture(impl->disp_texture);

    impl->disp_texture = SDL_CreateTexture(
        impl->renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        impl->display_w, impl->display_h
    );

    if (impl->disp_texture == NULL)
        ABORT_F("Display: SDL_CreateTexture failed with %s", SDL_GetError());
}

void Display::apply_guest_dirty(unsigned dirty) {
    if (dirty & 1) video_ctrl->set_draw_fb();
    if (dirty & 2) video_ctrl->set_cursor_dirty();
}

void Display::handle_events(const WindowEvent& wnd_event) {
    unsigned dirty = 0;
    if (pippin_cpu_owner::active()) {
        // Window signals are dispatched by the guest-owner event drain.
        if (!pippin_cpu_owner::is_worker_thread())
            throw std::logic_error("Display window event outside guest owner");
        pippin_cpu_owner::call_main([this, wnd_event, &dirty] {
            dirty = this->handle_events_main(wnd_event);
        });
    } else {
        dirty = this->handle_events_main(wnd_event);
    }
    this->apply_guest_dirty(dirty);
}

unsigned Display::handle_events_main(const WindowEvent& wnd_event) {
    unsigned dirty = 0;
    switch (wnd_event.sub_type) {

    case SDL_WINDOWEVENT_SIZE_CHANGED:
        if (wnd_event.window_id == impl->disp_wnd_id) {
            int ww, wh;
            SDL_GetWindowSize(impl->display_wnd, &ww, &wh);
            int w, h;
            SDL_GetRendererOutputSize(impl->renderer, &w, &h);
            if (ww <= 0 || wh <= 0)
                break;
            double new_default_scale_x = double(w) / ww;
            double new_default_scale_y = double(h) / wh;
            if (new_default_scale_x != impl->default_scale_x || new_default_scale_y != impl->default_scale_y) {
                // recalculate scale when switching between Retina and Low Resolution modes
                impl->drawable_w = impl->drawable_w * new_default_scale_x / impl->default_scale_x;
                impl->drawable_h = impl->drawable_h * new_default_scale_y / impl->default_scale_y;
                this->scale_window = this->scale_window * new_default_scale_x / impl->default_scale_x;
                impl->default_scale_x = new_default_scale_x;
                impl->default_scale_y = new_default_scale_y;
                this->configure_dest();
            }

            this->update_window_title();
            dirty |= 1;
        }
        break;

    case SDL_WINDOWEVENT_EXPOSED:
        if (wnd_event.window_id == impl->disp_wnd_id) {
            dirty |= 1;
        }
        break;

    case SDL_WINDOWEVENT_FOCUS_GAINED:
        if (wnd_event.window_id == impl->disp_wnd_id) {
            SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
            SDL_SetWindowKeyboardGrab(impl->display_wnd, SDL_TRUE);
if (g_auto_grab_mouse) {
            // When the window (re)gains focus (e.g. the user clicks it), SDL
            // may have dropped the relative mouse grab on focus loss. Re-grab
            // it so the guest cursor keeps working; this is the reliable
            // re-grab trigger instead of polling every frame.
            if (impl->guest_cursor_drawn && !impl->manual_grab && !SDL_GetRelativeMouseMode()) {
                SDL_SetRelativeMouseMode(SDL_TRUE);
                this->update_window_title();
            }
}
        }
        break;

    case SDL_WINDOWEVENT_FOCUS_LOST:
        if (wnd_event.window_id == impl->disp_wnd_id) {
            SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "1");
            SDL_SetWindowKeyboardGrab(impl->display_wnd, SDL_FALSE);
        }
        break;

    case DPPC_WINDOWEVENT_WINDOW_SCALE_QUALITY_TOGGLE:
        if (wnd_event.window_id == impl->disp_wnd_id) {
            auto current_quality = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
            auto new_quality = current_quality == NULL || strcmp(current_quality, "nearest") == 0 ? "best" : "nearest";
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, new_quality);

            // We need the window/texture to be recreated to pick up the hint change.
            this->configure_texture();
            dirty |= 1;
            dirty |= 2;
        }
        break;

    case DPPC_WINDOWEVENT_WINDOW_FULL_SCREEN_TOGGLE:
    case DPPC_WINDOWEVENT_WINDOW_FULL_SCREEN_TOGGLE_REVERSE:
        {
            if (wnd_event.window_id != impl->disp_wnd_id)
                break;

            if (wnd_event.sub_type == DPPC_WINDOWEVENT_WINDOW_FULL_SCREEN_TOGGLE)
                this->full_screen_mode = this->full_screen_mode_forward;
            else
                this->full_screen_mode = this->full_screen_mode_reverse;

            if (this->full_screen_mode > not_full_screen)
                this->use_scale_full_screen = false;

            this->configure_dest();
            dirty |= 1;
            dirty |= 2;
        }
        break;

    case DPPC_WINDOWEVENT_WINDOW_BIGGER:
    case DPPC_WINDOWEVENT_WINDOW_SMALLER:
        if (wnd_event.window_id == impl->disp_wnd_id) {
            if (this->full_screen_mode == not_full_screen) {
                if (wnd_event.sub_type == DPPC_WINDOWEVENT_WINDOW_BIGGER)
                    this->scale_window *= scale_step;
                else
                    this->scale_window /= scale_step;
                this->update_window_size();
            } else {
                this->use_scale_full_screen = true;
                if (wnd_event.sub_type == DPPC_WINDOWEVENT_WINDOW_BIGGER)
                    this->scale_full_screen *= scale_step;
                else
                    this->scale_full_screen /= scale_step;
            }

            this->configure_dest();
            dirty |= 1;
            dirty |= 2;
        }
        break;

    case DPPC_WINDOWEVENT_MOUSE_GRAB_TOGGLE:
        if (wnd_event.window_id == impl->disp_wnd_id)
            this->toggle_mouse_grab();
        break;

    case DPPC_WINDOWEVENT_MOUSE_GRAB_CHANGED:
        this->update_window_title();
        break;

    }
    return dirty;
}

void Display::toggle_mouse_grab()
{
    if (SDL_GetRelativeMouseMode()) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        impl->manual_grab = false;
    } else {
        this->update_mouse_grab(true);
        SDL_SetRelativeMouseMode(SDL_TRUE);
        impl->manual_grab = true;
    }
}

void Display::update_mouse_grab(bool will_be_grabbed)
{
    bool is_grabbed = SDL_GetRelativeMouseMode();
    if (will_be_grabbed || is_grabbed) {
        // If the mouse is initially outside the window, move it to the middle,
        // so that clicks are handled by the window (instead making it lose
        // focus, at least with macOS hosts).
        int window_x, window_y, window_width, window_height, mouse_x, mouse_y;
        SDL_GetWindowPosition(impl->display_wnd, &window_x, &window_y);
        SDL_GetWindowSize(impl->display_wnd, &window_width, &window_height);
        SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
        if (mouse_x < window_x || mouse_x >= window_x + window_width ||
                mouse_y < window_y || mouse_y >= window_y + window_height) {
            if (is_grabbed)
                SDL_SetRelativeMouseMode(SDL_FALSE);
            SDL_WarpMouseInWindow(impl->display_wnd, window_width / 2, window_height / 2);
            if (is_grabbed)
                SDL_SetRelativeMouseMode(SDL_TRUE);
        }
    }
}

void Display::update_window_title()
{
    std::string old_window_title = SDL_GetWindowTitle(impl->display_wnd);

    int width, height;
    SDL_GetWindowSize(impl->display_wnd, &width, &height);
    bool is_grabbed = SDL_GetRelativeMouseMode();

    std::string new_window_title = "DingusPPC Display " +
        std::to_string(impl->display_w) + "x" + std::to_string(impl->display_h)
        + " " + std::to_string(int(std::round(impl->renderer_scale_x * 100))) + "%";
    if (is_grabbed)
        new_window_title += " (Grabbed, Ctrl+G to release)";
    else if (!g_auto_grab_mouse || impl->guest_cursor_drawn)
        new_window_title += " (Press Ctrl+G to grab)";

    if (new_window_title != old_window_title)
        SDL_SetWindowTitle(impl->display_wnd, new_window_title.c_str());
}

void Display::blank() {
    if (pippin_cpu_owner::active() && pippin_cpu_owner::is_worker_thread()) {
        // Preserve a blank transition as an ordered UI command. Repeated blank
        // refreshes coalesce, but a subsequent unblank frame cannot erase it.
        if (impl->guest_blanked) return;
        impl->guest_blanked = true;
        auto q = impl->mailbox;
        {
            std::lock_guard<std::mutex> lock(q->mutex);
            ++q->generation;
            if (q->latest || q->pool_latest) ++q->replaced;
            q->latest.reset();
            q->pool_latest.reset();
            q->scheduled = false;
            ++q->submitted;
        }
        pippin_cpu_owner::post_main([q] {
            if (q->owner) {
                q->owner->blank();
                ++q->blanked;
            }
        });
        return;
    }
    SDL_SetRenderDrawColor(impl->renderer, 0, 0, 0, 255);
    SDL_RenderClear(impl->renderer);
    SDL_RenderPresent(impl->renderer);
}

void Display::update(std::function<void(uint8_t *dst_buf, int dst_pitch)> convert_fb_cb,
                     std::function<void(uint8_t *dst_buf, int dst_pitch)> cursor_ovl_cb,
                     bool draw_hw_cursor, int cursor_x, int cursor_y,
                     bool fb_known_to_be_changed) {
    if (pippin_cpu_owner::active() && !pippin_cpu_owner::is_worker_thread())
        throw std::logic_error("Framebuffer conversion outside guest owner");
    if (pippin_cpu_owner::active() && pippin_cpu_owner::is_worker_thread()) {
        if (impl->raw_enabled && !impl->pool_enabled) ++impl->raw_fallback;
        if (impl->pool_qualified) {
            this->update_pooled(std::move(convert_fb_cb), std::move(cursor_ovl_cb),
                                draw_hw_cursor, cursor_x, cursor_y);
            return;
        }
        impl->guest_blanked = false;
        const int width = impl->frame_w, height = impl->frame_h;
        if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4 ||
            size_t(width) > std::numeric_limits<size_t>::max() / size_t(height) / 4)
            throw std::runtime_error("Invalid worker framebuffer dimensions");
        auto frame = std::make_shared<Impl::Frame>();
        frame->width = width;
        frame->height = height;
        frame->draw_cursor = draw_hw_cursor;
        frame->cursor_x = cursor_x;
        frame->cursor_y = cursor_y;
        frame->cursor = impl->guest_cursor;
        frame->pixels.resize(size_t(width) * size_t(height));
        // These callbacks and all source bytes/palette reads stay guest-owned.
        convert_fb_cb(reinterpret_cast<uint8_t*>(frame->pixels.data()), width * 4);
        if (cursor_ovl_cb)
            cursor_ovl_cb(reinterpret_cast<uint8_t*>(frame->pixels.data()), width * 4);
        auto q = impl->mailbox;
        bool schedule = false;
        uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(q->mutex);
            frame->generation = generation = q->generation;
            if (q->latest) ++q->replaced;
            q->latest = std::move(frame);
            ++q->submitted;
            if (!q->scheduled) q->scheduled = schedule = true;
        }
        if (schedule) pippin_cpu_owner::post_main([q, generation] {
            if (q->owner) q->owner->present_pending_frame(generation);
        });
        return;
    }
    uint8_t*    dst_buf = nullptr;
    int         dst_pitch;

    SDL_LockTexture(impl->disp_texture, NULL, (void **)&dst_buf, &dst_pitch);

    // texture update callback to get ARGB data from guest framebuffer
    convert_fb_cb(dst_buf, dst_pitch);

    // overlay cursor data if requested
    if (cursor_ovl_cb != nullptr)
        cursor_ovl_cb(dst_buf, dst_pitch);

    SDL_UnlockTexture(impl->disp_texture);
    this->render_frame(draw_hw_cursor, cursor_x, cursor_y);
}

bool Display::raw_frames_enabled() const {
    return impl->raw_enabled && !impl->pool_enabled &&
           pippin_cpu_owner::active() && pippin_cpu_owner::is_worker_thread();
}

void Display::update_raw(std::unique_ptr<const pippin_indexed::Snapshot> snapshot,
                         bool draw_hw_cursor, int cursor_x, int cursor_y) {
    if (!this->raw_frames_enabled() || !snapshot ||
        snapshot->width() != impl->frame_w || snapshot->height() != impl->frame_h)
        throw std::logic_error("Invalid raw framebuffer handoff");
    impl->guest_blanked = false;
    auto frame = std::make_shared<Impl::Frame>();
    frame->width = snapshot->width(); frame->height = snapshot->height();
    frame->draw_cursor = draw_hw_cursor;
    frame->cursor_x = cursor_x; frame->cursor_y = cursor_y;
    frame->cursor = impl->guest_cursor;
    impl->raw_bytes += snapshot->raw_bytes();
    frame->indexed = std::move(snapshot);
    ++impl->raw_submitted;
    auto q = impl->mailbox;
    bool schedule = false;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(q->mutex);
        frame->generation = generation = q->generation;
        if (q->latest) ++q->replaced;
        q->latest = std::move(frame);
        ++q->submitted;
        if (!q->scheduled) q->scheduled = schedule = true;
    }
    if (schedule) pippin_cpu_owner::post_main([q, generation] {
        if (q->owner) q->owner->present_pending_frame(generation);
    });
}

void Display::update_pooled(std::function<void(uint8_t*, int)> convert_fb_cb,
                            std::function<void(uint8_t*, int)> cursor_ovl_cb,
                            bool draw_hw_cursor, int cursor_x, int cursor_y) {
    impl->guest_blanked = false;
    if (!impl->pixel_pool || impl->pixel_pool->width() != impl->frame_w ||
            impl->pixel_pool->height() != impl->frame_h)
        throw std::logic_error("Frame-pixel pool is not configured");
    auto writing = impl->pixel_pool->try_acquire();
    if (!writing) throw std::logic_error("No free slot in three-buffer frame-pixel pool");
    // Selected Pippin Taos 8/15-bit callbacks write every output pixel before
    // the partial cursor overlay. No clear/resize/allocation occurs per frame.
    convert_fb_cb(reinterpret_cast<uint8_t*>(writing.pixels()), writing.pitch_bytes());
    if (cursor_ovl_cb)
        cursor_ovl_cb(reinterpret_cast<uint8_t*>(writing.pixels()), writing.pitch_bytes());
    auto frame = std::make_unique<Impl::PooledFrame>();
    frame->draw_cursor = draw_hw_cursor;
    frame->cursor_x = cursor_x; frame->cursor_y = cursor_y;
    frame->cursor = impl->guest_cursor;
    frame->pixels = std::move(writing).publish();
    auto q = impl->mailbox;
    bool schedule = false;
    uint64_t generation;
    {
        // Lock order is mailbox then pool (on discarded READY destruction).
        // No pool operation retains its mutex while acquiring the mailbox.
        std::lock_guard<std::mutex> lock(q->mutex);
        frame->generation = generation = q->generation;
        if (q->pool_latest) ++q->replaced;
        q->pool_latest = std::move(frame);
        ++q->submitted;
        if (!q->scheduled) q->scheduled = schedule = true;
    }
    if (schedule) pippin_cpu_owner::post_main([q, generation] {
        if (q->owner) q->owner->present_pending_pooled_frame(generation);
    });
}

void Display::render_frame(bool draw_hw_cursor, int cursor_x, int cursor_y) {
    SDL_RenderClear(impl->renderer);
    SDL_RenderCopy(impl->renderer, impl->disp_texture, NULL, &impl->dest_rect);

if (g_auto_grab_mouse) {
    bool is_grabbed = SDL_GetRelativeMouseMode();

    // The guest draws its own cursor into the framebuffer (hardware cursor).
    // When such a cursor session starts, auto-grab the host mouse so the
    // pointer cannot leave the window and the guest cursor can reach every
    // screen area. When the session ends, release an automatic grab again so
    // the user can move the pointer to other windows. A manual grab (Ctrl+G)
    // is sticky and is neither grabbed nor released automatically.
    if (draw_hw_cursor) {
        if (!impl->guest_cursor_drawn && !impl->manual_grab && !is_grabbed) {
            this->update_mouse_grab(true);
            SDL_SetRelativeMouseMode(SDL_TRUE);
            is_grabbed = true;
        }
    } else {
        if (!impl->manual_grab && is_grabbed) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
            is_grabbed = false;
        }
    }

    // The guest draws its own cursor into the framebuffer. Even without a
    // grab, keep the SDL host pointer hidden so the user never sees a second
    // cursor that does not track the guest one. When the guest cursor is not
    // drawn (e.g. firmware), show the host pointer so the user can aim and
    // click.
    bool want_host_cursor = !draw_hw_cursor;
    if (impl->show_host_cursor != want_host_cursor) {
        impl->show_host_cursor = want_host_cursor;
        SDL_ShowCursor(want_host_cursor ? SDL_ENABLE : SDL_DISABLE);
    }

    // Let the window title reflect the current grab/cursor state.
    if (is_grabbed != impl->was_grabbed || draw_hw_cursor != impl->guest_cursor_drawn) {
        impl->was_grabbed = is_grabbed;
        impl->guest_cursor_drawn = draw_hw_cursor;
        this->update_window_title();
    }
}

    // draw HW cursor if enabled
    if (draw_hw_cursor) {
        impl->cursor_rect.x = cursor_x * impl->renderer_scale_x + impl->dest_rect.x;
        impl->cursor_rect.y = cursor_y * impl->renderer_scale_y + impl->dest_rect.y;
        SDL_RenderCopy(impl->renderer, impl->cursor_texture, NULL, &impl->cursor_rect);
    }

    SDL_RenderPresent(impl->renderer);
}

void Display::present_pending_frame(uint64_t generation) {
    std::shared_ptr<const Impl::Frame> frame;
    {
        std::lock_guard<std::mutex> lock(impl->mailbox->mutex);
        if (generation != impl->mailbox->generation) {
            ++impl->mailbox->stale;
            return;
        }
        impl->mailbox->scheduled = false;
        frame = std::move(impl->mailbox->latest);
        if (!frame) return;
        if (frame->generation != impl->mailbox->generation) {
            ++impl->mailbox->stale;
            return;
        }
    }
    if (frame->width != impl->display_w || frame->height != impl->display_h) {
        ++impl->mailbox->stale;
        return;
    }
    uint8_t* dst = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(impl->disp_texture, nullptr, reinterpret_cast<void**>(&dst), &pitch))
        throw std::runtime_error(std::string("Worker framebuffer lock failed: ") + SDL_GetError());
    if (!dst || pitch < frame->width * 4) {
        SDL_UnlockTexture(impl->disp_texture);
        throw std::runtime_error("Worker framebuffer texture has an invalid pitch");
    }
    if (frame->indexed) {
        if (size_t(frame->height) > std::numeric_limits<size_t>::max() / size_t(pitch) ||
            !frame->indexed->write_argb(dst, size_t(pitch) * size_t(frame->height), pitch)) {
            SDL_UnlockTexture(impl->disp_texture);
            throw std::runtime_error("Raw framebuffer destination rejected");
        }
    } else {
        for (int y = 0; y < frame->height; ++y)
            std::memcpy(dst + size_t(y) * pitch,
                frame->pixels.data() + size_t(y) * frame->width, size_t(frame->width) * 4);
    }
    SDL_UnlockTexture(impl->disp_texture);
    if (frame->cursor && frame->cursor != impl->uploaded_cursor) {
        const auto& cursor = *frame->cursor;
        if (impl->cursor_texture) SDL_DestroyTexture(impl->cursor_texture);
        impl->cursor_texture = SDL_CreateTexture(impl->renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, cursor.width, cursor.height);
        if (!impl->cursor_texture)
            throw std::runtime_error(std::string("Worker cursor texture failed: ") + SDL_GetError());
        SDL_SetTextureBlendMode(impl->cursor_texture, SDL_BLENDMODE_BLEND);
        if (SDL_LockTexture(impl->cursor_texture, nullptr, reinterpret_cast<void**>(&dst), &pitch))
            throw std::runtime_error(std::string("Worker cursor lock failed: ") + SDL_GetError());
        if (!dst || pitch < cursor.width * 4) {
            SDL_UnlockTexture(impl->cursor_texture);
            throw std::runtime_error("Worker cursor texture has an invalid pitch");
        }
        for (int y = 0; y < cursor.height; ++y)
            std::memcpy(dst + size_t(y) * pitch,
                cursor.pixels.data() + size_t(y) * cursor.width, size_t(cursor.width) * 4);
        SDL_UnlockTexture(impl->cursor_texture);
        impl->uploaded_cursor = frame->cursor;
    }
    if (frame->cursor) {
        impl->cursor_rect.w = frame->cursor->width * impl->renderer_scale_x;
        impl->cursor_rect.h = frame->cursor->height * impl->renderer_scale_y;
    }
    this->render_frame(frame->draw_cursor, frame->cursor_x, frame->cursor_y);
    if (frame->indexed) ++impl->raw_presented;
    ++impl->mailbox->presented;
}

void Display::present_pending_pooled_frame(uint64_t generation) {
    std::unique_ptr<Impl::PooledFrame> frame;
    {
        std::lock_guard<std::mutex> lock(impl->mailbox->mutex);
        if (generation != impl->mailbox->generation) {
            ++impl->mailbox->stale;
            return;
        }
        impl->mailbox->scheduled = false;
        frame = std::move(impl->mailbox->pool_latest);
        if (!frame) return;
        if (frame->generation != impl->mailbox->generation) {
            ++impl->mailbox->stale;
            return;
        }
    }
    auto reading = std::move(frame->pixels).read();
    if (reading.width() != impl->display_w || reading.height() != impl->display_h) {
        ++impl->mailbox->stale;
        return;
    }
    uint8_t* dst = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(impl->disp_texture, nullptr, reinterpret_cast<void**>(&dst), &pitch))
        throw std::runtime_error(std::string("Worker framebuffer lock failed: ") + SDL_GetError());
    if (!dst || pitch < reading.width() * 4) {
        SDL_UnlockTexture(impl->disp_texture);
        throw std::runtime_error("Worker framebuffer texture has an invalid pitch");
    }
    for (int y = 0; y < reading.height(); ++y)
        std::memcpy(dst + size_t(y) * pitch,
            reading.pixels() + size_t(y) * reading.width(), size_t(reading.width()) * 4);
    SDL_UnlockTexture(impl->disp_texture);
    if (frame->cursor && frame->cursor != impl->uploaded_cursor) {
        const auto& cursor = *frame->cursor;
        if (impl->cursor_texture) SDL_DestroyTexture(impl->cursor_texture);
        impl->cursor_texture = SDL_CreateTexture(impl->renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, cursor.width, cursor.height);
        if (!impl->cursor_texture)
            throw std::runtime_error(std::string("Worker cursor texture failed: ") + SDL_GetError());
        SDL_SetTextureBlendMode(impl->cursor_texture, SDL_BLENDMODE_BLEND);
        if (SDL_LockTexture(impl->cursor_texture, nullptr, reinterpret_cast<void**>(&dst), &pitch))
            throw std::runtime_error(std::string("Worker cursor lock failed: ") + SDL_GetError());
        if (!dst || pitch < cursor.width * 4) {
            SDL_UnlockTexture(impl->cursor_texture);
            throw std::runtime_error("Worker cursor texture has an invalid pitch");
        }
        for (int y = 0; y < cursor.height; ++y)
            std::memcpy(dst + size_t(y) * pitch,
                cursor.pixels.data() + size_t(y) * cursor.width, size_t(cursor.width) * 4);
        SDL_UnlockTexture(impl->cursor_texture);
        impl->uploaded_cursor = frame->cursor;
    }
    if (frame->cursor) {
        impl->cursor_rect.w = frame->cursor->width * impl->renderer_scale_x;
        impl->cursor_rect.h = frame->cursor->height * impl->renderer_scale_y;
    }
    this->render_frame(frame->draw_cursor, frame->cursor_x, frame->cursor_y);
    ++impl->mailbox->presented;
}

void Display::update_skipped() {
    // SDL implementation does not care about skipped updates.
}

void Display::setup_hw_cursor(std::function<void(uint8_t *dst_buf, int dst_pitch)> draw_hw_cursor,
                              int cursor_width, int cursor_height) {
    if (pippin_cpu_owner::active() && !pippin_cpu_owner::is_worker_thread())
        throw std::logic_error("Cursor conversion outside guest owner");
    if (pippin_cpu_owner::enabled()) {
        if (cursor_width <= 0 || cursor_height <= 0 ||
            cursor_width > std::numeric_limits<int>::max() / 4 ||
            size_t(cursor_width) > std::numeric_limits<size_t>::max() / size_t(cursor_height) / 4)
            throw std::runtime_error("Invalid worker cursor dimensions");
        auto cursor = std::make_shared<Impl::CursorImage>();
        cursor->width = cursor_width;
        cursor->height = cursor_height;
        cursor->pixels.resize(size_t(cursor_width) * size_t(cursor_height));
        draw_hw_cursor(reinterpret_cast<uint8_t*>(cursor->pixels.data()), cursor_width * 4);
        impl->guest_cursor = cursor;
        if (pippin_cpu_owner::active() && pippin_cpu_owner::is_worker_thread()) return;
        // Startup may configure a cursor before run(); consume the same owned
        // image below instead of invoking the guest callback twice.
        draw_hw_cursor = [cursor](uint8_t* dst, int pitch) {
            for (int y = 0; y < cursor->height; ++y)
                std::memcpy(dst + size_t(y) * pitch,
                    cursor->pixels.data() + size_t(y) * cursor->width, size_t(cursor->width) * 4);
        };
    }
    uint8_t*    dst_buf = nullptr;
    int         dst_pitch;

    if (impl->cursor_texture)
        SDL_DestroyTexture(impl->cursor_texture);

    impl->cursor_texture = SDL_CreateTexture(
        impl->renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        cursor_width, cursor_height
    );

    if (impl->cursor_texture == NULL)
        ABORT_F("SDL_CreateTexture for HW cursor failed with %s", SDL_GetError());

    SDL_LockTexture(impl->cursor_texture, NULL, (void **)&dst_buf, &dst_pitch);
    SDL_SetTextureBlendMode(impl->cursor_texture, SDL_BLENDMODE_BLEND);
    draw_hw_cursor(dst_buf, dst_pitch);
    SDL_UnlockTexture(impl->cursor_texture);

    impl->cursor_rect.x = 0;
    impl->cursor_rect.y = 0;
    impl->cursor_rect.w = cursor_width * impl->renderer_scale_x;
    impl->cursor_rect.h = cursor_height * impl->renderer_scale_y;
}
