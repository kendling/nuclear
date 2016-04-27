/*
 * Copyright 2013-2014  Giulio Camuffo <giuliocamuffo@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>

#include <wayland-server.h>

#include <weston/compositor.h>
#include <weston/matrix.h>
// #include <weston/config-parser.h>

#include "desktop-shell.h"
#include "wayland-desktop-shell-server-protocol.h"
#include "shellsurface.h"
#include "binding.h"

#include "inputpanel.h"
#include "shellseat.h"
#include "workspace.h"
#include "wl_shell/wlshell.h"
#include "wl_shell/wlshellsurface.h"
#include "xdg_shell/xdgshell.h"
#include "xwlshell.h"
#include "desktopshellwindow.h"
#include "desktopshellworkspace.h"
#include "animation.h"
#include "settings.h"
#include "settingsinterface.h"
#include "sessionmanager.h"
#include "dropdown.h"
#include "screenshooter.h"
#include "signal.h"

class Splash {
public:
    Splash() {}
    void addOutput(weston_view *s, wl_resource *res)
    {
        splashes.push_back(new splash(this, s, res));
    }
    void fadeOut()
    {
        for (splash *s: splashes) {
            s->fadeAnimation->setStart(1.f);
            s->fadeAnimation->setTarget(0.f);
            s->fadeAnimation->run(s->view->output, 200, Animation::Flags::SendDone);
        }
    }

private:
    struct splash {
        weston_view *view;
        Animation *fadeAnimation;
        wl_resource *resource;
        WlListener surfaceListener;
        Splash *parent;

        splash(Splash *p, weston_view *v, wl_resource *r) {
            parent = p;
            view = v;
            resource = r;
            fadeAnimation = new Animation;

            fadeAnimation->updateSignal->connect(this, &splash::setAlpha);
            fadeAnimation->doneSignal->connect(this, &splash::done);
            surfaceListener.listen(&v->surface->destroy_signal);
            surfaceListener.signal->connect(this, &splash::surfaceDestroyed);
        }

        void setAlpha(float a)
        {
            view->alpha = a;
            weston_view_geometry_dirty(view);
            weston_surface_damage(view->surface);
        }
        void done()
        {
            desktop_shell_splash_send_done(resource);
        }
        void surfaceDestroyed(void *data)
        {
            delete fadeAnimation;
            parent->splashes.remove(this);
            delete this;
        }
    };
    std::list<splash *> splashes;
};

struct Client {
    ~Client() {
        destroyListener.reset();
    }

    wl_client *client;
    WlListener destroyListener;
};

DesktopShell::DesktopShell(struct weston_compositor *ec)
            : Shell(ec)
            , m_sessionManager(nullptr)
            , m_pingTimer(500)
{
    m_pingTimer.triggered.connect(this, &DesktopShell::pingTimerTimeout);
}

DesktopShell::~DesktopShell()
{
    delete m_splash;
    for (auto value: m_trustedClients) {
        for (Client *c: value.second) {
            delete c;
        }
    }
    delete m_moveBinding;
    delete m_resizeBinding;
    delete m_closeBinding;
    delete m_prevWsBinding;
    delete m_nextWsBinding;
    delete m_quitBinding;

    if (m_sessionManager) {
        std::list<pid_t> pids;
        for (ShellSurface *s: surfaces()) {
            WlShellSurface *w = s->findInterface<WlShellSurface>();
            if (w) {
                wl_client *c = wl_resource_get_client(w->resource());
                pid_t pid;
                wl_client_get_credentials(c, &pid, nullptr, nullptr);
                pids.push_back(pid);
            }
        }
        for (wl_client *c: findInterface<Dropdown>()->boundClients()) {
            pid_t pid;
            wl_client_get_credentials(c, &pid, nullptr, nullptr);
            pids.push_back(pid);
        }
        m_sessionManager->save(pids);
    }
}

void DesktopShell::init()
{
    Shell::init();

    if (!wl_global_create(compositor()->wl_display, &desktop_shell_interface, 1, this,
        [](struct wl_client *client, void *data, uint32_t version, uint32_t id) { static_cast<DesktopShell *>(data)->bind(client, version, id); }))
        return;

    weston_seat *seat;
    wl_list_for_each(seat, &compositor()->seat_list, link) {
        ShellSeat *shseat = ShellSeat::shellSeat(seat);
        shseat->pointerMotionSignal.connect(this, &DesktopShell::pointerMotion);
    }

    m_moveBinding = new Binding();
    m_moveBinding->buttonTriggered.connect(this, &DesktopShell::moveBinding);
    m_resizeBinding = new Binding();
    m_resizeBinding->buttonTriggered.connect(this, &DesktopShell::resizeBinding);
    m_closeBinding = new Binding();
    m_closeBinding->keyTriggered.connect(this, &DesktopShell::closeBinding);
    m_prevWsBinding = new Binding();
    m_prevWsBinding->keyTriggered.connect([this](weston_seat *seat, uint32_t time, uint32_t key) {
        selectPreviousWorkspace();
    });
    m_nextWsBinding = new Binding();
    m_nextWsBinding->keyTriggered.connect([this](weston_seat *seat, uint32_t time, uint32_t key) {
        selectNextWorkspace();
    });
    m_quitBinding = new Binding();
    m_quitBinding->keyTriggered.connect([this](weston_seat *seat, uint32_t time, uint32_t key) {
        Shell::quit();
    });

    if (!wl_global_create(compositor()->wl_display, &desktop_shell_splash_interface, 1, this,
        [](wl_client *client, void *data, uint32_t version, uint32_t id) { static_cast<DesktopShell *>(data)->bindSplash(client, version, id); }))
        return;

    WlShell *wls = new WlShell;
    wls->surfaceResponsivenessChangedSignal.connect(this, &DesktopShell::surfaceResponsivenessChanged);
    addInterface(wls);
    addInterface(new XWlShell);
    addInterface(new SettingsInterface);
    addInterface(new Dropdown);
    XdgShell *xdg = new XdgShell;
    xdg->surfaceResponsivenessChangedSignal.connect(this, &DesktopShell::surfaceResponsivenessChanged);
    addInterface(xdg);
    addInterface(new Screenshooter);

    m_inputPanel = new InputPanel(compositor()->wl_display);
    m_splash = new Splash;
}

void DesktopShell::setGrabCursor(Cursor cursor)
{
    desktop_shell_send_grab_cursor(m_child.desktop_shell, (uint32_t)cursor);
}

ShellSurface *DesktopShell::createShellSurface(weston_surface *surface, const weston_shell_client *client)
{
    ShellSurface *s = Shell::createShellSurface(surface, client);
    s->addInterface(new DesktopShellWindow);
    return s;
}

struct BusyGrab : public ShellGrab {
    void focus() override
    {
        wl_fixed_t sx, sy;
        weston_view *view = weston_compositor_pick_view(pointer()->seat->compositor, pointer()->x, pointer()->y, &sx, &sy);

        if (surface->view() != view) {
            delete this;
        }
    }
    void button(uint32_t time, uint32_t button, uint32_t state) override
    {
        weston_seat *seat = pointer()->seat;

        if (surface && button == BTN_LEFT && state) {
            ShellSeat::shellSeat(seat)->activate(surface);
            surface->dragMove(seat);
        } else if (surface && button == BTN_RIGHT && state) {
            ShellSeat::shellSeat(seat)->activate(surface);
//         surface_rotate(grab->surface, &seat->seat);
        }
    }

    ShellSurface *surface;
};

void DesktopShell::setBusyCursor(ShellSurface *surface, struct weston_seat *seat)
{
    BusyGrab *grab = new BusyGrab;
    if (!grab && grab->pointer())
        return;

    grab->surface = surface;
    grab->start(seat, Cursor::Busy);
}

void DesktopShell::endBusyCursor(struct weston_seat *seat)
{
    ShellGrab *grab = ShellGrab::fromGrab(seat->pointer_state->grab);

    if (dynamic_cast<BusyGrab *>(grab)) {
        delete grab;
    }
}

bool DesktopShell::isTrusted(wl_client *client, const char *interface) const
{
    if (client == m_child.client) {
        return true;
    }

    auto it = m_trustedClients.find(interface);
    if (it == m_trustedClients.end()) {
        return false;
    }

    for (Client *c: it->second) {
        if (c->client == client) {
            return true;
        }
    }

    return false;
}

void DesktopShell::trustedClientDestroyed(void *data)
{
    wl_client *client = static_cast<wl_client *>(data);
    for (auto v: m_trustedClients) {
        std::list<Client *> &list = m_trustedClients[v.first];
        for (auto i = list.begin(); i != list.end(); ++i) {
            if ((*i)->client == client) {
                delete *i;
                list.erase(i);
                return;
            }
        }
    }
}

void DesktopShell::pointerMotion(ShellSeat *seat, weston_pointer *pointer)
{
    if (!m_child.desktop_shell || m_pingTimer.isRunning()) {
        return;
    }

    wl_display *display = wl_client_get_display(shellClient());
    m_pingSerial = wl_display_next_serial(display);
    m_pingTimer.start();

    desktop_shell_send_ping(m_child.desktop_shell, m_pingSerial);
    wl_client_flush(shellClient());
}

void DesktopShell::pingTimerTimeout()
{
    printf("The shell client is unresponsive, restarting it...\n");
    m_pingTimer.stop();
    wl_client_destroy(m_child.client);
}

void DesktopShell::sendInitEvents()
{
    for (uint i = 0; i < numWorkspaces(); ++i) {
        Workspace *ws = workspace(i);
        DesktopShellWorkspace *dws = ws->findInterface<DesktopShellWorkspace>();
        dws->init(m_child.client);
        workspaceAdded(dws);
    }

    for (ShellSurface *shsurf: surfaces()) {
        DesktopShellWindow *w = shsurf->findInterface<DesktopShellWindow>();
        if (w) {
            w->create();
        }
    }

    m_outputs.clear();
    weston_output *out;
    wl_list_for_each(out, &compositor()->output_list, link) {
        wl_resource *resource;
        wl_resource_for_each(resource, &out->resource_list) {
            if (wl_resource_get_client(resource) == m_child.client) {
                IRect2D rect = windowsArea(out);
                m_outputs.push_back({ out, resource, rect });
                desktop_shell_send_desktop_rect(m_child.desktop_shell, resource, rect.x, rect.y, rect.width, rect.height);
                break;
            }
        }
    }
}

void DesktopShell::workspaceAdded(DesktopShellWorkspace *ws)
{
    desktop_shell_send_workspace_added(m_child.desktop_shell, ws->resource(), ws->workspace()->isActive());
}

void DesktopShell::surfaceResponsivenessChanged(ShellSurface *shsurf, bool responsiveness)
{
    weston_seat *seat;
    wl_list_for_each(seat, &compositor()->seat_list, link) {
        if (seat->pointer_state->focus == shsurf->view()) {
            responsiveness ? endBusyCursor(seat) : setBusyCursor(shsurf, seat);
        }
    }
}

void DesktopShell::bind(struct wl_client *client, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &desktop_shell_interface, version, id);

    if (client == m_child.client) {
        wl_resource_set_implementation(resource, &m_desktop_shell_implementation, this,
                                       [](struct wl_resource *resource) { static_cast<DesktopShell *>(wl_resource_get_user_data(resource))->unbind(resource); });
        m_child.desktop_shell = resource;

        sendInitEvents();
        desktop_shell_send_load(resource);
        return;
    }

    wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "permission to bind desktop_shell denied");
    wl_resource_destroy(resource);
}

void DesktopShell::bindSplash(wl_client *client, uint32_t version, uint32_t id)
{
    wl_resource *resource = wl_resource_create(client, &desktop_shell_splash_interface, version, id);

    if (isTrusted(client, "desktop_shell_splash")) {
        wl_resource_set_implementation(resource, &s_desktop_shell_splash_implementation, this, nullptr);
        return;
    }

    wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "permission to bind desktop_shell_splash denied");
    wl_resource_destroy(resource);
}

void DesktopShell::unbind(struct wl_resource *resource)
{
    m_child.desktop_shell = nullptr;
}

void DesktopShell::moveBinding(struct weston_seat *seat, uint32_t time, uint32_t button)
{
    weston_view *view = seat->pointer_state->focus;
    if (!view) {
        return;
    }

    ShellSurface *shsurf = getShellSurface(view->surface);
    if (!shsurf || shsurf->isFullscreen() || shsurf->isMaximized()) {
        return;
    }

    shsurf = shsurf->topLevelParent();
    if (shsurf) {
        shsurf->dragMove(seat);
    }
}

void DesktopShell::resizeBinding(weston_seat *seat, uint32_t time, uint32_t button)
{
    weston_surface *surface = weston_surface_get_main_surface(seat->pointer_state->focus->surface);
    if (!surface) {
        return;
    }

    ShellSurface *shsurf = getShellSurface(surface);
    if (!shsurf || shsurf->isFullscreen() || shsurf->isMaximized()) {
        return;
    }

    if (ShellSurface *top = shsurf->topLevelParent()) {
        int32_t x, y;
        weston_view_from_global(shsurf->view(), wl_fixed_to_int(seat->pointer_state->grab_x), wl_fixed_to_int(seat->pointer_state->grab_y), &x, &y);

        pixman_box32_t *bbox = pixman_region32_extents(&surface->input);

        ShellSurface::Edges edges = ShellSurface::Edges::None;
        int32_t w = surface->width / 3;
        if (w > 20) w = 20;
        w += bbox->x1;

        if (x < w)
            edges |= ShellSurface::Edges::Left;
        else if (x < surface->width - w)
            edges |= ShellSurface::Edges::None;
        else
            edges |= ShellSurface::Edges::Right;

        int32_t h = surface->height / 3;
        if (h > 20) h = 20;
        h += bbox->y1;
        if (y < h)
            edges |= ShellSurface::Edges::Top;
        else if (y < surface->height - h)
            edges |= ShellSurface::Edges::None;
        else
            edges |= ShellSurface::Edges::Bottom;

        top->dragResize(seat, edges);
    }
}

void DesktopShell::closeBinding(weston_seat *seat, uint32_t time, uint32_t button)
{
    weston_surface *surface = weston_surface_get_main_surface(seat->keyboard_state->focus);
    if (!surface) {
        return;
    }

    ShellSurface *shsurf = getShellSurface(surface);
    if (!shsurf || shsurf->isFullscreen() || shsurf->isMaximized()) {
        return;
    }

    shsurf->close();
}

void DesktopShell::setBackground(struct wl_client *client, struct wl_resource *resource, struct wl_resource *output_resource,
                                 struct wl_resource *surface_resource)
{
    struct weston_surface *surface = static_cast<weston_surface *>(wl_resource_get_user_data(surface_resource));

    setBackgroundSurface(surface, static_cast<weston_output *>(wl_resource_get_user_data(output_resource)));

    desktop_shell_send_configure(resource, 0,
                                 surface_resource,
                                 surface->output->width,
                                 surface->output->height);
}

class Panel;
class PanelGrab : public ShellGrab
{
public:
    Panel *panel;

    void focus() override {}
    void motion(uint32_t time, weston_pointer_motion_event *event) override;
    void button(uint32_t time, uint32_t button, uint32_t state) override;
};

class Panel {
public:
    Panel(wl_resource *res, weston_surface *s, DesktopShell *shell, Shell::PanelPosition p)
        : m_resource(res)
        , m_surface(s)
        , m_shell(shell)
        , m_pos(p)
        , m_grab(nullptr)
    {
        wl_resource_set_implementation(m_resource, &s_implementation, this, panelDestroyed);
    }

    void move(wl_client *client, wl_resource *resource)
    {
        m_grab = new PanelGrab;
        m_grab->panel = this;
        weston_seat *seat = container_of(m_shell->compositor()->seat_list.next, weston_seat, link);
        m_grab->start(seat);
    }
    void setPosition(wl_client *client, wl_resource *resource, uint32_t pos)
    {
        m_pos = (Shell::PanelPosition)pos;
        m_shell->addPanelSurface(m_surface, m_surface->output, m_pos);
    }

    static void panelDestroyed(wl_resource *res)
    {
        Panel *_this = static_cast<Panel *>(wl_resource_get_user_data(res));
        delete _this->m_grab;
        delete _this;
    }

    wl_resource *m_resource;
    weston_surface *m_surface;
    DesktopShell *m_shell;
    Shell::PanelPosition m_pos;
    PanelGrab *m_grab;

    static struct desktop_shell_panel_interface s_implementation;
};

void PanelGrab::motion(uint32_t time, weston_pointer_motion_event *event)
{
    weston_pointer_move(pointer(), event);

    weston_output *out = panel->m_surface->output;
    if (!out) {
        delete this;
        return;
    }

    int x = wl_fixed_to_int(pointer()->x);
    int y = wl_fixed_to_int(pointer()->y);
    Shell::PanelPosition pos = panel->m_pos;

    const int size = 30;
    bool top = y <= out->y + size;
    bool left = x <= out->x + size;
    bool right = x >= out->x + out->width - size;
    bool bottom = y >= out->y + out->height - size;

    bool nomove = (top && pos == Shell::PanelPosition::Top) || (left && pos == Shell::PanelPosition::Left) ||
                  (right && pos == Shell::PanelPosition::Right) || (bottom && pos == Shell::PanelPosition::Bottom);

    if (!nomove) {
        if (top) {
            pos = Shell::PanelPosition::Top;
        } else if (left) {
            pos = Shell::PanelPosition::Left;
        } else if (right) {
            pos = Shell::PanelPosition::Right;
        } else if (bottom) {
            pos = Shell::PanelPosition::Bottom;
        }
    }

    // Send the output too?
    desktop_shell_panel_send_moved(panel->m_resource, (uint32_t)pos);

    wl_resource *resource;
    wl_resource_for_each(resource, &pointer()->focus_client->pointer_resources) {
        wl_fixed_t sx, sy;
        weston_view_from_global_fixed(pointer()->focus, pointer()->x, pointer()->y, &sx, &sy);
        wl_pointer_send_motion(resource, time, sx, sy);
    }
}

void PanelGrab::button(uint32_t time, uint32_t button, uint32_t state)
{
    wl_resource *resource;
    wl_resource_for_each(resource, &pointer()->focus_client->pointer_resources) {
        struct wl_display *display = wl_client_get_display(wl_resource_get_client(resource));
        uint32_t serial = wl_display_get_serial(display);
        wl_pointer_send_button(resource, serial, time, button, state);
    }

    panel->m_grab = nullptr;
    delete this;
}

struct desktop_shell_panel_interface Panel::s_implementation = {
    wrapInterface(&Panel::move),
    wrapInterface(&Panel::setPosition)
};

void DesktopShell::setPanel(wl_client *client, wl_resource *resource, uint32_t id, wl_resource *output_resource, wl_resource *surface_resource, uint32_t pos)
{
    weston_surface *surface = static_cast<weston_surface *>(wl_resource_get_user_data(surface_resource));
    weston_output *output = static_cast<weston_output *>(wl_resource_get_user_data(output_resource));

    if (surface->configure) {
        wl_resource_post_error(surface_resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "surface role already assigned");
        return;
    }

    wl_resource *res = wl_resource_create(client, &desktop_shell_panel_interface, wl_resource_get_version(resource), id);
    new Panel(res, surface, this, (Shell::PanelPosition)pos);


    addPanelSurface(surface, output, (Shell::PanelPosition)pos);
    desktop_shell_send_configure(resource, 0, surface_resource, surface->output->width, surface->output->height);
}

void DesktopShell::panelConfigure(weston_surface *es, int32_t sx, int32_t sy, Shell::PanelPosition pos)
{
    Shell::panelConfigure(es, sx, sy, pos);

    if (es->width == 0) {
        return;
    }

    IRect2D rect = windowsArea(es->output);
    for (Output &out: m_outputs) {
        if (out.output == es->output && out.rect != rect) {
            out.rect = rect;
            desktop_shell_send_desktop_rect(m_child.desktop_shell, out.resource, rect.x, rect.y, rect.width, rect.height);
        }
    }
}

void DesktopShell::setLockSurface(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource)
{
//     struct desktop_shell *shell = resource->data;
//     struct weston_surface *surface = surface_resource->data;
//
//     shell->prepare_event_sent = false;
//
//     if (!shell->locked)
//         return;
//
//     shell->lock_surface = surface;
//
//     shell->lock_surface_listener.notify = handle_lock_surface_destroy;
//     wl_signal_add(&surface_resource->destroy_signal,
//                   &shell->lock_surface_listener);
//
//     surface->configure = lock_surface_configure;
//     surface->configure_private = shell;
}

class PopupGrab : public ShellGrab {
public:
    weston_view *view;
    wl_resource *shsurfResource;
    bool inside;
    uint32_t creationTime;

    void focus() override
    {
        wl_fixed_t sx, sy;
        weston_view *v = weston_compositor_pick_view(pointer()->seat->compositor, pointer()->x, pointer()->y, &sx, &sy);

        inside = v == view;
        if (inside)
            weston_pointer_set_focus(pointer(), view, sx, sy);
    }
    void motion(uint32_t time, weston_pointer_motion_event *event) override
    {
        ShellGrab::motion(time, event);

        wl_resource *resource;
        wl_resource_for_each(resource, &pointer()->focus_client->pointer_resources) {
            wl_fixed_t sx, sy;
            weston_view_from_global_fixed(pointer()->focus, pointer()->x, pointer()->y, &sx, &sy);
            wl_pointer_send_motion(resource, time, sx, sy);
        }
    }
    void button(uint32_t time, uint32_t button, uint32_t state) override
    {
        wl_resource *resource;
        wl_resource_for_each(resource, &pointer()->focus_client->pointer_resources) {
            struct wl_display *display = wl_client_get_display(wl_resource_get_client(resource));
            uint32_t serial = wl_display_get_serial(display);
            wl_pointer_send_button(resource, serial, time, button, state);
        }

        // this time check is to ensure the window doesn't get shown and hidden very fast, mainly because
        // there is a bug in QQuickWindow, which hangs up the process.
        if (!inside && state == WL_POINTER_BUTTON_STATE_RELEASED && time - creationTime > 500) {
            desktop_shell_surface_send_popup_close(shsurfResource);
            wl_resource_destroy(shsurfResource);
        }
    }
};

static void popupGrabDestroyed(wl_resource *res)
{
    delete static_cast<PopupGrab *>(wl_resource_get_user_data(res));
}

struct Popup {
    Popup(weston_view *p, DesktopShell *s, int32_t _x, int32_t _y)
        : parent(p), shell(s), x(_x), y(_y) {}
    weston_view *parent;
    DesktopShell *shell;
    int32_t x, y;
    wl_listener destroyListener;
};

static void popupDestroyed(wl_listener *listener, void *data)
{
    weston_surface *surface = static_cast<weston_surface *>(data);
    delete static_cast<Popup *>(surface->configure_private);
}

void DesktopShell::configurePopup(weston_surface *es, int32_t sx, int32_t sy)
{
    if (es->width == 0)
        return;

    Popup *p = static_cast<Popup *>(es->configure_private);
    DesktopShell *shell= p->shell;
    Layer *layer = &shell->m_panelsLayer;

    weston_view *view = container_of(es->views.next, weston_view, surface_link);
    weston_view_set_position(view, p->parent->geometry.x + p->x, p->parent->geometry.y + p->y);

    if (wl_list_empty(&view->layer_link.link) || view->layer_link.link.next == view->layer_link.link.prev) {
        layer->addSurface(view);
        weston_compositor_schedule_repaint(es->compositor);
    }
}

void DesktopShell::setPopup(wl_client *client, wl_resource *resource, uint32_t id, wl_resource *parent_resource, wl_resource *surface_resource, int x, int y)
{
    weston_surface *parent = static_cast<weston_surface *>(wl_resource_get_user_data(parent_resource));
    weston_surface *surface = static_cast<weston_surface *>(wl_resource_get_user_data(surface_resource));
    weston_view *pv = container_of(parent->views.next, weston_view, surface_link);

    Popup *p = nullptr;
    if (surface->configure == configurePopup) {
        p = static_cast<Popup *>(surface->configure_private);
        p->x = x;
        p->y = y;
        p->parent = pv;
    } else {
        p = new Popup(pv, this, x, y);
        p->destroyListener.notify = popupDestroyed;
        wl_signal_add(&surface->destroy_signal, &p->destroyListener);
    }

    surface->configure = configurePopup;
    surface->configure_private = p;
    surface->output = parent->output;
    weston_view *sv = weston_view_create(surface);;

    PopupGrab *grab = new PopupGrab;
    if (!grab)
        return;

    grab->shsurfResource = wl_resource_create(client, &desktop_shell_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_destructor(grab->shsurfResource, popupGrabDestroyed);
    wl_resource_set_user_data(grab->shsurfResource, grab);

    weston_seat *seat = container_of(compositor()->seat_list.next, weston_seat, link);
    grab->view = sv;
    grab->creationTime = seat->pointer_state->grab_time;

    wl_fixed_t sx, sy;
    weston_view_from_global_fixed(sv, seat->pointer_state->x, seat->pointer_state->y, &sx, &sy);
    weston_pointer_set_focus(seat->pointer_state, sv, sx, sy);

    grab->start(seat);
}

void DesktopShell::unlock(struct wl_client *client, struct wl_resource *resource)
{
//     struct desktop_shell *shell = resource->data;
//
//     shell->prepare_event_sent = false;
//
//     if (shell->locked)
//         resume_desktop(shell);
}

void DesktopShell::setGrabSurface(struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource)
{
    this->Shell::setGrabSurface(static_cast<struct weston_surface *>(wl_resource_get_user_data(surface_resource)));
}

void DesktopShell::desktopReady(struct wl_client *client, struct wl_resource *resource)
{
    if (m_sessionManager) {
        m_sessionManager->restore();
    }
    m_splash->fadeOut();
}

void DesktopShell::addKeyBinding(struct wl_client *client, struct wl_resource *resource, uint32_t id, uint32_t key, uint32_t modifiers)
{
    wl_resource *res = wl_resource_create(client, &desktop_shell_binding_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(res, nullptr, res, [](wl_resource *) {});

    weston_compositor_add_key_binding(compositor(), key, (weston_keyboard_modifier)modifiers,
                                         [](struct weston_keyboard *keyboard_state, uint32_t time, uint32_t key, void *data) {

                                             desktop_shell_binding_send_triggered(static_cast<wl_resource *>(data));
                                         }, res);
}

void DesktopShell::addOverlay(struct wl_client *client, struct wl_resource *resource, struct wl_resource *output_resource, struct wl_resource *surface_resource)
{
    struct weston_surface *surface = static_cast<weston_surface *>(wl_resource_get_user_data(surface_resource));

    addOverlaySurface(surface, static_cast<weston_output *>(wl_resource_get_user_data(output_resource)));
    desktop_shell_send_configure(resource, 0, surface_resource, surface->output->width, surface->output->height);
    pixman_region32_fini(&surface->pending.input);
    pixman_region32_init_rect(&surface->pending.input, 0, 0, 0, 0);
}

void DesktopShell::addWorkspace(wl_client *client, wl_resource *resource)
{
    Workspace *ws = new Workspace(this, numWorkspaces());
    DesktopShellWorkspace *dws = new DesktopShellWorkspace;
    ws->addInterface(dws);
    dws->init(client);
    Shell::addWorkspace(ws);
    workspaceAdded(dws);
}

void DesktopShell::selectWorkspace(wl_client *client, wl_resource *resource, wl_resource *workspace_resource)
{
    Shell::selectWorkspace(DesktopShellWorkspace::fromResource(workspace_resource)->workspace()->number());
}

class ClientGrab : public ShellGrab {
public:
    void focus() override
    {
        wl_fixed_t sx, sy;
        weston_view *view = weston_compositor_pick_view(pointer()->seat->compositor, pointer()->x, pointer()->y, &sx, &sy);
        if (surfFocus != view) {
            surfFocus = view;
            desktop_shell_grab_send_focus(resource, view->surface->resource, sx, sy);
        }
    }
    void motion(uint32_t time, weston_pointer_motion_event *event) override
    {
        weston_pointer_move(pointer(), event);

        wl_fixed_t sx = pointer()->x;
        wl_fixed_t sy = pointer()->y;
        if (surfFocus) {
            weston_view_from_global_fixed(surfFocus, pointer()->x, pointer()->y, &sx, &sy);
        }

        desktop_shell_grab_send_motion(resource, time, sx, sy);
    }
    void button(uint32_t time, uint32_t button, uint32_t state) override
    {
        // Send the event to the application as normal if the mouse was pressed initially.
        // The application has to know the button was released, otherwise its internal state
        // will be inconsistent with the physical button state.
        // Eat the other events, as the app doesn't need to know them.
        // NOTE: this works only if there is only 1 button pressed initially. i can know how many button
        // are pressed but weston currently has no API to determine which ones they are.
        wl_resource *resource;
        wl_resource_for_each(resource, &pointer()->focus_client->pointer_resources) {
            if (pressed && button == pointer()->grab_button) {
                wl_display *display = wl_client_get_display(wl_resource_get_client(resource));
                uint32_t serial = wl_display_next_serial(display);
                wl_pointer_send_button(resource, serial, time, button, state);
                pressed = false;
            }
        }

        desktop_shell_grab_send_button(this->resource, time, button, state);
    }

    wl_resource *resource;
    weston_view *surfFocus;
    bool pressed;
};

static void clientGrabDestroyed(wl_resource *res)
{
    delete static_cast<ClientGrab *>(wl_resource_get_user_data(res));
}

void client_grab_end(wl_client *client, wl_resource *resource)
{
    ClientGrab *cg = static_cast<ClientGrab *>(wl_resource_get_user_data(resource));
    weston_output_schedule_repaint(cg->pointer()->focus->output);
    cg->end();
}

static const struct desktop_shell_grab_interface desktop_shell_grab_implementation = {
    client_grab_end
};

void DesktopShell::createGrab(wl_client *client, wl_resource *resource, uint32_t id)
{
    wl_resource *res = wl_resource_create(client, &desktop_shell_grab_interface, wl_resource_get_version(resource), id);

    ClientGrab *grab = new ClientGrab;
    wl_resource_set_implementation(res, &desktop_shell_grab_implementation, grab, clientGrabDestroyed);

    if (!grab)
        return;

    weston_seat *seat = container_of(compositor()->seat_list.next, weston_seat, link);
    grab->resource = res;
    grab->pressed = seat->pointer_state->button_count > 0;

    ShellSeat::shellSeat(seat)->endPopupGrab();

    wl_fixed_t sx, sy;
    weston_view *view = weston_compositor_pick_view(compositor(), seat->pointer_state->x, seat->pointer_state->y, &sx, &sy);
    grab->surfFocus = view;
    grab->start(seat);

    weston_pointer_set_focus(seat->pointer_state, view, sx, sy);
    desktop_shell_grab_send_focus(grab->resource, view->surface->resource, sx, sy);
}

void DesktopShell::quit(wl_client *client, wl_resource *resource)
{
    Shell::quit();
}

void DesktopShell::addTrustedClient(wl_client *client, wl_resource *resource, int32_t fd, const char *interface)
{
    wl_client *c = wl_client_create(compositor()->wl_display, fd);

    Client *cl = new Client;
    cl->client = c;
    cl->destroyListener.signal->connect(this, &DesktopShell::trustedClientDestroyed);
    wl_client_add_destroy_listener(c, cl->destroyListener.listener());

    m_trustedClients[interface].push_back(cl);
}

void DesktopShell::pong(uint32_t serial)
{
    if (!m_pingTimer.isRunning())
        /* Just ignore unsolicited pong. */
        return;

    if (m_pingSerial == serial) {
        m_pingTimer.stop();
    }
}

