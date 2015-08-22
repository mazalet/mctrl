/*
 * Copyright (c) 2008-2015 Martin Mitas
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "mditab.h"
#include "generic.h"
#include "anim.h"
#include "dsa.h"
#include "theme.h"
#include "xdraw.h"

#include <math.h>


/* Uncomment this to have more verbose traces from this module. */
/*#define MDITAB_DEBUG     1*/
#define MDITAB_DEBUG     1

#ifdef MDITAB_DEBUG
    #define MDITAB_TRACE       MC_TRACE
#else
    #define MDITAB_TRACE(...)  do {} while(0)
#endif


/* Few implementation notes:
 * =========================
 *
 * The control geometry is quite complex, therefore we distinguish several
 * different widths of items:
 *
 * (1) Current width: This is the width "right now". I.e. painting and hit
 *     testing uses this item width.
 *
 *     current_width = (mditab_item_t::x1 - mditab_item_t::x0)
 *
 * (2) Ideal width: Width which guarantees that whole item label (and icon)
 *     can be painted within the item. The ideal width is cached in
 *     mditab_item_t::ideal_width (but it is only calculated lazily).
 *
 * (3) Target width: On a control change, width of an item may need to change.
 *     This determines the desired width. Unless animation is in progress,
 *     this is same as (1). It depends on many control styles, control size
 *     and other attributes.
 *
 *     Normally, it is equal to mditab_t::item_def_width (if it is non-zero)
 *     or ideal width (if the item_def_width is zero). But if the control is
 *     too small to accommodate all items (i.e. if scrolling arrows are visible
 *     and enabled) the target width may be shorter (mditab_t::item_min_width).
 *     (The shortening is disabled if item_min_width is zero.)
 *
 * Furthermore, the shape of items is irregular (it has those curved sides),
 * so all the width measure "an average width", which is defined to ignore
 * parts of items which may overlap with other items. Painting (invalidating)
 * and hit-testing code has to deal with this specially but otherwise it
 * simplifies a lot of our math.
 *
 * The curved parts are parts of circle, their radius equal to half of the
 * item height (which is equal to client rect. height - MDITAB_ITEM_TOP_MARGIN).
 */



static const TCHAR mditab_wc[] = MC_WC_MDITAB;  /* window class name */


/* Geometry constants */
#define DEFAULT_ITEM_MIN_WIDTH       60
#define DEFAULT_ITEM_DEF_WIDTH        0

#define BTNID_LSCROLL                 0
#define BTNID_RSCROLL                 1
#define BTNID_LIST                    2
#define BTNID_CLOSE                   3

#define BTNMASK_LSCROLL              (1 << BTNID_LSCROLL)
#define BTNMASK_RSCROLL              (1 << BTNID_RSCROLL)
#define BTNMASK_LIST                 (1 << BTNID_LIST)
#define BTNMASK_CLOSE                (1 << BTNID_CLOSE)

#define BTNMASK_SCROLL               (BTNMASK_LSCROLL | BTNMASK_RSCROLL)

#define ITEM_HOT_NONE              -100

#define ANIM_TIMER_ID                 1
#define ANIM_MAX_PIXELS_PER_SECOND  800

#define MDITAB_ITEM_TOP_MARGIN        4    /* space above item */
#define MDITAB_ITEM_PADDING           8    /* horizontal padding inside the item */
#define MDITAB_ITEM_ICON_MARGIN       5    /* space between icon and text */

#define MDITAB_COLOR_BACKGROUND    XDRAW_COLORREF(GetSysColor(COLOR_APPWORKSPACE))
#define MDITAB_COLOR_BORDER        XDRAW_COLORREF(GetSysColor(COLOR_3DDKSHADOW))
#define MDITAB_COLOR_INACTIVEITEM  XDRAW_ACOLORREF(127,GetSysColor(COLOR_APPWORKSPACE))
#define MDITAB_COLOR_HOTITEM       XDRAW_ACOLORREF(63,GetSysColor(COLOR_APPWORKSPACE))


typedef struct mditab_paint_tag mditab_paint_t;
struct mditab_paint_tag {
    xdraw_canvas_t* canvas;
    xdraw_brush_t* solid_brush;
    xdraw_font_t* font;
};

/* Per-tab structure */
typedef struct mditab_item_tag mditab_item_t;
struct mditab_item_tag {
    TCHAR* text;
    LPARAM lp;
    SHORT img;
    USHORT x0;           /* Relative to mditab_t::area_margin0. */
    USHORT x1;
    USHORT ideal_width;  /* Cached result of mditab_item_ideal_width() */
};

/* Per-control structure */
typedef struct mditab_tag mditab_t;
struct mditab_tag {
    HWND win;
    HWND notify_win;
    HIMAGELIST img_list;
    HFONT font;
    mditab_paint_t* paint_ctx;
    anim_t* animation;
    dsa_t items;
    DWORD style             : 16;
    DWORD btn_mask          :  4;
    DWORD no_redraw         :  1;
    DWORD tracking_leave    :  1;
    DWORD dirty_layout      :  1;
    DWORD dirty_scroll      :  1;
    DWORD btn_pressed       :  1;  /* Button ABS(item_hot) is pressed. */
    DWORD scrolling_to_item :  1;  /* If set, scroll_x_desired is item index. */
    int scroll_x;
    int scroll_x_desired;
    int scroll_x_max;
    USHORT area_margin0; /* Left and right margins of area where tabs live. */
    USHORT area_margin1;
    SHORT item_selected;
    SHORT item_hot;      /* If negative, ABS(item_hot+1) is BTNID_xxx of hot/pressed button. */
    SHORT item_mclose;   /* Close-by-middle-button candidate. */
    USHORT item_min_width;
    USHORT item_def_width;
};

typedef struct mditab_item_layout_tag mditab_item_layout_t;
struct mditab_item_layout_tag {
    xdraw_rect_t icon_rect;
    xdraw_rect_t text_rect;
};


static inline int
mditab_hot_button(mditab_t* mditab)
{
    if(mditab->item_hot < 0)
        return -mditab->item_hot - 1;
    else
        return -1;
}

static inline mditab_item_t*
mditab_item(mditab_t* mditab, WORD index)
{
    return DSA_ITEM(&mditab->items, index, mditab_item_t);
}

static inline WORD
mditab_count(mditab_t* mditab)
{
    return dsa_size(&mditab->items);
}

static void
mditab_item_dtor(dsa_t* dsa, void* it)
{
    mditab_item_t* item = (mditab_item_t*) it;
    if(item->text != NULL)
        free(item->text);
}

static inline USHORT
mditab_item_current_width(mditab_t* mditab, WORD index)
{
    mditab_item_t* item = mditab_item(mditab, index);
    return (item->x1 - item->x0);
}

static USHORT
mditab_item_ideal_width(mditab_t* mditab, WORD index)
{
    mditab_item_t* item = mditab_item(mditab, index);

    if(MC_UNLIKELY(item->ideal_width == 0)) {
        const TCHAR* text = item->text;
        int w = 0;

        if(mditab->img_list != NULL) {
            int ico_w, ico_h;
            ImageList_GetIconSize(mditab->img_list, &ico_w, &ico_h);
            w += ico_w + MDITAB_ITEM_PADDING;

            if(text != NULL)
                w += MDITAB_ITEM_ICON_MARGIN;
            else
                w += MDITAB_ITEM_PADDING;
        }

        if(text != NULL) {
            xdraw_canvas_t* canvas;
            xdraw_font_t* font;

            if(mditab->paint_ctx != NULL) {
                canvas = mditab->paint_ctx->canvas;
                font = mditab->paint_ctx->font;
            } else {
                RECT client;
                HDC dc;

                GetClientRect(mditab->win, &client);
                dc = GetDCEx(NULL, NULL, DCX_CACHE);
                canvas = xdraw_canvas_create_with_dc(dc, &client, 0);
                if(MC_ERR(canvas == NULL)) {
                    MC_TRACE("mditab_item_ideal_width: "
                             "xdraw_canvas_create_with_dc() failed.");
                    goto err_xdraw_canvas_create_with_dc;
                }
                font = xdraw_font_create_with_HFONT(canvas, mditab->font);
                if(MC_ERR(font == NULL)) {
                    MC_TRACE("mditab_item_ideal_width: "
                             "draw_font_create_with_HFONT() failed.");
                    goto err_xdraw_font_create_with_HFONT;
                }
            }

            w += xdraw_measure_string_width(canvas, font, text);
            w += MDITAB_ITEM_PADDING;

            /* FIXME: Not sure why this is needed so that the text fits in.
             *        Would be better to fix in xdraw.c */
            w += 1.0f;

            if(mditab->paint_ctx == NULL) {
                xdraw_font_destroy(font);
err_xdraw_font_create_with_HFONT:
                xdraw_canvas_destroy(canvas);
err_xdraw_canvas_create_with_dc:
                ;  /* noop */
            }
        }

        item->ideal_width = w;
    }

    return MC_MAX(item->ideal_width, mditab->item_min_width);
}

static void
mditab_reset_ideal_widths(mditab_t* mditab)
{
    int i;
    int n = mditab_count(mditab);

    for(i = 0; i < n; i++) {
        mditab_item_t* item = mditab_item(mditab, i);
        item->ideal_width = 0;
    }
}

static inline int
mditab_button_size(const RECT* client)
{
    return mc_height(client) - 4;
}

static void
mditab_button_rect(mditab_t* mditab, int btn_id, RECT* rect)
{
    RECT client;
    int x0, y0;
    int btn_size;

    MC_ASSERT(mditab->btn_mask & (1 << btn_id));

    GetClientRect(mditab->win, &client);
    btn_size = mditab_button_size(&client);
    y0 = (client.bottom - btn_size + 1) / 2;

    if(btn_id == BTNID_LSCROLL) {
        x0 = 0;
        mc_rect_set(rect, x0, y0, x0 + btn_size, y0 + btn_size);
        return;
    }

    x0 = client.right - btn_size;
    if(btn_id == BTNID_CLOSE) {
        mc_rect_set(rect, x0, y0, x0 + btn_size, y0 + btn_size);
        return;
    }

    if(mditab->btn_mask & (1 << BTNID_CLOSE))
        x0 -= btn_size;

    if(btn_id == BTNID_LIST) {
        mc_rect_set(rect, x0, y0, x0 + btn_size, y0 + btn_size);
        return;
    }

    if(mditab->btn_mask & (1 << BTNID_LIST))
        x0 -= btn_size;

    MC_ASSERT(btn_id == BTNID_RSCROLL);
    mc_rect_set(rect, x0, y0, x0 + btn_size, y0 + btn_size);
}

