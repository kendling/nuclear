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

#ifndef LAYER_H
#define LAYER_H

#include <weston/compositor.h>

#include "utils.h"

class ShellSurface;

struct weston_view;

class Layer {
public:
    template<class L, class S>
    class Iterator {
    public:
        Iterator(const Iterator &it) { *this = it; }

        Iterator &operator=(const Iterator &it);

        bool operator==(const Iterator &it) const { return m_elm == it.m_elm; }
        bool operator!=(const Iterator &it) const { return m_elm != it.m_elm; }

        S *operator*() const { return deref(); }
        S *operator->() const { return deref(); }

        Iterator &operator++();

    private:
        Iterator(const struct wl_list *list, const L *elm, bool reverse);
        S *deref() const;

        const struct wl_list *m_list;
        const L *m_elm;
        // this m_next is needed to do what wl_list_for_each_safe does, that is
        // it allows for the current element to be removed from the list
        // without having the iterator go berserk.
        const L *m_next;
        bool m_reverse;

        friend class Layer;
    };

    typedef Iterator<struct wl_list, weston_view> iterator;
    typedef Iterator<const struct wl_list, const weston_view> const_iterator;

    Layer();

    void insert(struct weston_layer *below);
    void insert(Layer *below);
    void remove();
    void hide();
    void show();
    bool isVisible() const;

    void addSurface(weston_view *surf);
    void addSurface(ShellSurface *surf);
    void restack(weston_view *surf);
    void restack(ShellSurface *surf);

    bool isEmpty() const;
    int numberOfSurfaces() const;

    void stackAbove(weston_view *surf, weston_view *parent);
    void stackBelow(weston_view *surf, weston_view *parent);

    iterator begin() const;
    iterator rbegin() const;
    iterator end() const;

private:
    struct weston_layer m_layer;
    struct wl_list *m_below;
};

template<class L, class S>
Layer::Iterator<L, S>::Iterator(const struct wl_list *list, const L *elm, bool reverse)
                     : m_list(list)
                     , m_elm(elm)
                     , m_reverse(reverse)
{
    m_next = m_reverse ? m_elm->prev : m_elm->next;
}

template<class L, class S>
Layer::Iterator<L, S> &Layer::Iterator<L, S>::operator=(const Iterator &it)
{
    m_list = it.m_list;
    m_elm = it.m_elm;
    m_next = it.m_next;
    m_reverse = it.m_reverse;
    return *this;
}

template<class L, class S>
Layer::Iterator<L, S> &Layer::Iterator<L, S>::operator++()
{
    if (m_list != m_elm) {
        m_elm = m_next;
        m_next = m_reverse ? m_elm->prev : m_elm->next;
    }
    return *this;
}

template<class L, class S>
S *Layer::Iterator<L, S>::deref() const
{
    if (m_elm) {
        return container_of(m_elm, weston_view, layer_link.link);
    } else {
        return nullptr;
    }
}

#endif