const struct desktop_shell_interface DesktopShell::m_desktop_shell_implementation = {
    wrapInterface(&DesktopShell::setBackground),
    wrapInterface(&DesktopShell::setPanel),
    wrapInterface(&DesktopShell::setLockSurface),
    wrapInterface(&DesktopShell::setPopup),
    wrapInterface(&DesktopShell::unlock),
    wrapInterface(&DesktopShell::setGrabSurface),
    wrapInterface(&DesktopShell::desktopReady),
    wrapInterface(&DesktopShell::addKeyBinding),
    wrapInterface(&DesktopShell::addOverlay),
    wrapInterface(&Shell::minimizeWindows),
    wrapInterface(&Shell::restoreWindows),
    wrapInterface(&DesktopShell::createGrab),
    wrapInterface(&DesktopShell::addWorkspace),
    wrapInterface(&DesktopShell::selectWorkspace),
    wrapInterface(&DesktopShell::quit),
    wrapInterface(&DesktopShell::addTrustedClient),
    wrapInterface(&DesktopShell::pong)
};

void DesktopShell::setSplashSurface(wl_client *client, wl_resource *resource, wl_resource *output_resource, wl_resource *surface_resource)
{
    weston_surface *surf = static_cast<weston_surface *>(wl_resource_get_user_data(surface_resource));
    weston_output *out = static_cast<weston_output *>(wl_resource_get_user_data(output_resource));

    int x = out->x, y = out->y;
    surf->output = out;

    surf->configure = [](weston_surface *es, int32_t x, int32_t y) {
        // FIXME: Remove these once they're not needed anymore!!
        if (es->output) {
            weston_output_schedule_repaint(es->output);
        }
    };

    weston_view *view = weston_view_create(surf);
    weston_view_set_position(view, x, y);
    setSplash(view);
    view->output = out;
    m_splash->addOutput(view, resource);
}