static void
mditab_setup_item_layout(mditab_t* mditab,mditab_item_t* item, int x0, int y0,
                         int x1, int y1, mditab_item_layout_t* layout)
{
    int contents_x = x0 + MDITAB_ITEM_PADDING;

    if(mditab->img_list != NULL) {
        int icon_w, icon_h;

        ImageList_GetIconSize(mditab->img_list, &icon_w, &icon_h);
        layout->icon_rect.x0 = contents_x;
        layout->icon_rect.y0 = (y0 + y1 - icon_h) / 2;
        layout->icon_rect.x1 = layout->icon_rect.x0 + icon_w;
        layout->icon_rect.y1 = layout->icon_rect.y0 + icon_h;

        contents_x += icon_w + MDITAB_ITEM_ICON_MARGIN;
    }

    if(item->text != NULL) {
        SIZE size;

        mc_font_size(mditab->font, &size, TRUE);
        layout->text_rect.x0 = contents_x;
        layout->text_rect.y0 = (y0 + y1 - size.cy) / 2;
        layout->text_rect.x1 = x1 - MDITAB_ITEM_PADDING;
        layout->text_rect.y1 = layout->text_rect.y0 + size.cy;
    }
}

static BOOL
mditab_hit_test_item(mditab_t* mditab, MC_MTHITTESTINFO* hti,
                     const RECT* client, WORD index, BOOL want_hti_flags)
{
    int x = hti->pt.x;
    int y = hti->pt.y;
    mditab_item_t* item;
    int x0, y0, x1, y1;
    int r;

    item = mditab_item(mditab, index);

    x0 = mditab->area_margin0 + item->x0 - mditab->scroll_x;
    y0 = MDITAB_ITEM_TOP_MARGIN;
    x1 = mditab->area_margin0 + item->x1 - mditab->scroll_x;
    y1 = client->bottom;

    r = (y1 - y0) / 2;

    if(y < y0  ||  y >= y1)
        return FALSE;
    if(x < x0 - r  ||  x >= x1 + r)
        return FALSE;

    if(x < x0 + r  ||  x > x1 - r) {
        if(x < x0) {
            int cx = x0 - r;
            int cy = y0 + r;
            int xdiff = x - cx;
            int ydiff = y - cy;
            if(!(y > cy  &&  xdiff * xdiff + ydiff * ydiff >= r * r))
                return FALSE;
        } else if(x < x0 + r) {
            int cx = x0 + r;
            int cy = y0 + r;
            int xdiff = x - cx;
            int ydiff = y - cy;
            if(!(y > cy  ||  xdiff * xdiff + ydiff * ydiff <= r * r))
                return FALSE;
        } else if(x < x1) {
            int cx = x1 - r;
            int cy = y0 + r;
            int xdiff = x - cx;
            int ydiff = y - cy;
            if(!(y > cy  ||  xdiff * xdiff + ydiff * ydiff <= r * r))
                return FALSE;
        } else {
            int cx = x1 + r;
            int cy = y0 + r;
            int xdiff = x - cx;
            int ydiff = y - cy;
            if(!(y > cy  &&  xdiff * xdiff + ydiff * ydiff >= r * r))
                return FALSE;
        }
    }

    if(want_hti_flags) {
        mditab_item_layout_t layout;

        mditab_setup_item_layout(mditab, item, x0, y0, x1, y1, &layout);

        if(mditab->img_list != NULL  &&
           layout.icon_rect.x0 <= x  &&  x < layout.icon_rect.x1  &&
           layout.icon_rect.y0 <= y  &&  y < layout.icon_rect.y1)
            hti->flags = MC_MTHT_ONITEMICON;
        else
            hti->flags = MC_MTHT_ONITEMLABEL;
    }

    return TRUE;
}

static int
mditab_hit_test(mditab_t* mditab, MC_MTHITTESTINFO* hti, BOOL want_hti_flags)
{
    static const UINT btn_map[] = {
        MC_MTHT_ONLEFTSCROLLBUTTON, MC_MTHT_ONRIGHTSCROLLBUTTON,
        MC_MTHT_ONLISTBUTTON, MC_MTHT_ONCLOSEBUTTON
    };

    RECT client;
    int area_x0;
    int area_x1;
    int r;
    int btn_id;
    int i;

    GetClientRect(mditab->win, &client);

    /* Handle if outside client. */
    if(!mc_rect_contains_pt(&client, &hti->pt)) {
        hti->flags = 0;
        if(hti->pt.x < client.left)
            hti->flags |= MC_MTHT_TOLEFT;
        else if(hti->pt.x >= client.right)
            hti->flags |= MC_MTHT_TORIGHT;
        if(hti->pt.y < client.top)
            hti->flags |= MC_MTHT_ABOVE;
        else if(hti->pt.y >= client.bottom)
            hti->flags |= MC_MTHT_BELOW;
        return -1;
    }

    /* Hit test items. */
    /* FIXME: This can be optimized, we can guess the right item given the X
     *        coordinate and start the right item from there. */
    area_x0 = mditab->area_margin0;
    area_x1 = client.right - mditab->area_margin1;
    r = (mc_height(&client) - MDITAB_ITEM_TOP_MARGIN + 1) / 2;
    if(mditab->scroll_x <= 0)
        area_x0 -= r;
    if(mditab->scroll_x >= mditab->scroll_x_max)
        area_x1 += r;
    if(area_x0 <= hti->pt.x  &&  hti->pt.x < area_x1) {
        if(mditab->item_selected >= 0) {
            i = mditab->item_selected;
            if(mditab_hit_test_item(mditab, hti, &client, i, want_hti_flags))
                return i;
        }
        for(i = mditab_count(mditab) - 1; i >= 0; i--) {
            if(i == mditab->item_selected)
                continue;
            if(mditab_hit_test_item(mditab, hti, &client, i, want_hti_flags))
                return i;
        }
    }

    /* Hit test auxiliary buttons. */
    for(btn_id = 0; btn_id < MC_ARRAY_SIZE(btn_map); btn_id++) {
        if(mditab->btn_mask & (1 << btn_id)) {
            RECT btn_rect;

            mditab_button_rect(mditab, btn_id, &btn_rect);
            if(mc_rect_contains_pt(&btn_rect, &hti->pt)) {
                hti->flags = btn_map[btn_id];
                return -1;
            }
        }
    }

    hti->flags = MC_MTHT_NOWHERE;
    return -1;
}

static void
mditab_invalidate_item(mditab_t* mditab, WORD index)
{
    mditab_item_t* item;
    RECT rect;
    int r;

    if(mditab->no_redraw)
        return;

    item = mditab_item(mditab, index);

    GetClientRect(mditab->win, &rect);
    r = mc_height(&rect) - MDITAB_ITEM_TOP_MARGIN;

    rect.left = mditab->area_margin0 + item->x0 - mditab->scroll_x - r;
    rect.right = mditab->area_margin0 + item->x1 - mditab->scroll_x + r;

    InvalidateRect(mditab->win, &rect, TRUE);
}

static void
mditab_invalidate_button(mditab_t* mditab, int btn_id)
{
    RECT rect;

    if(mditab->no_redraw  ||  !(mditab->btn_mask & (1 << btn_id)))
        return;

    mditab_button_rect(mditab, btn_id, &rect);
    InvalidateRect(mditab->win, &rect, TRUE);
}

static void
mditab_set_hot(mditab_t* mditab, SHORT hot, BOOL is_pressed)
{
    if(hot == mditab->item_hot  &&  is_pressed == mditab->btn_pressed)
        return;

    if(mditab->item_hot != ITEM_HOT_NONE) {
        if(mditab->item_hot >= 0)
            mditab_invalidate_item(mditab, mditab->item_hot);
        else
            mditab_invalidate_button(mditab, mditab_hot_button(mditab));
    }

    mditab->item_hot = hot;
    mditab->btn_pressed = is_pressed;

    if(mditab->item_hot != ITEM_HOT_NONE) {
        if(mditab->item_hot >= 0)
            mditab_invalidate_item(mditab, mditab->item_hot);
        else
            mditab_invalidate_button(mditab, mditab_hot_button(mditab));
    }
}

static inline void mditab_set_hot_item(mditab_t* mditab, WORD hot_item)
        { mditab_set_hot(mditab, hot_item, FALSE); }
static inline void mditab_set_hot_button(mditab_t* mditab, int btn_id, BOOL is_pressed)
        { mditab_set_hot(mditab, -(SHORT)btn_id - 1, is_pressed); }
static inline void mditab_reset_hot(mditab_t* mditab)
        { mditab_set_hot(mditab, ITEM_HOT_NONE, FALSE); }

static void
mditab_mouse_move(mditab_t* mditab, int x, int y)
{
    int index;
    MC_MTHITTESTINFO hti;

    if(mditab->btn_pressed)
        return;

    hti.pt.x = x;
    hti.pt.y = y;
    index = mditab_hit_test(mditab, &hti, FALSE);

    if(index >= 0) {
        mditab_set_hot_item(mditab, index);
    } else {
        int btn_id = -1;

        switch(hti.flags & MC_MTHT_ONBUTTON) {
            case MC_MTHT_ONLEFTSCROLLBUTTON:    btn_id = BTNID_LSCROLL; break;
            case MC_MTHT_ONRIGHTSCROLLBUTTON:   btn_id = BTNID_RSCROLL; break;
            case MC_MTHT_ONLISTBUTTON:          btn_id = BTNID_LIST; break;
            case MC_MTHT_ONCLOSEBUTTON:         btn_id = BTNID_CLOSE; break;
        }

        if(btn_id >= 0)
            mditab_set_hot_button(mditab, btn_id, FALSE);
        else
            mditab_reset_hot(mditab);
    }

    /* Ask for WM_MOUSELEAVE. */
    if(mditab->item_hot != ITEM_HOT_NONE  &&  !mditab->tracking_leave) {
        mc_track_mouse(mditab->win, TME_LEAVE);
        mditab->tracking_leave = TRUE;
    }
}

static void
mditab_mouse_leave(mditab_t* mditab)
{
    if(mditab->btn_pressed)
        return;

    mditab_reset_hot(mditab);
    mditab->tracking_leave = FALSE;
}

/* Helper for mditab_update_layout() */
static void
mditab_update_item_widths(mditab_t* mditab, WORD index, WORD n, USHORT width)
{
    USHORT i;
    USHORT x;

    if(index > 0)
        x = mditab_item(mditab, index-1)->x1;
    else
        x = 0;

    for(i = index; i < n; i++) {
        mditab_item_t* item = mditab_item(mditab, i);
        USHORT w = (width == 0 ? mditab_item_ideal_width(mditab, i) : width);

        item->x0 = x;
        item->x1 = x + w;
        x += w;
    }
}

