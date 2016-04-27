/*
 * Copyright 2013  Giulio Camuffo <giuliocamuffo@gmail.com>
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

#include "xwlshell.h"
#include "shell.h"
#include "shellsurface.h"

XWlShell::XWlShell()
{
    weston_compositor *compositor = Shell::compositor();

    compositor->shell_interface.shell = this;
    compositor->shell_interface.create_shell_surface = [](void *shell, weston_surface *surface, const weston_shell_client *client) {
        return (shell_surface *)static_cast<XWlShell *>(shell)->getShellSurface(surface, client);
    };
#define _this reinterpret_cast<ShellSurface *>(shsurf)
    compositor->shell_interface.set_toplevel = [](shell_surface *shsurf) { _this->setTopLevel(); };
    compositor->shell_interface.set_transient = [](shell_surface *shsurf, weston_surface *parent, int x, int y, uint32_t flags) { _this->setTransient(parent, x, y, flags & WL_SHELL_SURFACE_TRANSIENT_INACTIVE); };
    compositor->shell_interface.set_fullscreen = [](shell_surface *shsurf, uint32_t method, uint32_t framerate, weston_output *output) { _this->setFullscreen((ShellSurface::FullscreenMethod)method, framerate, output);};
    compositor->shell_interface.resize = [](shell_surface *shsurf, weston_pointer *pointer_state, uint32_t edges) { _this->dragResize(pointer_state->seat, (ShellSurface::Edges)edges); return 0; };
    compositor->shell_interface.move = [](shell_surface *shsurf, weston_pointer *pointer_state) { _this->dragMove(pointer_state->seat); return 0; };
    compositor->shell_interface.set_xwayland = [](shell_surface *shsurf, int x, int y, uint32_t flags) { _this->setXWayland(x, y, flags & WL_SHELL_SURFACE_TRANSIENT_INACTIVE); };
    compositor->shell_interface.set_title = [](shell_surface *shsurf, const char *t) { _this->setTitle(t); };
    compositor->shell_interface.set_window_geometry = [](shell_surface *shsurf, int32_t x, int32_t y, int32_t w, int32_t h) { _this->setGeometry(x, y, w, h); };
#undef _this
}

ShellSurface *XWlShell::getShellSurface(weston_surface *surface, const weston_shell_client *client)
{
    ShellSurface *shsurf = Shell::instance()->createShellSurface(surface, client);

    XWlShellSurface *iface = new XWlShellSurface;
    shsurf->addInterface(iface);
    surface->configure_private = iface;

    return shsurf;
}

XWlShellSurface::XWlShellSurface()
{
}