const struct desktop_shell_splash_interface DesktopShell::s_desktop_shell_splash_implementation = {
    wrapInterface(&DesktopShell::setSplashSurface)
};


WL_EXPORT int
module_init(struct weston_compositor *ec, int *argc, char *argv[])
{

    char *client = nullptr;
    char *sfile = nullptr;

    for (int i = *argc - 1; i >= 0; --i) {
        if (char *s = strstr(argv[i], "--nuclear-client=")) {
            client = strdup(s + 17);
            --*argc;
        } else if (char *s = strstr(argv[i], "--session-file=")) {
            sfile = strdup(s + 15);
            --*argc;
        }
    }

    DesktopShell *shell = Shell::load<DesktopShell>(ec, client);
    if (!shell) {
        return -1;
    }
    if (sfile) {
        shell->m_sessionManager = new SessionManager(sfile);
    }
    shell->init();

    return 0;
}


class DesktopShellSettings : public Settings
{
public:
    DesktopShellSettings()
    {

    }

    ~DesktopShellSettings()
    {

    }

    inline DesktopShell *shell() const
    {
        return static_cast<DesktopShell *>(Shell::instance());
    }

    virtual std::list<Option> options() const override
    {
        std::list<Option> list;
        list.push_back(Option::binding("move_window", Binding::Type::Button));
        list.push_back(Option::binding("resize_window", Binding::Type::Button));
        list.push_back(Option::binding("close_window", Binding::Type::Key));
        list.push_back(Option::binding("previous_workspace", Binding::Type::Key));
        list.push_back(Option::binding("next_workspace", Binding::Type::Key));
        list.push_back(Option::binding("quit", Binding::Type::Key));
        return list;
    }

    virtual void unSet(const std::string &name) override
    {
        if (name == "move_window") {
            shell()->m_moveBinding->reset();
        } else if (name == "resize_window") {
            shell()->m_resizeBinding->reset();
        } else if (name == "close_window") {
            shell()->m_closeBinding->reset();
        } else if (name == "previous_workspace") {
            shell()->m_prevWsBinding->reset();
        } else if (name == "next_workspace") {
            shell()->m_nextWsBinding->reset();
        } else if (name == "quit") {
            shell()->m_quitBinding->reset();
        }
    }

    virtual void set(const std::string &name, const Option::BindingValue &v) override
    {
        if (name == "move_window") {
            v.bind(shell()->m_moveBinding);
        } else if (name == "resize_window") {
            v.bind(shell()->m_resizeBinding);
        } else if (name == "close_window") {
            v.bind(shell()->m_closeBinding);
        } else if (name == "previous_workspace") {
            v.bind(shell()->m_prevWsBinding);
        } else if (name == "next_workspace") {
            v.bind(shell()->m_nextWsBinding);
        } else if (name == "quit") {
            v.bind(shell()->m_quitBinding);
        }
    }

private:
};

SETTINGS(desktop_shell, DesktopShellSettings)