static inline int
mditab_animate(int value_current, int value_desired, int max_delta,
               BOOL* continue_animation)
{
    if(value_current < value_desired  &&  value_current + max_delta < value_desired) {
        *continue_animation = TRUE;
        return value_current + max_delta;
    } else if(value_current > value_desired  &&  value_current - max_delta > value_desired) {
        *continue_animation = TRUE;
        return value_current - max_delta;
    }

    return value_desired;
}

static void
mditab_update_layout(mditab_t* mditab, BOOL refresh)
{
    typedef struct curr_geom_tag { USHORT x0; USHORT x1; } curr_geom_t;
    curr_geom_t* curr_geom = NULL;
    BOOL animate = (mditab->style & MC_MTS_ANIMATE);
    USHORT def_width = mditab->item_def_width;
    USHORT min_width = mditab->item_min_width;
    int btn_size;
    RECT client;
    BOOL need_scroll = FALSE;
    DWORD btn_mask = 0;
    USHORT area_margin0, area_margin1;
    USHORT area_width;
    int scroll_x_desired;
    int scroll_x;
    int i, n;
    BOOL continue_animation = FALSE;
    int anim_max_distance = 0;

    if(mditab->no_redraw)
        refresh = FALSE;

    GetClientRect(mditab->win, &client);
    btn_size = mditab_button_size(&client);
    n = mditab_count(mditab);

    /* When animating, compute how many pixels we can move an item, or
     * scroll all items. (Those two movements are independent on each other,
     * but they share the same max. speed.) */
    if(animate) {
        DWORD frame_time;

        if(mditab->animation != NULL)
            frame_time = anim_frame_time(mditab->animation);
        else
            frame_time = 1000 / ANIM_DEFAULT_FREQUENCY;

        /* Lets move the stuff maximally at 1000 pixels per second. */
        anim_max_distance = (frame_time * ANIM_MAX_PIXELS_PER_SECOND) / 1000;

        /* For animating we need the current (old) geometry to compute new
         * animation frame. */
        curr_geom = _malloca(n * sizeof(curr_geom_t));
        if(curr_geom != NULL) {
            for(i = 0; i < n; i++) {
                mditab_item_t* item = mditab_item(mditab, i);
                curr_geom[i].x0 = item->x0;
                curr_geom[i].x1 = item->x1;
            }
        } else {
            MC_TRACE("mditab_update_layout: _malloca() failed. Using worse code path.");
            animate = FALSE;
        }
    }

    /* Determine what auxiliary buttons we need. */
    if((mditab->style & MC_MTS_CBMASK) == MC_MTS_CBONTOOLBAR)
        btn_mask |= BTNMASK_CLOSE;

    if(((mditab->style & MC_MTS_TLBMASK) == MC_MTS_TLBALWAYS)  ||
       ((mditab->style & MC_MTS_TLBMASK) == MC_MTS_TLBONSCROLL  &&  need_scroll))
        btn_mask |= BTNMASK_LIST;

again_with_scroll:
    if((mditab->style & MC_MTS_SCROLLALWAYS)  ||  need_scroll)
        btn_mask |= BTNMASK_SCROLL;

    /* Determine what item area we need */
    area_margin0 = mc_height(&client) / 2;
    if(btn_mask & BTNMASK_LSCROLL)
        area_margin0 += btn_size;

    area_margin1 = mc_height(&client) / 2;
    if(btn_mask & BTNMASK_RSCROLL)
        area_margin1 += btn_size;
    if(btn_mask & BTNMASK_LIST)
        area_margin1 += btn_size;
    if(btn_mask & BTNMASK_CLOSE)
        area_margin1 += btn_size;

    area_width = mc_width(&client) - area_margin0 - area_margin1;

    /* Check whether we need scrolling. */
    if(!need_scroll  &&  n > 0) {
        if(min_width != 0) {
            need_scroll = (n * min_width > area_width);
        } else if(mditab->item_def_width != 0) {
            need_scroll = (n * def_width > area_width);
        } else {
            int width_sum = 0;
            for(i = 0; i < n; i++) {
                width_sum += mditab_item_ideal_width(mditab, i);
                if(width_sum > area_width) {
                    need_scroll = TRUE;
                    break;
                }
            }
        }

        /* If need scrolling, ensure the button mask involves scroll buttons. */
        if(need_scroll  &&  (btn_mask & BTNMASK_SCROLL) != BTNMASK_SCROLL)
            goto again_with_scroll;
    }

again_without_animation:
    /* Compute TARGET geometry of all items:
     *
     *     min_width  def_width  need_scroll  Behavior
     * ========================================================================
     * #1  == 0       == 0       FALSE        All items use ideal widths.
     * #2  == 0       == 0       TRUE         All items use ideal widths.
     * #3  == 0       != 0       FALSE        All items use def_width.
     * #4  == 0       != 0       TRUE         All items use def_width.
     * #5  != 0       == 0       FALSE        Try ideal width but may need some shrinking.
     * #6  != 0       == 0       TRUE         All items use min_width.
     * #7  != 0       != 0       FALSE        Try def_width but may need some shrinking.
     * #8  != 0       != 0       TRUE         All items use min_width.
     */
    if(n > 0) {
        if(min_width == 0) {            /* Cases #1, #2, #3, #4 */
            mditab_update_item_widths(mditab, 0, n, def_width);
        } else if(need_scroll) {        /* Cases #6, #8 */
            mditab_update_item_widths(mditab, 0, n, min_width);
        } else if(def_width == 0) {     /* Case #5 */
            USHORT w_sum = 0;

            for(i = 0; i < n; i++)
                w_sum += mditab_item_ideal_width(mditab, i);

            if(w_sum <= area_width) {
                /* All items fit in comfortably. */
                mditab_update_item_widths(mditab, 0, n, 0);
            } else {
                /* We still may make all items visible by some shrinking. So
                 * we reserve the min_width for each item and all the remaining
                 * space in the area_w is allocated proportionally among all
                 * the items.
                 *
                 * I.e. we project points excess_x (corresponding to the item
                 * boundaries) from the range <0, excess_sum> into the range
                 * <0, excess_target> in the linear fashion (with rounding to
                 * the closest integer). */
                UINT excess_sum = w_sum - n * min_width;
                UINT excess_target = area_width - n * min_width;
                UINT excess_x = 0;
                UINT excess_x_projection;
                UINT x = 0;

                for(i = 0; i < n; i++) {
                    mditab_item_t* item = mditab_item(mditab, i);
                    excess_x += mditab_item_ideal_width(mditab, i) - min_width;
                    excess_x_projection = (excess_x * excess_target + excess_sum/2) / excess_sum;
                    item->x0 = x;
                    item->x1 = (i+1) * min_width + excess_x_projection;
                    x = item->x1;
                }
            }
        } else {                        /* Case #7 */
            if(n * def_width <= area_width) {
                /* All items fit in comfortably. */
                mditab_update_item_widths(mditab, 0, n, def_width);
            } else {
                /* We still may make all items visible by some shrinking. */
                USHORT w_base = area_width / n;
                USHORT w_extra = area_width % n;
                MC_ASSERT(w_base >= min_width);
                mditab_update_item_widths(mditab, 0, w_extra, w_base + 1);
                mditab_update_item_widths(mditab, w_extra, n, w_base);
            }
        }
    }

    /* Animate, i.e. compute geometry for a new animation frame. */
    if(animate) {
        MC_ASSERT(curr_geom != NULL);

        area_margin0 = mditab_animate(mditab->area_margin0, area_margin0,
                            anim_max_distance, &continue_animation);
        area_margin1 = mditab_animate(mditab->area_margin1, area_margin1,
                            anim_max_distance, &continue_animation);
        area_width = mc_width(&client) - area_margin0 - area_margin1;

        for(i = 0; i < n; i++) {
            mditab_item_t* item = mditab_item(mditab, i);

            item->x0 = mditab_animate(curr_geom[i].x0, item->x0,
                            anim_max_distance, &continue_animation);
            item->x1 = mditab_animate(curr_geom[i].x1, item->x1,
                            anim_max_distance, &continue_animation);
        }
    }

    /* Scrolling */
    scroll_x = mditab->scroll_x;
    if(n > 0  &&  mditab_item(mditab, n-1)->x1 > area_width)
        mditab->scroll_x_max = mditab_item(mditab, n-1)->x1 - area_width;
    else
        mditab->scroll_x_max = 0;

    if(mditab->scrolling_to_item) {
        mditab_item_t* item = mditab_item(mditab, mditab->scroll_x_desired);
        if(item->x0 < scroll_x)
            scroll_x_desired = item->x0;
        else if(item->x1 > scroll_x + area_width)
            scroll_x_desired = item->x1 - area_width;
        else
            scroll_x_desired = mditab->scroll_x;
    } else {
        if(mditab->scroll_x_desired > mditab->scroll_x_max)
            mditab->scroll_x_desired = mditab->scroll_x_max;
        scroll_x_desired = mditab->scroll_x_desired;
    }

    scroll_x = scroll_x_desired;
    if(animate) {
        scroll_x = mditab_animate(mditab->scroll_x, scroll_x,
                            anim_max_distance, &continue_animation);
    }
    if(scroll_x == scroll_x_desired) {
        mditab->scrolling_to_item = FALSE;
        mditab->scroll_x_desired = scroll_x;
    } else {
        continue_animation = TRUE;
    }

    /* Refresh */
    if(refresh)
        InvalidateRect(mditab->win, NULL, TRUE);

    /* Commit the new values. */
    mditab->btn_mask = btn_mask;
    mditab->area_margin0 = area_margin0;
    mditab->area_margin1 = area_margin1;
    mditab->scroll_x = scroll_x;

    /* Manage the animation */
    if(continue_animation) {
        if(mditab->animation == NULL) {
            MDITAB_TRACE("mditab_update_layout: Starting animation.");
            mditab->animation = anim_start(mditab->win, ANIM_TIMER_ID,
                    ANIM_UNLIMITED_DURATION, ANIM_DEFAULT_FREQUENCY);
            if(MC_ERR(mditab->animation == NULL)) {
                MC_TRACE("mditab_update_layout: anim_start() failed.");
                /* Retry with animation disabled. */
                animate = FALSE;
                goto again_without_animation;
            }
        }
    } else {
        if(mditab->animation != NULL) {
            MDITAB_TRACE("mditab_update_layout: Stopping animation.");
            anim_stop(mditab->animation);
            mditab->animation = NULL;
        }
    }

    if(curr_geom != NULL)
        _freea(curr_geom);
}

static void
mditab_paint_ctx_init(mditab_paint_t* ctx, xdraw_canvas_t* canvas, HFONT font)
{
    ctx->canvas = canvas;
    ctx->solid_brush = xdraw_brush_solid_create(canvas, 0);
    ctx->font = xdraw_font_create_with_HFONT(canvas, font);
}

