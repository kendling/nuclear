/*
 * Copyright 2013-2014 Giulio Camuffo <giuliocamuffo@gmail.com>
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

#ifndef UTILS_H
#define UTILS_H

#include <weston/compositor.h>

#include "shellsignal.h"

#define container_of(ptr, type, member) ({				\
	const __typeof__( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

template<typename T>
class Vector2D {
public:
    inline Vector2D(T a, T b) : x(a), y(b) {}
    T x;
    T y;
};

template<typename T>
class Rect2D {
public:
    inline Rect2D(T a, T b, T w, T h) : x(a), y(b), width(w), height(h) {}
    T x;
    T y;
    T width;
    T height;

    bool contains(T a, T b) const { return a >= x && a <= x + width && b >= y && b <= y + height; }

    bool operator==(const Rect2D &r) { return x == r.x && y == r.y && width == r.width && height == r.height; }
    bool operator!=(const Rect2D &r) { return !(*this == r); }
};

typedef Vector2D<int> IVector2D;
typedef Rect2D<int> IRect2D;

class WlListener {
public:
    WlListener() {
        signal = new Signal<void *>;
        m_listener.parent = this;
        m_listener.listener.notify = notify;
    }
    ~WlListener() { signal->flush(); wl_list_remove(&m_listener.listener.link); }

    wl_listener *listener() { return &m_listener.listener; }
    void listen(struct wl_signal *signal) {
        wl_signal_add(signal, &m_listener.listener);
    }
    void reset() {
        wl_list_remove(&m_listener.listener.link);
        wl_list_init(&m_listener.listener.link);
    }

    Signal<void *> *signal;
private:
    static void notify(wl_listener *listener, void *data) {
        WlListener *_this = static_cast<Wrapper *>(container_of(listener, Wrapper, listener))->parent;
        (*_this->signal)(data);
    }

    struct Wrapper {
        struct wl_listener listener;
        WlListener *parent;
    };
    Wrapper m_listener;
};

template<class R, class T, class... Args>
struct Wrapper {
    template<R (T::*F)(wl_client *, wl_resource *, Args...)>
    static void forward(wl_client *client, wl_resource *resource, Args... args) {
        (static_cast<T *>(wl_resource_get_user_data(resource))->*F)(client,resource, args...);
    }
    template<R (T::*F)(Args...)>
    static void forward(wl_client *client, wl_resource *resource, Args... args) {
        (static_cast<T *>(wl_resource_get_user_data(resource))->*F)(args...);
    }
};

template<class R, class T, class... Args>
constexpr static auto createWrapper(R (T::*func)(wl_client *client, wl_resource *resource, Args...)) -> Wrapper<R, T, Args...> {
    return Wrapper<R, T, Args...>();
}

template<class R, class T, class... Args>
constexpr static auto createWrapper(R (T::*func)(Args...)) -> Wrapper<R, T, Args...> {
    return Wrapper<R, T, Args...>();
}

class Timer {
public:
    Timer(int interval);
    ~Timer();

    void start();
    void stop();
    bool isRunning() const;

    Signal<> triggered;

private:
    int m_interval;
    wl_event_source *m_source;
};

#define wrapInterface(method) createWrapper(method).forward<method>

#endif