static void
mditab_paint_ctx_fini(mditab_paint_t* ctx)
{
    if(ctx->font != NULL)
        xdraw_font_destroy(ctx->font);
    if(ctx->solid_brush != NULL)
        xdraw_brush_destroy(ctx->solid_brush);
    if(ctx->canvas != NULL)
        xdraw_canvas_destroy(ctx->canvas);
}

static void
mditab_free_cached_paint_ctx(mditab_t* mditab)
{
    if(mditab->paint_ctx != NULL) {
        mditab_paint_ctx_fini(mditab->paint_ctx);
        free(mditab->paint_ctx);
        mditab->paint_ctx = NULL;
    }
}

#define BTNSTATE_NORMAL     0
#define BTNSTATE_HOT        1
#define BTNSTATE_PRESSED    2
#define BTNSTATE_DISABLED   3

static void
mditab_do_paint_button(mditab_t* mditab, mditab_paint_t* ctx, int btn_id,
                       xdraw_rect_t* rect, int state)
{
    static const struct {
        const xdraw_line_t lines[2];
    } dies[] = {
        { { { 0.6f, 0.3f, 0.4f, 0.5f }, { 0.4f, 0.5f, 0.6f, 0.7f } } },
        { { { 0.4f, 0.3f, 0.6f, 0.5f }, { 0.6f, 0.5f, 0.4f, 0.7f } } },
        { { { 0.3f, 0.4f, 0.5f, 0.6f }, { 0.5f, 0.6f, 0.7f, 0.4f } } },
        { { { 0.3f, 0.3f, 0.7f, 0.7f }, { 0.3f, 0.7f, 0.7f, 0.3f } } }
    };

    const xdraw_line_t* die_lines = dies[btn_id].lines;
    float w = rect->x1 - rect->x0;
    float h = rect->y1 - rect->y0;
    xdraw_color_t c;
    xdraw_line_t line;
    int i;

    if(state == BTNSTATE_HOT  ||  state == BTNSTATE_PRESSED) {
        xdraw_circle_t circle = { rect->x0 + w/2.0f, rect->y0 + h/2.0f, MC_MIN(w, h) / 2.0f - 1.0f };

        c = (state == BTNSTATE_HOT ? XDRAW_ACOLORREF(191, GetSysColor(COLOR_BTNFACE))
                                   : XDRAW_ACOLORREF(127, GetSysColor(COLOR_BTNFACE)));
        xdraw_brush_solid_set_color(ctx->solid_brush, c);
        xdraw_fill_circle(ctx->canvas, ctx->solid_brush, &circle);

    }

    c = (state == BTNSTATE_DISABLED ? XDRAW_ACOLORREF(63, GetSysColor(COLOR_BTNTEXT))
                                    : XDRAW_COLORREF(GetSysColor(COLOR_BTNTEXT)));

    xdraw_brush_solid_set_color(ctx->solid_brush, c);
    for(i = 0; i < 2; i++) {
        float stroke_width = (mc_win_version >= MC_WIN_10 ? 1.0f : 2.0f);

        line.x0 = rect->x0 + w * die_lines[i].x0;
        line.y0 = rect->y0 + h * die_lines[i].y0;
        line.x1 = rect->x0 + w * die_lines[i].x1;
        line.y1 = rect->y0 + h * die_lines[i].y1;
        xdraw_draw_line(ctx->canvas, ctx->solid_brush, &line, stroke_width);
    }
}

static void
mditab_paint_button(mditab_t* mditab, mditab_paint_t* ctx, int btn_id, BOOL enabled)
{
    RECT rect;
    xdraw_rect_t r;
    int state;

    /* Determine button state */
    if(!enabled)
        state = BTNSTATE_DISABLED;
    else if(btn_id == mditab_hot_button(mditab))
        state = (mditab->btn_pressed ? BTNSTATE_PRESSED : BTNSTATE_HOT);
    else
        state = BTNSTATE_NORMAL;

    /* Calculate button rect */
    mditab_button_rect(mditab, btn_id, &rect);
    r.x0 = (float) rect.left;
    r.y0 = (float) rect.top;
    r.x1 = (float) rect.right - 1;
    r.y1 = (float) rect.bottom - 1;

    mditab_do_paint_button(mditab, ctx, btn_id, &r, state);
}

static void
mditab_paint_scroll_block(mditab_t* mditab, mditab_paint_t* ctx,
                          float x, float y0, float y1, int direction)
{
    xdraw_line_t line;
    BYTE a = 0xff;
    BYTE r = XDRAW_REDVALUE(MDITAB_COLOR_BORDER);
    BYTE g = XDRAW_GREENVALUE(MDITAB_COLOR_BORDER);
    BYTE b = XDRAW_BLUEVALUE(MDITAB_COLOR_BORDER);
    int i;
    int ydiff = 1;

    line.x0 = x;
    line.y0 = y0 - MDITAB_ITEM_TOP_MARGIN / 2;
    line.x1 = x;
    line.y1 = y1;

    for(i = 0; i < 8; i++) {
        xdraw_brush_solid_set_color(ctx->solid_brush, XDRAW_ARGB(a, r, g, b));
        xdraw_draw_line(ctx->canvas, ctx->solid_brush, &line, 1.0f);

        line.x0 += direction;
        line.y0 += ydiff;
        line.x1 += direction;
        a /= 2;
        ydiff *= 2;
    }
}

static void
mditab_paint_item(mditab_t* mditab, mditab_paint_t* ctx, const RECT* client,
                  mditab_item_t* item, const xdraw_rect_t* item_rect,
                  int area_x0, int area_x1,
                  const xdraw_image_t* background_image,
                  BOOL is_selected, BOOL is_hot)
{
    float x0 = item_rect->x0;
    float y0 = item_rect->y0;
    float x1 = item_rect->x1;
    float y1 = item_rect->y1;
    float r;
    xdraw_path_t* path;
    xdraw_path_sink_t sink;
    xdraw_point_t pt;
    BOOL left_block;
    BOOL right_block;
    xdraw_rect_t clip_rect;
    xdraw_rect_t blit_rect;
    xdraw_line_t line;
    mditab_item_layout_t layout;
    BOOL degenerate_shape = FALSE;

    mditab_setup_item_layout(mditab, item, x0, y0, x1, y1, &layout);

    /* Construct a path defining shape of the item. */
    path = xdraw_path_create(ctx->canvas);
    if(MC_ERR(path == NULL)) {
        MC_TRACE("mditab_paint_item: xdraw_path_create() failed.");
        return;
    }
    if(MC_ERR(xdraw_path_open_sink(&sink, path) != 0)) {
        MC_TRACE("mditab_paint_item: xdraw_path_open_sink() failed.");
        xdraw_path_destroy(path);
        return;
    }
    r = (y1 - y0 - 1.0f) / 2.0f;
    if(2.0f * r > x1 - x0) {
        /* The item is too small, so we degenerate only to the curved shape
         * with decreased radius. As this should happen only during animation
         * and for very short time, hit testing and other code ignores this
         * problem altogether. But here, it would make the animation visually
         * disruptive if ignoring it.
         */
        r = (x1 - x0) / 2.0f;
        degenerate_shape = TRUE;
    }
    pt.x = x0 - r - 5.0f;
    pt.y = y1;
    xdraw_path_begin_figure(&sink, &pt);
    pt.x = x0 - r;
    pt.y = y1 - 1.0f;
    xdraw_path_add_line(&sink, &pt);
    pt.x = x0 - r;
    pt.y = y1 - r;
    xdraw_path_add_arc(&sink, &pt, -90.0f);
    pt.x = x0 + r;
    pt.y = y1 - r;
    xdraw_path_add_arc(&sink, &pt, 90.0f);
    if(!degenerate_shape) {
        pt.x = x1 - r;
        pt.y = y0;
        xdraw_path_add_line(&sink, &pt);
    }
    pt.x = x1 - r;
    pt.y = y1 - r;
    xdraw_path_add_arc(&sink, &pt, 90.0f);
    pt.x = x1 + r;
    pt.y = y1 - r;
    xdraw_path_add_arc(&sink, &pt, -90.0f);
    pt.x = x1 + r + 5.0f;
    pt.y = y1;
    xdraw_path_add_line(&sink, &pt);
    xdraw_path_end_figure(&sink, TRUE);
    xdraw_path_close_sink(&sink);

    /* Determine if we need to paint scroll blocks. */
    left_block = (mditab->scroll_x > 0  &&  x0 < area_x0);
    right_block = (mditab->scroll_x < mditab->scroll_x_max  &&  x1 > area_x1);

    /* Clip to the item geometry. */
    clip_rect.x0 = (left_block ? area_x0 - r : 0);
    clip_rect.y0 = 0;
    clip_rect.x1 = (right_block ? area_x1 + r : area_x1 + 100);
    clip_rect.y1 = y1;
    xdraw_canvas_set_clip(ctx->canvas, &clip_rect, path);

    /* Paint background of the item. */
    blit_rect.x0 = x0 - r;
    blit_rect.y0 = y0;
    blit_rect.x1 = x1 + r;
    blit_rect.y1 = y1;
    xdraw_blit_image(ctx->canvas, background_image, &blit_rect, &blit_rect);

    /* Colorize non-selected items. */
    if(!is_selected) {
        xdraw_brush_solid_set_color(ctx->solid_brush,
                (is_hot ? MDITAB_COLOR_HOTITEM : MDITAB_COLOR_INACTIVEITEM));
        xdraw_fill_path(ctx->canvas, ctx->solid_brush, path);
    }

    /* Paint item icon. */
    if(mditab->img_list != NULL) {
        HICON icon;

        icon = ImageList_GetIcon(mditab->img_list, item->img, ILD_NORMAL);
        if(icon != NULL) {
            xdraw_blit_HICON(ctx->canvas, icon, &layout.icon_rect);
            DestroyIcon(icon);
        }
    }

    /* Paint item text. */
    if(item->text != NULL) {
        xdraw_brush_solid_set_color(ctx->solid_brush, XDRAW_RGB(0,0,0));
        xdraw_draw_string(ctx->canvas, ctx->font, &layout.text_rect, item->text,
                _tcslen(item->text), ctx->solid_brush, XDRAW_STRING_NOWRAP |
                XDRAW_STRING_CLIP | XDRAW_STRING_END_ELLIPSIS);
    }

    /* Paint border of the item. */
    xdraw_canvas_set_clip(ctx->canvas, &clip_rect, NULL);
    xdraw_brush_solid_set_color(ctx->solid_brush, MDITAB_COLOR_BORDER);
    xdraw_draw_path(ctx->canvas, ctx->solid_brush, path, 1.0f);
    xdraw_canvas_reset_clip(ctx->canvas);

    /* For the active tab, paint the bottom line. */
    line.x0 = 0.0f;
    line.y0 = y1 - 1.0f;
    line.x1 = blit_rect.x0 - 1.0f;
    line.y1 = y1 - 1.0f;
    xdraw_draw_line(ctx->canvas, ctx->solid_brush, &line, 1.0f);
    line.x0 = blit_rect.x1;
    line.x1 = client->right;
    xdraw_draw_line(ctx->canvas, ctx->solid_brush, &line, 1.0f);

    /* Destroy the item shape path. */
    xdraw_path_destroy(path);

    /* Paint scrolling blocks. */
    if(left_block)
        mditab_paint_scroll_block(mditab, ctx, floorf(area_x0 - r), y0, y1, +1);
    if(right_block)
        mditab_paint_scroll_block(mditab, ctx, ceilf(area_x1 + r), y0, y1, -1);
}

static BOOL
mditab_paint_with_ctx(mditab_t* mditab, HDC dc, mditab_paint_t* ctx,
                      RECT* dirty, BOOL erase)
{
    RECT client;
    int area_x0, area_x1;
    WORD i, n;
    BOOL enabled;
    HWND parent_win;
    RECT parent_rect;
    HDC background_dc;
    HBITMAP background_bmp;
    HBITMAP old_bmp;
    POINT old_origin;
    xdraw_image_t* background_image;
    BOOL paint_selected_item = FALSE;

    GetClientRect(mditab->win, &client);
    area_x0 = mditab->area_margin0;
    area_x1 = client.right - mditab->area_margin1;
    n = mditab_count(mditab);
    enabled = IsWindowEnabled(mditab->win);

    xdraw_canvas_begin_paint(ctx->canvas);

    if(erase)
        xdraw_clear(ctx->canvas, MDITAB_COLOR_BACKGROUND);

    /* Paint auxiliary buttons */
    if(mditab->btn_mask & BTNMASK_LSCROLL)
        mditab_paint_button(mditab, ctx, BTNID_LSCROLL, (enabled && mditab->scroll_x > 0));
    if(mditab->btn_mask & BTNMASK_CLOSE)
        mditab_paint_button(mditab, ctx, BTNID_CLOSE, (enabled && n > 0));
    if(mditab->btn_mask & BTNMASK_LIST)
        mditab_paint_button(mditab, ctx, BTNID_LIST, (enabled && n > 0));
    if(mditab->btn_mask & BTNMASK_RSCROLL)
        mditab_paint_button(mditab, ctx, BTNID_RSCROLL, (enabled && mditab->scroll_x < mditab->scroll_x_max));

    if(area_x1 <= area_x0)
        goto skip_item_painting;

    /* Make parent paint into a temporary bitmap. (Used to paint background
     * of items.) */
    parent_win = GetAncestor(mditab->win, GA_ROOT);
    mc_rect_copy(&parent_rect, &client);
    MapWindowPoints(mditab->win, parent_win, (POINT*) &parent_rect, 2);
    background_dc = CreateCompatibleDC(dc);
    if(MC_ERR(background_dc == NULL)) {
        MC_TRACE_ERR("mditab_paint_with_ctx: CreateCompatibleDC() failed.");
        goto err_CreateCompatibleDC;
    }
    background_bmp = CreateCompatibleBitmap(dc, mc_width(&parent_rect), mc_height(&parent_rect));
    if(MC_ERR(background_bmp == NULL)) {
        MC_TRACE_ERR("mditab_paint_with_ctx: CreateCompatibleBitmap() failed.");
        goto err_CreateCompatibleBitmap;
    }
    old_bmp = SelectObject(background_dc, background_bmp);
    OffsetViewportOrgEx(background_dc, -parent_rect.left, -parent_rect.top, &old_origin);
    MC_SEND(parent_win, WM_PRINT, background_dc, PRF_ERASEBKGND | PRF_CLIENT);
    SetViewportOrgEx(background_dc, old_origin.x, old_origin.y, NULL);
    SelectObject(background_dc, old_bmp);
    background_image = xdraw_image_create_from_HBITMAP(background_bmp);
    if(MC_ERR(background_image == NULL)) {
        MC_TRACE("mditab_paint_with_ctx: xdraw_image_create_from_HBITMAP() failed.");
        goto err_xdraw_image_create_from_HBITMAP;
    }

    /* Paint items */
    if(n > 0) {
        xdraw_rect_t sel_rect;
        int r = (mc_height(&client) - MDITAB_ITEM_TOP_MARGIN + 1) / 2;  /* "+1" to compensate rounding errors */

        for(i = 0; i < n; i++) {
            mditab_item_t* item = mditab_item(mditab, i);
            int x0 = area_x0 - mditab->scroll_x + item->x0;
            int x1 = area_x0 - mditab->scroll_x + item->x1;
            xdraw_rect_t item_rect;

            if(x1 <= area_x0 - r)
                continue;
            if(x0 > area_x1 + r)
                break;

            if(i == mditab->item_selected) {
                /* We need to paint the selected item as last one, due to the
                 * overlaps. Remember its rect. */
                paint_selected_item = TRUE;
                sel_rect.x0 = x0;
                sel_rect.x1 = x1;
                sel_rect.y0 = (float) MDITAB_ITEM_TOP_MARGIN;
                sel_rect.y1 = client.bottom;
                continue;
            }

            /* Paint an item. */
            item_rect.x0 = x0;
            item_rect.x1 = x1;
            item_rect.y0 = (float) MDITAB_ITEM_TOP_MARGIN;
            item_rect.y1 = client.bottom;
            mditab_paint_item(mditab, ctx, &client, item, &item_rect,
                              area_x0, area_x1, background_image, FALSE,
                              (i == mditab->item_hot));
        }

        /* Paint the selected item. */
        if(paint_selected_item) {
            mditab_item_t* item = mditab_item(mditab, mditab->item_selected);
            mditab_paint_item(mditab, ctx, &client, item, &sel_rect,
                              area_x0, area_x1, background_image, TRUE,
                              (mditab->item_selected == mditab->item_hot));
        }
    }

    /* Painting of selected item does also the bottom border as a side effect.
     * If there are no items or selected items is out of the view port, we have
     * to paint it here. */
    if(!paint_selected_item) {
        xdraw_line_t line;
        line.x0 = -1.0f;
        line.y0 = client.bottom - 1.0f;
        line.x1 = client.right;
        line.y1 = client.bottom - 1.0f;
        xdraw_brush_solid_set_color(ctx->solid_brush, MDITAB_COLOR_BORDER);
        xdraw_draw_line(ctx->canvas, ctx->solid_brush, &line, 1.0f);
    }

    /* Clean-up */
    xdraw_image_destroy(background_image);
err_xdraw_image_create_from_HBITMAP:
    DeleteObject(background_bmp);
err_CreateCompatibleBitmap:
    DeleteDC(background_dc);
err_CreateCompatibleDC:
skip_item_painting:
    return xdraw_canvas_end_paint(ctx->canvas);
}

static void
mditab_paint(mditab_t* mditab)
{
    PAINTSTRUCT ps;
    mditab_paint_t* ctx;
    mditab_paint_t tmp_ctx;

    BeginPaint(mditab->win, &ps);
    if(mditab->no_redraw)
        goto no_paint;

    if(mditab->paint_ctx != NULL) {
        /* Use the cached paint context. */
        ctx = mditab->paint_ctx;
    } else {
        /* Initialize new context. */
        DWORD flags;
        xdraw_canvas_t* c;

        flags = XDRAW_CANVAS_GDICOMPAT;
        if(mditab->style & MC_MTS_DOUBLEBUFFER)
            flags |= XDRAW_CANVAS_DOUBLEBUFER;

        c = xdraw_canvas_create_with_paintstruct(mditab->win, &ps, flags);
        if(MC_ERR(c == NULL))
            goto no_paint;

        mditab_paint_ctx_init(&tmp_ctx, c, mditab->font);
        ctx = &tmp_ctx;
    }

    /* Do the painting itself. */
    if(mditab_paint_with_ctx(mditab, ps.hdc, ctx, &ps.rcPaint, ps.fErase)) {
        /* We are allowed to cache the context for reuse. */
        if(ctx == &tmp_ctx) {
            ctx = (mditab_paint_t*) malloc(sizeof(mditab_paint_t));
            if(ctx != NULL) {
                memcpy(ctx, &tmp_ctx, sizeof(mditab_paint_t));
                mditab->paint_ctx = ctx;
            } else {
                mditab_paint_ctx_fini(&tmp_ctx);
            }
        }
    } else {
        /* We are instructed to destroy the context. */
        mditab_paint_ctx_fini(ctx);
        if(mditab->paint_ctx != NULL) {
            free(mditab->paint_ctx);
            mditab->paint_ctx = NULL;
        }
    }

no_paint:
    EndPaint(mditab->win, &ps);
}

static void
mditab_printclient(mditab_t* mditab, HDC dc)
{
    mditab_paint_t ctx;
    xdraw_canvas_t* c;
    RECT rect;

    GetClientRect(mditab->win, &rect);

    c = xdraw_canvas_create_with_dc(dc, &rect, XDRAW_CANVAS_GDICOMPAT);
    if(MC_ERR(c == NULL)) {
        MC_TRACE("xdraw_canvas_create_with_dc: xdraw_canvas_create_with_dc() failed.");
        return;
    }

    mditab_paint_ctx_init(&ctx, c, mditab->font);
    mditab_paint_with_ctx(mditab, dc, &ctx, &rect, TRUE);
    mditab_paint_ctx_fini(&ctx);
}

static void
mditab_notify_sel_change(mditab_t* mditab, int old_index, int new_index)
{
    MC_NMMTSELCHANGE notify;

    notify.hdr.hwndFrom = mditab->win;
    notify.hdr.idFrom = GetDlgCtrlID(mditab->win);
    notify.hdr.code = MC_MTN_SELCHANGE;
    notify.iItemOld = old_index;
    notify.lParamOld = (old_index >= 0  ?  mditab_item(mditab, old_index)->lp  :  0);
    notify.iItemNew = new_index;
    notify.lParamNew = (new_index >= 0  ?  mditab_item(mditab, new_index)->lp  :  0);

    MC_SEND(mditab->notify_win, WM_NOTIFY, notify.hdr.idFrom, &notify);
}

static int
mditab_insert_item(mditab_t* mditab, int index, MC_MTITEM* id, BOOL unicode)
{
    TCHAR* item_text;
    mditab_item_t* item;

    MDITAB_TRACE("mditab_insert_item(%p, %d, %p, %d)", mditab, index, id, unicode);

    if(MC_ERR(id == NULL)) {
        MC_TRACE("mditab_insert_item: id == NULL");
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    if(MC_ERR(index < 0)) {
        MC_TRACE("mditab_insert_item: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    if(index > mditab_count(mditab))
        index = mditab_count(mditab);

    /* Preallocate the string now, so we can stop early if there is not
     * enough memory. */
    if((id->dwMask & MC_MTIF_TEXT) && id->pszText != NULL) {
        item_text = (TCHAR*) mc_str(id->pszText,
                (unicode ? MC_STRW : MC_STRA), MC_STRT);
        if(MC_ERR(item_text == NULL)) {
            MC_TRACE("mditab_insert_item: mc_str() failed.");
            mc_send_notify(mditab->notify_win, mditab->win, NM_OUTOFMEMORY);
            return -1;
        }
    } else {
        item_text = NULL;
    }

    /* Allocate a cell in item DSA */
    item = (mditab_item_t*) dsa_insert_raw(&mditab->items, index);
    if(MC_ERR(item == NULL)) {
        MC_TRACE("mditab_insert_item: dsa_insert_raw() failed.");
        if(item_text != NULL)
            free(item_text);
        mc_send_notify(mditab->notify_win, mditab->win, NM_OUTOFMEMORY);
        return -1;
    }

    /* Setup the new item */
    item->text = item_text;
    item->img = ((id->dwMask & MC_MTIF_IMAGE) ? id->iImage : (SHORT)MC_I_IMAGENONE);
    item->lp = ((id->dwMask & MC_MTIF_PARAM) ? id->lParam : 0);
    item->x0 = (index > 0  ?  mditab_item(mditab, index-1)->x1  :  0);
    item->x1 = item->x0;
    item->ideal_width = 0;

    /* Update stored item indexes */
    if(index <= mditab->item_selected)
        mditab->item_selected++;
    if(mditab->item_selected < 0) {
        mditab->item_selected = index;
        mditab_notify_sel_change(mditab, -1, index);
    }
    if(index <= mditab->item_mclose)
        mditab->item_mclose++;
    /* We don't update ->item_hot. This is determined by mouse and set
     * in mditab_update_layout() below anyway... */

    /* Refresh */
    mditab_update_layout(mditab, TRUE);
    return index;
}

static BOOL
mditab_set_item(mditab_t* mditab, int index, MC_MTITEM* id, BOOL unicode)
{
    mditab_item_t* item;

    MDITAB_TRACE("mditab_set_item(%p, %d, %p, %d)", mditab, index, id, unicode);

    if(MC_ERR(id == NULL)) {
        MC_TRACE("mditab_set_item: id == NULL");
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if(MC_ERR(index < 0  ||  index >= mditab_count(mditab))) {
        MC_TRACE("mditab_set_item: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    item = mditab_item(mditab, index);

    if(id->dwMask & MC_MTIF_TEXT) {
        TCHAR* item_text;

        item_text = (TCHAR*) mc_str(id->pszText, (unicode ? MC_STRW : MC_STRA), MC_STRT);
        if(MC_ERR(item_text == NULL && id->pszText != NULL)) {
            MC_TRACE("mditab_set_item: mc_str() failed.");
            mc_send_notify(mditab->notify_win, mditab->win, NM_OUTOFMEMORY);
            return FALSE;
        }

        if(item->text != NULL)
            free(item->text);
        item->text = item_text;

        /* New text implies new ideal width. */
        item->ideal_width = 0;
    }
    if(id->dwMask & MC_MTIF_IMAGE)
        item->img = (SHORT)id->iImage;
    if(id->dwMask & MC_MTIF_PARAM)
        item->lp = id->lParam;

    mditab_invalidate_item(mditab, index);
    if(mditab->item_def_width == 0  &&  (id->dwMask & MC_MTIF_TEXT))
        mditab_update_layout(mditab, TRUE);

    return TRUE;
}

static BOOL
mditab_get_item(mditab_t* mditab, int index, MC_MTITEM* id, BOOL unicode)
{
    mditab_item_t* item;

    MDITAB_TRACE("mditab_get_item(%p, %d, %p, %d)", mditab, index, id, unicode);

    if(MC_ERR(id == NULL)) {
        MC_TRACE("mditab_get_item: id == NULL");
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if(MC_ERR(index < 0  || index >= mditab_count(mditab))) {
        MC_TRACE("mditab_get_item: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    item = mditab_item(mditab, index);

    if(id->dwMask & MC_MTIF_TEXT) {
        mc_str_inbuf(item->text, MC_STRT, id->pszText,
                (unicode ? MC_STRW : MC_STRA), id->cchTextMax);
    }
    if(id->dwMask & MC_MTIF_IMAGE)
        id->iImage = item->img;
    if(id->dwMask & MC_MTIF_PARAM)
        id->lParam = item->lp;

    return TRUE;
}

static void
mditab_notify_delete_item(mditab_t* mditab, int index)
{
    MC_NMMTDELETEITEM notify;

    notify.hdr.hwndFrom = mditab->win;
    notify.hdr.idFrom = GetDlgCtrlID(mditab->win);
    notify.hdr.code = MC_MTN_DELETEITEM;
    notify.iItem = index;
    notify.lParam = mditab_item(mditab, index)->lp;

    MC_SEND(mditab->notify_win, WM_NOTIFY, notify.hdr.idFrom, &notify);
}

static BOOL
mditab_delete_item(mditab_t* mditab, int index)
{
    MDITAB_TRACE("mditab_delete_item(%p, %d)", mditab, index);

    if(index < 0  ||  index >= mditab_count(mditab)) {
        MC_TRACE("mditab_delete_item: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* We may need to change all item indexes referring to the item being
     * deleted. */
    if(mditab->scrolling_to_item  &&  index == mditab->scroll_x_desired) {
        mditab->scrolling_to_item = FALSE;
        mditab->scroll_x_desired = mditab->scroll_x;
    }

    if(index == mditab->item_selected) {
        int old_item_selected = mditab->item_selected;
        int n = mditab_count(mditab);
        if(mditab->item_selected < n-1)
            mditab->item_selected++;
        else
            mditab->item_selected = n-2;

        mditab_notify_sel_change(mditab, old_item_selected, mditab->item_selected);
    }

    if(index == mditab->item_mclose)
        mditab->item_mclose = -1;

    /* Do the delete. */
    mditab_invalidate_item(mditab, index);
    mditab_notify_delete_item(mditab, index);
    dsa_remove(&mditab->items, index, mditab_item_dtor);

    /* Update stored item indexes. */
    if(mditab->scrolling_to_item  &&  index < mditab->scroll_x_desired)
        mditab->scroll_x_desired--;
    if(index < mditab->item_selected)
        mditab->item_selected--;
    if(index < mditab->item_mclose)
        mditab->item_mclose--;
    mditab_reset_hot(mditab);

    /* Refresh */
    mditab_update_layout(mditab, TRUE);
    return TRUE;
}

static void
mditab_notify_delete_all_items(mditab_t* mditab)
{
    UINT i;

    if(mc_send_notify(mditab->notify_win, mditab->win, MC_MTN_DELETEALLITEMS) != 0) {
        /* App. has canceled sending per-item notifications. */
        return;
    }

    for(i = 0; i < mditab_count(mditab); i++)
        mditab_notify_delete_item(mditab, i);
}

static BOOL
mditab_delete_all_items(mditab_t* mditab)
{
    MDITAB_TRACE("mditab_notify_delete_all_items(%p)", mditab);

    if(mditab_count(mditab) == 0)
        return TRUE;

    if(mditab->item_selected >= 0) {
        int old_index = mditab->item_selected;
        mditab->item_selected = -1;
        mditab_notify_sel_change(mditab, old_index, mditab->item_selected);
    }

    mditab_notify_delete_all_items(mditab);
    dsa_clear(&mditab->items, mditab_item_dtor);

    mditab->item_hot = ITEM_HOT_NONE;
    mditab->item_mclose = -1;
    mditab->scrolling_to_item = FALSE;
    mditab->scroll_x_desired = 0;
    mditab->scroll_x = 0;
    mditab->scroll_x_max = 0;

    mditab_update_layout(mditab, FALSE);
    if(!mditab->no_redraw)
        InvalidateRect(mditab->win, NULL, TRUE);
    return TRUE;
}

static HIMAGELIST
mditab_set_img_list(mditab_t* mditab, HIMAGELIST img_list)
{
    HIMAGELIST old_img_list;

    MDITAB_TRACE("mditab_set_img_list(%p, %p)", mditab, img_list);

    if(img_list == mditab->img_list)
        return img_list;

    old_img_list = mditab->img_list;
    mditab->img_list = img_list;

    if(mditab->item_def_width != 0) {
        int old_icon_cx;
        int new_icon_cx;
        int dummy;

        if(old_img_list != NULL)
            ImageList_GetIconSize(old_img_list, &old_icon_cx, &dummy);
        else
            old_icon_cx = 0;

        if(img_list != NULL)
            ImageList_GetIconSize(img_list, &new_icon_cx, &dummy);
        else
            new_icon_cx = 0;

        if(old_icon_cx != new_icon_cx) {
            mditab_reset_ideal_widths(mditab);
            mditab_update_layout(mditab, FALSE);
        }
    }

    if(!mditab->no_redraw)
        InvalidateRect(mditab->win, NULL, TRUE);
    return old_img_list;
}

static void
mditab_scroll_to_item(mditab_t* mditab, int index)
{
    mditab->scrolling_to_item = TRUE;
    mditab->scroll_x_desired = index;
    mditab_update_layout(mditab, TRUE);
}

static void
mditab_scroll_rel(mditab_t* mditab, int dx)
{
    int scroll_x_desired;

    scroll_x_desired = mditab->scroll_x + dx;

    if(scroll_x_desired < 0)
        scroll_x_desired = 0;
    if(scroll_x_desired > mditab->scroll_x_max)
        scroll_x_desired = mditab->scroll_x_max;

    if(mditab->scroll_x_desired != scroll_x_desired) {
        mditab->scrolling_to_item = FALSE;
        mditab->scroll_x_desired = scroll_x_desired;
        mditab_update_layout(mditab, TRUE);
    }
}

static BOOL
mditab_ensure_visible(mditab_t* mditab, int index)
{
    if(MC_ERR(index < 0  ||  index >= mditab_count(mditab))) {
        MC_TRACE("mditab_ensure_visible: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    mditab_scroll_to_item(mditab, index);
    return TRUE;
}

static int
mditab_set_cur_sel(mditab_t* mditab, int index)
{
    int old_sel_index;

    MDITAB_TRACE("mditab_set_cur_sel(%p, %d)", mditab, index);

    if(index < 0  ||  index >= mditab_count(mditab)) {
        MC_TRACE("mditab_set_cur_sel: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        index = -1;
    }

    old_sel_index = mditab->item_selected;
    if(index == old_sel_index)
        return old_sel_index;

    mditab->item_selected = index;
    mditab_ensure_visible(mditab, index);

    /* Redraw the tabs with changed status */
    if(old_sel_index >= 0)
        mditab_invalidate_item(mditab, old_sel_index);
    if(index >= 0)
        mditab_invalidate_item(mditab, index);

    /* Notify parent window */
    mditab_notify_sel_change(mditab, old_sel_index, index);

    return old_sel_index;
}

static void
mditab_measure_menu_icon(mditab_t* mditab, MEASUREITEMSTRUCT* mis)
{
    mditab_item_t* item = mditab_item(mditab, mis->itemID - 1000);

    if(mditab->img_list != NULL  &&  item->img >= 0) {
        int w, h;

        ImageList_GetIconSize(mditab->img_list, &w, &h);
        mis->itemWidth = w;
        mis->itemHeight = h;
    } else {
        mis->itemWidth = 0;
        mis->itemHeight = 0;
    }
}

static void
mditab_draw_menu_icon(mditab_t* mditab, DRAWITEMSTRUCT* dis)
{
    mditab_item_t* item = mditab_item(mditab, dis->itemID - 1000);

    if(mditab->img_list != NULL  &&  item->img >= 0) {
        ImageList_Draw(mditab->img_list, item->img, dis->hDC,
                       dis->rcItem.left, dis->rcItem.top, ILD_TRANSPARENT);
    }
}

static BOOL
mditab_close_item(mditab_t* mditab, int index)
{
    MC_NMMTCLOSEITEM notify;

    MDITAB_TRACE("mditab_close_item(%p, %d)", mditab, index);

    if(index < 0  ||  index >= mditab_count(mditab)) {
        MC_TRACE("mditab_close_item: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    notify.hdr.hwndFrom = mditab->win;
    notify.hdr.idFrom = GetDlgCtrlID(mditab->win);
    notify.hdr.code = MC_MTN_CLOSEITEM;
    notify.iItem = index;
    notify.lParam = mditab_item(mditab, index)->lp;

    if(MC_SEND(mditab->notify_win, WM_NOTIFY, notify.hdr.idFrom, &notify) == FALSE)
        return mditab_delete_item(mditab, index);
    else
        return FALSE;
}

static BOOL
mditab_set_item_width(mditab_t* mditab, MC_MTITEMWIDTH* tw)
{
    USHORT def_w;
    USHORT min_w;

    MDITAB_TRACE("mditab_set_item_width(%p, %p)", mditab, tw);

    if(tw != NULL) {
        def_w = (USHORT)tw->dwDefWidth;
        min_w = (USHORT)tw->dwMinWidth;
    } else {
        def_w = DEFAULT_ITEM_DEF_WIDTH;
        min_w = DEFAULT_ITEM_MIN_WIDTH;
    }

    if(def_w < min_w)
        def_w = min_w;

    if(def_w == mditab->item_def_width  &&  min_w == mditab->item_min_width)
        return TRUE;

    mditab->item_def_width = def_w;
    mditab->item_min_width = min_w;
    mditab_update_layout(mditab, TRUE);
    return TRUE;
}

static BOOL
mditab_get_item_width(mditab_t* mditab, MC_MTITEMWIDTH* tw)
{
    tw->dwDefWidth = mditab->item_def_width;
    tw->dwMinWidth = mditab->item_min_width;
    return TRUE;
}

static BOOL
mditab_get_item_rect(mditab_t* mditab, WORD index, RECT* rect, BOOL whole)
{
    RECT client;
    mditab_item_t* item;

    if(MC_ERR(index >= mditab_count(mditab))) {
        MC_TRACE("mditab_get_item_rect: invalid tab index (%d)", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    GetClientRect(mditab->win, &client);
    item = mditab_item(mditab, index);

    rect->left = mditab->area_margin0 - mditab->scroll_x + item->x0;
    rect->top = MDITAB_ITEM_TOP_MARGIN;
    rect->right = mditab->area_margin0 - mditab->scroll_x + item->x1;
    rect->bottom = client.bottom;

    if(!whole) {
        int area_x0 = mditab->area_margin0;
        int area_x1 = client.right - mditab->area_margin1;

        if(rect->left < area_x0)
            rect->left = area_x0;
        if(rect->right > area_x1)
            rect->right = area_x1;
    }

    return TRUE;
}

static BOOL
mditab_key_up(mditab_t* mditab, int key_code, DWORD key_data)
{
    int index;

    MDITAB_TRACE("mditab_key_up(%p, %d, 0x%x)", mditab, key_code, key_data);

    switch(key_code) {
        case VK_LEFT:
            index = mditab->item_selected - 1;
            break;
        case VK_RIGHT:
            index = mditab->item_selected + 1;
            break;
        default:
            return FALSE;
    }

    if(0 <= index  &&  index < mditab_count(mditab))
        mditab_set_cur_sel(mditab, index);

    return TRUE;
}

static void
mditab_list_items(mditab_t* mditab)
{
    HMENU popup;
    MENUINFO mi;
    MENUITEMINFO mii;
    TPMPARAMS tpm_param;
    int i, n;
    int cmd;

    MDITAB_TRACE("mditab_list_items(%p)", mditab);

    /* Construct the menu */
    popup = CreatePopupMenu();
    if(MC_ERR(popup == NULL)) {
        MC_TRACE("mditab_list_items: CreatePopupMenu() failed.");
        return;
    }

    /* Disable the place for check marks in the menu as it is never used
     * and looks ugly. */
    mi.cbSize = sizeof(MENUINFO);
    mi.fMask = MIM_STYLE;
    mi.dwStyle = (mditab->img_list != NULL ? MNS_CHECKORBMP : MNS_NOCHECK);
    SetMenuInfo(popup, &mi);

    /* Install menu items */
    mii.cbSize = sizeof(MENUITEMINFO);
    mii.fMask = MIIM_DATA | MIIM_ID | MIIM_STATE | MIIM_STRING;
    if(mditab->img_list != NULL) {
        mii.fMask |= MIIM_BITMAP;
        mii.hbmpItem = HBMMENU_CALLBACK;
    }
    n = mditab_count(mditab);
    for(i = 0; i < n; i++) {
        mii.dwItemData = i;
        mii.wID = 1000 + i;
        mii.fState = (i == mditab->item_selected ? MFS_DEFAULT : 0);
        mii.dwTypeData = mditab_item(mditab, i)->text;
        mii.cch = _tcslen(mii.dwTypeData);
        InsertMenuItem(popup, i, TRUE, &mii);
    }

    /* Show the menu */
    tpm_param.cbSize = sizeof(TPMPARAMS);
    mditab_button_rect(mditab, BTNID_LIST, &tpm_param.rcExclude);
    MapWindowPoints(mditab->win, HWND_DESKTOP, (POINT*) &tpm_param.rcExclude, 2);
    cmd = TrackPopupMenuEx(popup, TPM_LEFTBUTTON | TPM_RIGHTALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                     tpm_param.rcExclude.right, tpm_param.rcExclude.bottom,
                     mditab->win, &tpm_param);
    DestroyMenu(popup);
    if(cmd != 0)
        mditab_set_cur_sel(mditab, cmd - 1000);
}

static void
mditab_left_button_down(mditab_t* mditab, UINT keys, short x, short y)
{
    int index;
    MC_MTHITTESTINFO hti;

    if((mditab->style & MC_MTS_FOCUSMASK) == MC_MTS_FOCUSONBUTTONDOWN)
        SetFocus(mditab->win);

    hti.pt.x = x;
    hti.pt.y = y;
    index = mditab_hit_test(mditab, &hti, FALSE);

    MDITAB_TRACE("mditab_left_button_down(): hittest index %d, flags 0x%x",
                 index, hti.flags);

    if(index >= 0) {
        /* Handle item selection. */
        if(index == mditab->item_selected) {
            if((mditab->style & MC_MTS_FOCUSMASK) != MC_MTS_FOCUSNEVER)
                SetFocus(mditab->win);
        } else {
            mditab_set_cur_sel(mditab, index);
        }
    } else {
        /* Handle auxiliary buttons */
        int btn_id = -1;

        switch(hti.flags) {
            case MC_MTHT_ONLEFTSCROLLBUTTON:   btn_id = BTNID_LSCROLL; break;
            case MC_MTHT_ONRIGHTSCROLLBUTTON:  btn_id = BTNID_RSCROLL; break;
            case MC_MTHT_ONLISTBUTTON:         btn_id = BTNID_LIST; break;
            case MC_MTHT_ONCLOSEBUTTON:        btn_id = BTNID_CLOSE; break;
        }

        if(btn_id >= 0) {
            if(btn_id == BTNID_LIST) {
                /* We handle BTNID_LIST specially because the popup menu is not
                 * friend with CaptureMouse etc. */
                RECT btn_rect;

                mditab_button_rect(mditab, BTNID_LIST, &btn_rect);
                mditab->btn_pressed = TRUE;
                RedrawWindow(mditab->win, &btn_rect, NULL, RDW_INTERNALPAINT);
                mditab_list_items(mditab);
                mditab->btn_pressed = FALSE;
                mditab_invalidate_button(mditab, btn_id);
            } else {
                SetCapture(mditab->win);
                mditab->btn_pressed = TRUE;
                mditab_invalidate_button(mditab, btn_id);

                switch(btn_id) {
                    case BTNID_LSCROLL:
                        mditab_scroll_rel(mditab, -DEFAULT_ITEM_MIN_WIDTH);
                        break;

                    case BTNID_RSCROLL:
                        mditab_scroll_rel(mditab, +DEFAULT_ITEM_MIN_WIDTH);
                        break;
                }
            }
        }
    }
}

static void
mditab_left_button_up(mditab_t* mditab, UINT keys, short x, short y)
{
    if(mditab->item_hot < 0  &&  mditab->item_hot != ITEM_HOT_NONE  &&  mditab->btn_pressed) {
        int btn_id = mditab_hot_button(mditab);
        if(btn_id == BTNID_CLOSE  &&  mditab->item_selected >= 0)
            mditab_close_item(mditab, mditab->item_selected);
        mditab_invalidate_button(mditab, btn_id);
    } else {
        mc_send_notify(mditab->notify_win, mditab->win, NM_CLICK);
    }

   if(GetCapture() == mditab->win) {
        mditab->btn_pressed = FALSE;
        ReleaseCapture();
        mc_send_notify(mditab->notify_win, mditab->win, NM_RELEASEDCAPTURE);
    }
}

static void
mditab_middle_button_down(mditab_t* mditab, UINT keys, short x, short y)
{
    MC_MTHITTESTINFO hti;

    if(!(mditab->style & MC_MTS_CLOSEONMCLICK))
        return;

    hti.pt.x = x;
    hti.pt.y = y;
    mditab->item_mclose = mditab_hit_test(mditab, &hti, FALSE);

    if(mditab->item_mclose >= 0)
        SetCapture(mditab->win);
}

static void
mditab_middle_button_up(mditab_t* mditab, UINT keys, short x, short y)
{
    int index;
    MC_MTHITTESTINFO hti;

    if(GetCapture() == mditab->win) {
        ReleaseCapture();
        mc_send_notify(mditab->notify_win, mditab->win, NM_RELEASEDCAPTURE);
    }

    if((mditab->style & MC_MTS_CLOSEONMCLICK) == 0  ||  mditab->item_mclose < 0)
        return;

    hti.pt.x = x;
    hti.pt.y = y;
    index = mditab_hit_test(mditab, &hti, FALSE);

    if(index == mditab->item_mclose)
        mditab_close_item(mditab, index);
    mditab->item_mclose = -1;
}

static void
mditab_change_focus(mditab_t* mditab)
{
    /* The selected tab needs refresh to draw/hide focus rect. */
    if(mditab->item_selected < 0)
        return;
    mditab_invalidate_item(mditab, mditab->item_selected);
}

static void
mditab_style_changed(mditab_t* mditab, STYLESTRUCT* ss)
{
    static const DWORD style_mask_for_layout =
                (MC_MTS_CBMASK | MC_MTS_TLBMASK | MC_MTS_SCROLLALWAYS);
    BOOL do_update_layout = FALSE;

    mditab->style = ss->styleNew;

    if((ss->styleOld & style_mask_for_layout) != (ss->styleNew & style_mask_for_layout))
        do_update_layout = TRUE;

    if((ss->styleOld & MC_MTS_ANIMATE) && !(ss->styleNew & MC_MTS_ANIMATE)) {
        if(mditab->animation != NULL) {
            anim_stop(mditab->animation);
            mditab->animation = NULL;
            do_update_layout = TRUE;
        }
    }

    if(do_update_layout)
        mditab_update_layout(mditab, FALSE);

    if(!mditab->no_redraw)
        InvalidateRect(mditab->win, NULL, TRUE);
}

static mditab_t*
mditab_nccreate(HWND win, CREATESTRUCT* cs)
{
    mditab_t* mditab;

    mditab = (mditab_t*) malloc(sizeof(mditab_t));
    if(MC_ERR(mditab == NULL)) {
        MC_TRACE("mditab_nccreate malloc() failed.");
        return NULL;
    }

    memset(mditab, 0, sizeof(mditab_t));

    mditab->win = win;
    mditab->notify_win = cs->hwndParent;
    dsa_init(&mditab->items, sizeof(mditab_item_t));
    mditab->item_selected = -1;
    mditab->item_hot = ITEM_HOT_NONE;
    mditab->item_mclose = -1;
    mditab->item_min_width = DEFAULT_ITEM_MIN_WIDTH;
    mditab->item_def_width = DEFAULT_ITEM_DEF_WIDTH;
    mditab->style = cs->style;

    /* This initializes mditab_t::btn_mask, area_margin0, area_margin1 */
    mditab_update_layout(mditab, FALSE);

    return mditab;
}

static int
mditab_create(mditab_t* mditab, CREATESTRUCT* cs)
{
    /* noop */
    return 0;
}

static void
mditab_destroy(mditab_t* mditab)
{
    /* noop */
}

static void
mditab_ncdestroy(mditab_t* mditab)
{
    mditab_notify_delete_all_items(mditab);
    dsa_fini(&mditab->items, mditab_item_dtor);

    if(mditab->animation != NULL)
        anim_stop(mditab->animation);

    free(mditab);
}

static LRESULT CALLBACK
mditab_proc(HWND win, UINT msg, WPARAM wp, LPARAM lp)
{
    mditab_t* mditab = (mditab_t*) GetWindowLongPtr(win, 0);

    switch(msg) {
        case WM_PAINT:
            mditab_paint(mditab);
            return 0;

        case WM_PRINTCLIENT:
            mditab_printclient(mditab, (HDC) wp);
            return 0;

        case WM_DISPLAYCHANGE:
            mditab_free_cached_paint_ctx(mditab);
            InvalidateRect(win, NULL, FALSE);
            break;

        case WM_ERASEBKGND:
            /* Keep it on WM_PAINT */
            return FALSE;

        case WM_TIMER:
            if(wp == ANIM_TIMER_ID) {
                anim_step(mditab->animation);
                mditab_update_layout(mditab, TRUE);
                return 0;
            }
            break;

        case MC_MTM_GETITEMCOUNT:
            return mditab_count(mditab);

        case MC_MTM_INSERTITEMW:
        case MC_MTM_INSERTITEMA:
            return mditab_insert_item(mditab, (int)wp, (MC_MTITEM*)lp, (msg == MC_MTM_INSERTITEMW));

        case MC_MTM_SETITEMW:
        case MC_MTM_SETITEMA:
            return mditab_set_item(mditab, (int)wp, (MC_MTITEM*)lp, (msg == MC_MTM_SETITEMW));

        case MC_MTM_GETITEMW:
        case MC_MTM_GETITEMA:
            return mditab_get_item(mditab, (int)wp, (MC_MTITEM*)lp, (msg == MC_MTM_GETITEMW));

        case MC_MTM_DELETEITEM:
            return mditab_delete_item(mditab, wp);

        case MC_MTM_CLOSEITEM:
            return mditab_close_item(mditab, wp);

        case MC_MTM_HITTEST:
            return mditab_hit_test(mditab, (MC_MTHITTESTINFO*)lp, TRUE);

        case MC_MTM_SETCURSEL:
            return mditab_set_cur_sel(mditab, wp);

        case MC_MTM_GETCURSEL:
            return mditab->item_selected;

        case MC_MTM_DELETEALLITEMS:
            return mditab_delete_all_items(mditab);

        case MC_MTM_SETIMAGELIST:
            return (LRESULT) mditab_set_img_list(mditab, (HIMAGELIST) lp);

        case MC_MTM_GETIMAGELIST:
            return (LRESULT) mditab->img_list;

        case MC_MTM_SETITEMWIDTH:
            return (LRESULT) mditab_set_item_width(mditab, (MC_MTITEMWIDTH*)lp);

        case MC_MTM_GETITEMWIDTH:
            return (LRESULT) mditab_get_item_width(mditab, (MC_MTITEMWIDTH*)lp);

        case MC_MTM_INITSTORAGE:
            return (dsa_reserve(&mditab->items, (UINT)wp) == 0 ? TRUE : FALSE);

        case MC_MTM_GETITEMRECT:
            return mditab_get_item_rect(mditab, LOWORD(wp), (RECT*) lp, (HIWORD(wp)) != 0);

        case MC_MTM_ENSUREVISIBLE:
            return mditab_ensure_visible(mditab, wp);

        case WM_LBUTTONDOWN:
            mditab_left_button_down(mditab, wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_LBUTTONUP:
            mditab_left_button_up(mditab, wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MBUTTONDOWN:
            mditab_middle_button_down(mditab, wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MBUTTONUP:
            mditab_middle_button_up(mditab, wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_RBUTTONUP:
            mc_send_notify(mditab->notify_win, win, NM_RCLICK);
            return 0;

        case WM_MOUSEMOVE:
            mditab_mouse_move(mditab, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MOUSELEAVE:
            mditab_mouse_leave(mditab);
            return 0;

        case WM_SIZE:
            if(mditab->paint_ctx != NULL)
                xdraw_canvas_resize(mditab->paint_ctx->canvas, LOWORD(lp), HIWORD(lp));
            mditab_update_layout(mditab, TRUE);
            return 0;

        case WM_KEYUP:
            if(mditab_key_up(mditab, (int)wp, (DWORD)lp))
                return 0;
            break;

        case WM_CAPTURECHANGED:
            if(mditab->btn_pressed) {
                if(mditab->item_hot < 0  &&  mditab->item_hot != ITEM_HOT_NONE)
                    mditab_invalidate_button(mditab, mditab_hot_button(mditab));
                mditab->btn_pressed = FALSE;
                mditab->item_hot = ITEM_HOT_NONE;
            }
            return 0;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            mditab_change_focus(mditab);
            break;

        case WM_GETFONT:
            return (LRESULT) mditab->font;

        case WM_SETFONT:
            mditab->font = (HFONT) wp;
            if(mditab->item_def_width == 0) {
                mditab_reset_ideal_widths(mditab);
                mditab_update_layout(mditab, FALSE);
            }
            if((BOOL) lp  &&  !mditab->no_redraw)
                InvalidateRect(win, NULL, TRUE);
            return 0;

        case WM_MEASUREITEM:
            mditab_measure_menu_icon(mditab, (MEASUREITEMSTRUCT*) lp);
            return TRUE;

        case WM_DRAWITEM:
            if(wp == 0)
                mditab_draw_menu_icon(mditab, (DRAWITEMSTRUCT*) lp);
            return TRUE;

        case WM_SETREDRAW:
            mditab->no_redraw = !wp;
            if(!mditab->no_redraw)
                RedrawWindow(win, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;

        case WM_GETDLGCODE:
            /* FIXME: Wine-1.1.2/dlls/comctl32/tab.c returns (DLGC_WANTARROWS
             *        | DLGC_WANTCHARS). Why? Shouldn't we follow it? */
            return DLGC_WANTARROWS;

        case WM_STYLECHANGED:
            if(wp == GWL_STYLE)
                mditab_style_changed(mditab, (STYLESTRUCT*) lp);
            break;

        case WM_SYSCOLORCHANGE:
            if(!mditab->no_redraw)
                RedrawWindow(win, NULL, NULL, RDW_INVALIDATE);
            break;

        case CCM_SETNOTIFYWINDOW:
        {
            HWND old = mditab->notify_win;
            mditab->notify_win = (wp ? (HWND) wp : GetAncestor(win, GA_PARENT));
            return (LRESULT) old;
        }

        case WM_NCCREATE:
            mditab = mditab_nccreate(win, (CREATESTRUCT*)lp);
            if(MC_ERR(mditab == NULL))
                return FALSE;
            SetWindowLongPtr(win, 0, (LONG_PTR)mditab);
            return TRUE;

        case WM_CREATE:
            return (mditab_create(mditab, (CREATESTRUCT*)lp) == 0 ? 0 : -1);

        case WM_DESTROY:
            mditab_destroy(mditab);
            return 0;

        case WM_NCDESTROY:
            if(mditab)
                mditab_ncdestroy(mditab);
            return 0;
    }

    return DefWindowProc(win, msg, wp, lp);
}


int
mditab_init_module(void)
{
    WNDCLASS wc = { 0 };

    wc.style = CS_GLOBALCLASS | CS_PARENTDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mditab_proc;
    wc.cbWndExtra = sizeof(mditab_t*);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = mditab_wc;
    if(MC_ERR(RegisterClass(&wc) == 0)) {
        MC_TRACE_ERR("mditab_init_module: RegisterClass() failed");
        return -1;
    }

    return 0;
}

void
mditab_fini_module(void)
{
    UnregisterClass(mditab_wc, NULL);
}


