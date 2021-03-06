/*	vcode.c
	Copyright (C) 2013-2014 Dmitry Groshev

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

#include "global.h"

#include "mygtk.h"
#include "memory.h"
#include "inifile.h"
#include "png.h"
#include "mainwindow.h"
#include "otherwindow.h"
#include "canvas.h"
#include "cpick.h"
#include "icons.h"
#include "fpick.h"
#include "prefs.h"
#include "vcode.h"

/* Make code not compile if it cannot work */
typedef char Opcodes_Too_Long[2 * (op_LAST <= WB_OPMASK) - 1];

/// V-CODE ENGINE

/* Max V-code subroutine nesting */
#define CALL_DEPTH 16
/* Max container widget nesting */
#define CONT_DEPTH 128
/* Max columns in a list */
#define MAX_COLS 16

#define GET_OP(S) ((int)*(void **)(S)[1] & WB_OPMASK)

#define VCODE_KEY "mtPaint.Vcode"

enum {
	pk_NONE = 0,
	pk_PACK,
	pk_PACKp,
	pk_XPACK,
	pk_XPACK1,
	pk_EPACK,
	pk_PACKEND,
	pk_TABLE,
	pk_TABLEx,
	pk_TABLEp,
	pk_TABLE2,
	pk_TABLE2x,
	pk_SCROLLVP,
	pk_SCROLLVPn,
	pk_SCROLLVPv,
	pk_BIN
};
#define pk_MASK     0xFF
#define pkf_FRAME  0x100
#define pkf_STACK  0x200
#define pkf_PARENT 0x400
#define pkf_SHOW   0x800

/* Internal datastore */

#define GET_VDATA(V) ((V)[1])

typedef struct {
	void *code;	// Noop tag, must be first field
	void ***dv;	// Pointer to dialog response
	void **destroy;	// Pre-destruction event slot
	char *ininame;	// Prefix for inifile vars
	int xywh[4];	// Stored window position & size
	int done;	// Set when destroyed
} v_dd;

/* Main toplevel, for anchoring dialogs and rendering pixmaps */
static GtkWidget *mainwindow;

/* From widget to its wdata */
void **get_wdata(GtkWidget *widget, char *id)
{
	return (gtk_object_get_data(GTK_OBJECT(widget), id ? id : VCODE_KEY));
}

/* From event to its originator */
void **origin_slot(void **slot)
{
	while (((int)*(void **)slot[1] & WB_OPMASK) >= op_EVT_0) slot -= 2;
	return (slot);
}

void dialog_event(void *ddata, void **wdata, int what, void **where)
{
	v_dd *vdata = GET_VDATA(wdata);

	if (((int)vdata->code & WB_OPMASK) != op_WDONE) return; // Paranoia
	if (vdata->dv) *vdata->dv = where;
}

/* !!! Warning: handlers should not access datastore after window destruction!
 * GTK+ refs objects for signal duration, but no guarantee every other toolkit
 * will behave alike - WJ */

static void get_evt_1(GtkObject *widget, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];

	((evt_fn)desc[1])(GET_DDATA(base), base, (int)desc[0] & WB_OPMASK, slot);
}

static void get_evt_1_t(GtkObject *widget, gpointer user_data)
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
		get_evt_1(widget, user_data);
}

void do_evt_1_d(void **slot)
{
	void **base = slot[0], **desc = slot[1];

	if (!desc[1]) run_destroy(base);
	else ((evt_fn)desc[1])(GET_DDATA(base), base, (int)desc[0] & WB_OPMASK, slot);
}

static gboolean get_evt_del(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	do_evt_1_d(user_data);
	return (TRUE); // it is for handler to decide, destroy it or not
}

static gboolean get_evt_key(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];
	key_ext key = {
		event->keyval, low_key(event), real_key(event), event->state };
	int res = ((evtkey_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot, &key);
#if GTK_MAJOR_VERSION == 1
	/* Return value alone doesn't stop GTK1 from running other handlers */
	if (res) gtk_signal_emit_stop_by_name(GTK_OBJECT(widget), "key_press_event");
#endif
	return (!!res);
}

void add_click(void **r, GtkWidget *widget)
{
	gtk_signal_connect_object(GTK_OBJECT(widget), "clicked",
		GTK_SIGNAL_FUNC(do_evt_1_d), r);
}

void add_del(void **r, GtkWidget *window)
{
	gtk_signal_connect(GTK_OBJECT(window), "delete_event",
		GTK_SIGNAL_FUNC(get_evt_del), r);
}

static void **skip_if(void **pp)
{
	int lp, mk;
	void **ifcode;

	ifcode = pp + 1 + (lp = WB_GETLEN((int)*pp));
	if (lp > 1) // skip till corresponding ENDIF
	{
		mk = (int)pp[2];
		while ((((int)*ifcode & WB_OPMASK) != op_ENDIF) ||
			((int)ifcode[1] != mk))
			ifcode += 1 + WB_GETLEN((int)*ifcode);
	}
	return (ifcode + 1 + (WB_GETLEN((int)*ifcode)));
}

/* Trigger events which need triggering */
static void trigger_things(void **wdata)
{
	char *data = GET_DDATA(wdata);
	void **slot, **desc;

	for (wdata = GET_WINDOW(wdata); wdata[1]; wdata += 2)
	{
		int op = GET_OP(wdata);
		/* Trigger events for nested widget */
		if ((op == op_MOUNT) || (op == op_PMOUNT))
		{
			GtkWidget *w = GTK_BIN(wdata[0])->child;
			if (w && (slot = get_wdata(w, NULL)))
				trigger_things(slot);
			continue;
		}
		if (op != op_TRIGGER) continue;
		slot = PREV_SLOT(wdata);
		desc = slot[1];
		((evt_fn)desc[1])(data, slot[0], (int)desc[0] & WB_OPMASK, slot);
	}
}

/* Predict how many _slots_ a V-code sequence could need */
// !!! With GCC inlining this, weird size fluctuations can happen
static int predict_size(void **ifcode, char *ddata)
{
	void **v, **pp, *rstack[CALL_DEPTH], **rp = rstack;
	int op, n = 2; // safety margin

	while (TRUE)
	{
		op = (int)*ifcode++;
		ifcode = (pp = ifcode) + WB_GETLEN(op);
		n += WB_GETREF(op);
		op &= WB_OPMASK;
		if (op < op_END_LAST) break; // End
		// Direct jump
		if (op == op_GOTO) ifcode = *pp;
		// Subroutine call/return
		else if (op == op_RET) ifcode = *--rp;
		else if (op == op_CALLp)
		{
			*rp++ = ifcode;
			v = *pp;
			if ((int)*(pp - 1) & WB_FFLAG)
				v = (void *)(ddata + (int)v);
			ifcode = *v;
		}
	}
	return (n);
}

// !!! And with inlining this, same problem
void table_it(GtkWidget *table, GtkWidget *it, int wh, int pad, int pack)
{
	int row = wh & 255, column = (wh >> 8) & 255, l = (wh >> 16) + 1;
	gtk_table_attach(GTK_TABLE(table), it, column, column + l, row, row + 1,
		pack == pk_TABLEx ? GTK_EXPAND | GTK_FILL : GTK_FILL, 0,
		pack == pk_TABLEp ? pad : 0, pad);
}

/* Find where unused rows/columns start */
static int next_table_level(GtkWidget *table, int h)
{
	GList *item;
	int y, n = 0;
	for (item = GTK_TABLE(table)->children; item; item = item->next)
	{
		y = h ? ((GtkTableChild *)item->data)->right_attach :
			((GtkTableChild *)item->data)->bottom_attach;
		if (n < y) n = y;
	}
	return (n);
}

/* Try to avoid scrolling - request full size of contents */
static void scroll_max_size_req(GtkWidget *widget, GtkRequisition *requisition,
	gpointer user_data)
{
	GtkWidget *child = GTK_BIN(widget)->child;

	if (child && GTK_WIDGET_VISIBLE(child))
	{
		GtkRequisition wreq;
		int n, border = GTK_CONTAINER(widget)->border_width * 2;

		gtk_widget_get_child_requisition(child, &wreq);
		n = wreq.width + border;
		if (requisition->width < n) requisition->width = n;
		n = wreq.height + border;
		if (requisition->height < n) requisition->height = n;
	}
}

/* Toggle notebook pages */
static void toggle_vbook(GtkToggleButton *button, gpointer user_data)
{
	gtk_notebook_set_page(**(void ***)user_data,
		!!gtk_toggle_button_get_active(button));
}

//	COLORLIST widget

typedef struct {
	unsigned char *col;
	int cnt, *idx;
	int scroll;
} colorlist_data;

// !!! ref to RGB[3]
static gboolean col_expose(GtkWidget *widget, GdkEventExpose *event,
	unsigned char *col)
{
	GdkGCValues sv;

	gdk_gc_get_values(widget->style->black_gc, &sv);
	gdk_rgb_gc_set_foreground(widget->style->black_gc, MEM_2_INT(col, 0));
	gdk_draw_rectangle(widget->window, widget->style->black_gc, TRUE,
		event->area.x, event->area.y, event->area.width, event->area.height);
	gdk_gc_set_foreground(widget->style->black_gc, &sv.foreground);

	return (TRUE);
}

static gboolean colorlist_click(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];
	colorlist_ext xdata;

	if (event->type == GDK_BUTTON_PRESS)
	{
		xdata.idx = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
		xdata.button = event->button;
		((evtx_fn)desc[1])(GET_DDATA(base), base,
			(int)desc[0] & WB_OPMASK, slot, &xdata);
	}

	/* Let click processing continue */
	return (FALSE);
}

static void colorlist_select(GtkList *list, GtkWidget *widget, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));

	/* Update the value */
	*dt->idx = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
	/* Call the handler */
	if (desc[1]) ((evt_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot);
}

static void colorlist_map_scroll(GtkWidget *list, colorlist_data *dt)
{	
	GtkAdjustment *adj;
	int idx = dt->scroll - 1;

	dt->scroll = 0;
	if (idx < 0) return;
	adj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(list->parent->parent));
	if (adj->upper > adj->page_size)
	{
		float f = adj->upper * (idx + 0.5) / dt->cnt - adj->page_size * 0.5;
		adj->value = f < 0.0 ? 0.0 : f > adj->upper - adj->page_size ?
			adj->upper - adj->page_size : f;
		gtk_adjustment_value_changed(adj);
	}
}

// !!! And with inlining this, problem also
GtkWidget *colorlist(GtkWidget *box, int *idx, char *ddata, void **pp,
	void **r)
{
	GtkWidget *scroll, *list, *item, *col, *label;
	colorlist_data *dt;
	void *v;
	char txt[64], *t, **sp = NULL;
	int i, cnt = 0;

	scroll = pack(box, gtk_scrolled_window_new(NULL, NULL));
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	list = gtk_list_new();

	// Allocate datablock
	dt = bound_malloc(list, sizeof(colorlist_data));
	v = ddata + (int)pp[3];
	if (((int)pp[0] & WB_OPMASK) == op_COLORLIST) // array of names
	{
		sp = *(char ***)v;
		while (sp[cnt]) cnt++;
	}
	else cnt = *(int *)v; // op_COLORLISTN - number
	dt->cnt = cnt;
	dt->col = (void *)(ddata + (int)pp[2]); // palette
	dt->idx = idx;

	gtk_object_set_user_data(GTK_OBJECT(list), dt); // know thy descriptor
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), list);
	gtk_widget_show_all(scroll);

	for (i = 0; i < cnt; i++)
	{
		item = gtk_list_item_new();
		gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)i);
		if (pp[5]) gtk_signal_connect(GTK_OBJECT(item), "button_press_event",
			GTK_SIGNAL_FUNC(colorlist_click), r);
		gtk_container_add(GTK_CONTAINER(list), item);

		box = gtk_hbox_new(FALSE, 3);
		gtk_widget_show(box);
		gtk_container_set_border_width(GTK_CONTAINER(box), 3);
		gtk_container_add(GTK_CONTAINER(item), box);

		col = pack(box, gtk_drawing_area_new());
		gtk_drawing_area_size(GTK_DRAWING_AREA(col), 20, 20);
		gtk_signal_connect(GTK_OBJECT(col), "expose_event",
			GTK_SIGNAL_FUNC(col_expose), dt->col + i * 3);

		/* Name or index */
		if (sp) t = _(sp[i]);
		else sprintf(t = txt, "%d", i);
		label = xpack(box, gtk_label_new(t));
		gtk_misc_set_alignment(GTK_MISC(label), 0.0, 1.0);

		gtk_widget_show_all(item);
	}
	gtk_list_set_selection_mode(GTK_LIST(list), GTK_SELECTION_BROWSE);
	/* gtk_list_select_*() don't work in GTK_SELECTION_BROWSE mode */
	gtk_container_set_focus_child(GTK_CONTAINER(list),
		GTK_WIDGET(g_list_nth(GTK_LIST(list)->children, *idx)->data));
	gtk_signal_connect(GTK_OBJECT(list), "select_child",
		GTK_SIGNAL_FUNC(colorlist_select), NEXT_SLOT(r));
	gtk_signal_connect_after(GTK_OBJECT(list), "map",
		GTK_SIGNAL_FUNC(colorlist_map_scroll), dt);

	return (list);
}

static void colorlist_scroll_in(GtkWidget *list, int idx)
{
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));
	dt->scroll = idx + 1;
	if (GTK_WIDGET_MAPPED(list)) colorlist_map_scroll(list, dt);
}

static void colorlist_set_color(GtkWidget *list, int idx, int v)
{
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));
	unsigned char *rgb = dt->col + idx * 3;
	GdkColor c;

	c.pixel = 0;
	c.red   = (rgb[0] = INT_2_R(v)) * 257;
	c.green = (rgb[1] = INT_2_G(v)) * 257;
	c.blue  = (rgb[2] = INT_2_B(v)) * 257;
	// In case of some really ancient system with indexed display mode
	gdk_colormap_alloc_color(gdk_colormap_get_system(), &c, FALSE, TRUE);
	/* Redraw the item displaying the color */
	gtk_widget_queue_draw(
		GTK_WIDGET(g_list_nth(GTK_LIST(list)->children, idx)->data));
}

//	COLORPAD widget

#define PPAD_SLOT 11
#define PPAD_XSZ 32
#define PPAD_YSZ 8
#define PPAD_WIDTH(X) (PPAD_XSZ * (X) - 1)
#define PPAD_HEIGHT(X) (PPAD_YSZ * (X) - 1)

static void colorpad_set(void **slot, int v)
{
	GtkWidget *widget = slot[0];
	void **desc = slot[1];

	wjpixmap_move_cursor(widget, (v % PPAD_XSZ) * PPAD_SLOT,
		(v / PPAD_XSZ) * PPAD_SLOT);
	*(int *)gtk_object_get_user_data(GTK_OBJECT(widget)) = v; // self-reading
	if (desc[4]) get_evt_1(NULL, NEXT_SLOT(slot)); // call handler
}

static gboolean colorpad_click(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data)
{
	int x = event->x, y = event->y;

	gtk_widget_grab_focus(widget);
	/* Only single clicks */
	if (event->type != GDK_BUTTON_PRESS) return (TRUE);
	x /= PPAD_SLOT; y /= PPAD_SLOT;
	colorpad_set(user_data, y * PPAD_XSZ + x);
	return (TRUE);
}

static gboolean colorpad_key(GtkWidget *widget, GdkEventKey *event,
	gpointer user_data)
{
	int x, y, dx, dy;

	if (!arrow_key(event, &dx, &dy, 0)) return (FALSE);
	wjpixmap_cursor(widget, &x, &y);
	x = x / PPAD_SLOT + dx; y = y / PPAD_SLOT + dy;
	y = y < 0 ? 0 : y >= PPAD_YSZ ? PPAD_YSZ - 1 : y;
	y = y * PPAD_XSZ + x;
	y = y < 0 ? 0 : y > 255 ? 255 : y;
	colorpad_set(user_data, y);
#if GTK_MAJOR_VERSION == 1
	/* Return value alone doesn't stop GTK1 from running other handlers */
	gtk_signal_emit_stop_by_name(GTK_OBJECT(widget), "key_press_event");
#endif
	return (TRUE);
}

static void colorpad_draw(GtkWidget *widget, gpointer user_data)
{
	unsigned char *rgb;
	int w, h, c;

	if (!wjpixmap_pixmap(widget)) return;
	w = PPAD_WIDTH(PPAD_SLOT);
	h = PPAD_HEIGHT(PPAD_SLOT);
	rgb = render_color_grid(w, h, PPAD_SLOT, user_data);
	if (!rgb) return;
	wjpixmap_draw_rgb(widget, 0, 0, w, h, rgb, w * 3);
	c = (PPAD_SLOT >> 1) - 1;
	wjpixmap_set_cursor(widget, xbm_ring4_bits, xbm_ring4_mask_bits,
		xbm_ring4_width, xbm_ring4_height,
		xbm_ring4_x_hot - c, xbm_ring4_y_hot - c, TRUE);
	free(rgb);
}

static GtkWidget *colorpad(int *idx, char *ddata, void **pp, void **r)
{
	GtkWidget *pix;

	pix = wjpixmap_new(PPAD_WIDTH(PPAD_SLOT), PPAD_HEIGHT(PPAD_SLOT));
	gtk_object_set_user_data(GTK_OBJECT(pix), (gpointer)idx);
	gtk_signal_connect(GTK_OBJECT(pix), "realize",
		GTK_SIGNAL_FUNC(colorpad_draw), (gpointer)(ddata + (int)pp[2]));
	r -= 2; // using the origin slot
	gtk_signal_connect(GTK_OBJECT(pix), "button_press_event",
		GTK_SIGNAL_FUNC(colorpad_click), (gpointer)r);
	gtk_signal_connect(GTK_OBJECT(pix), "key_press_event",
		GTK_SIGNAL_FUNC(colorpad_key), (gpointer)r);
	return (pix);
}

//	GRADBAR widget

#define GRADBAR_LEN 16
#define SLOT_SIZE 15

typedef struct {
	unsigned char *map, *rgb;
	GtkWidget *lr[2];
	void **r;
	int ofs, lim, *idx, *len;
} gradbar_data;

static void gradbar_scroll(GtkWidget *btn, gpointer user_data)
{
	gradbar_data *dt = gtk_object_get_user_data(GTK_OBJECT(btn->parent));
	int dir = (int)user_data * 2 - 1;

	dt->ofs += dir;
	*dt->idx += dir; // self-reading
	gtk_widget_set_sensitive(dt->lr[0], !!dt->ofs);
	gtk_widget_set_sensitive(dt->lr[1], dt->ofs < dt->lim - GRADBAR_LEN);
	gtk_widget_queue_draw(btn->parent);
	get_evt_1(NULL, (gpointer)dt->r);
}

static void gradbar_slot(GtkWidget *btn, gpointer user_data)
{
	gradbar_data *dt;

	if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn))) return;
	dt = gtk_object_get_user_data(GTK_OBJECT(btn->parent));
	*dt->idx = (int)user_data + dt->ofs; // self-reading
	get_evt_1(NULL, (gpointer)dt->r);
}

static gboolean gradbar_draw(GtkWidget *widget, GdkEventExpose *event,
	gpointer idx)
{
	unsigned char rgb[SLOT_SIZE * 2 * 3];
	gradbar_data *dt = gtk_object_get_user_data(
		GTK_OBJECT(widget->parent->parent));
	int i, n = (int)idx + dt->ofs;

	if (n < *dt->len) // Filled slot
	{
		memcpy(rgb, dt->rgb ? dt->rgb + dt->map[n] * 3 :
			dt->map + n * 3, 3);
		for (i = 3; i < SLOT_SIZE * 2 * 3; i++) rgb[i] = rgb[i - 3];
	}
	else // Empty slot - show that
	{
		memset(rgb, 178, sizeof(rgb));
		memset(rgb, 128, SLOT_SIZE * 3);
	}

	gdk_draw_rgb_image(widget->window, widget->style->black_gc,
		0, 0, SLOT_SIZE, SLOT_SIZE, GDK_RGB_DITHER_NONE,
		rgb + SLOT_SIZE * 3, -3);

	return (TRUE);
}

// !!! With inlining this, problem also
GtkWidget *gradbar(int *idx, char *ddata, void **pp, void **r)
{
	GtkWidget *hbox, *btn, *sw;
	gradbar_data *dt;
	int i;

	hbox = gtk_hbox_new(TRUE, 0);
	dt = bound_malloc(hbox, sizeof(gradbar_data));
	gtk_object_set_user_data(GTK_OBJECT(hbox), dt);

	dt->r = r;
	dt->idx = idx;
	dt->len = (void *)(ddata + (int)pp[3]); // length
	dt->map = (void *)(ddata + (int)pp[4]); // gradient map
	if (*(int *)(ddata + (int)pp[2])) // mode not-RGB
		dt->rgb = (void *)(ddata + (int)pp[5]); // colormap
	dt->lim = (int)pp[6];

	dt->lr[0] = btn = xpack(hbox, gtk_button_new());
	gtk_container_add(GTK_CONTAINER(btn), gtk_arrow_new(GTK_ARROW_LEFT,
#if GTK_MAJOR_VERSION == 1
        // !!! Arrow w/o shadow is invisible in plain GTK+1
		GTK_SHADOW_OUT));
#else /* #if GTK_MAJOR_VERSION == 2 */
		GTK_SHADOW_NONE));
#endif
	gtk_widget_set_sensitive(btn, FALSE);
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
		GTK_SIGNAL_FUNC(gradbar_scroll), (gpointer)0);
	btn = NULL;
	for (i = 0; i < GRADBAR_LEN; i++)
	{
		btn = xpack(hbox, gtk_radio_button_new_from_widget(
			GTK_RADIO_BUTTON_0(btn)));
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
		gtk_signal_connect(GTK_OBJECT(btn), "toggled",
			GTK_SIGNAL_FUNC(gradbar_slot), (gpointer)i);
		sw = gtk_drawing_area_new();
		gtk_container_add(GTK_CONTAINER(btn), sw);
		gtk_widget_set_usize(sw, SLOT_SIZE, SLOT_SIZE);
		gtk_signal_connect(GTK_OBJECT(sw), "expose_event",
			GTK_SIGNAL_FUNC(gradbar_draw), (gpointer)i);
	}
	dt->lr[1] = btn = xpack(hbox, gtk_button_new());
	gtk_container_add(GTK_CONTAINER(btn), gtk_arrow_new(GTK_ARROW_RIGHT,
#if GTK_MAJOR_VERSION == 1
        // !!! Arrow w/o shadow is invisible in plain GTK+1
		GTK_SHADOW_OUT));
#else /* #if GTK_MAJOR_VERSION == 2 */
		GTK_SHADOW_NONE));
#endif
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
		GTK_SIGNAL_FUNC(gradbar_scroll), (gpointer)1);

	gtk_widget_show_all(hbox);
	return (hbox);
}

//	TEXT widget

static GtkWidget *textarea(char *init)
{
	GtkWidget *scroll, *text;

#if GTK_MAJOR_VERSION == 1
	text = gtk_text_new(NULL, NULL);
	gtk_text_set_editable(GTK_TEXT(text), TRUE);
	if (init) gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, init, -1);

	scroll = gtk_scrolled_window_new(NULL, GTK_TEXT(text)->vadj);
#else /* #if GTK_MAJOR_VERSION == 2 */
	GtkTextBuffer *texbuf = gtk_text_buffer_new(NULL);
	if (init) gtk_text_buffer_set_text(texbuf, init, -1);

	text = gtk_text_view_new_with_buffer(texbuf);

	scroll = gtk_scrolled_window_new(GTK_TEXT_VIEW(text)->hadjustment,
		GTK_TEXT_VIEW(text)->vadjustment);
#endif
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), text);
	gtk_widget_show_all(scroll);

	return (text);
}

static char *read_textarea(GtkWidget *text)
{
#if GTK_MAJOR_VERSION == 1
	return (gtk_editable_get_chars(GTK_EDITABLE(text), 0, -1));
#else /* #if GTK_MAJOR_VERSION == 2 */
	GtkTextIter begin, end;
	GtkTextBuffer *buffer = GTK_TEXT_VIEW(text)->buffer;

	gtk_text_buffer_get_start_iter(buffer, &begin);
	gtk_text_buffer_get_end_iter(buffer, &end);
	return (gtk_text_buffer_get_text(buffer, &begin, &end, TRUE));
#endif
}

static void set_textarea(GtkWidget *text, char *init)
{
#if GTK_MAJOR_VERSION == 1
	gtk_editable_delete_text(GTK_EDITABLE(text), 0, -1);
	if (init) gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, init, -1);
#else /* #if GTK_MAJOR_VERSION == 2 */
	gtk_text_buffer_set_text(GTK_TEXT_VIEW(text)->buffer, init ? init : "", -1);
#endif
}

//	Columns for LIST* widgets

typedef struct {
	char *ddata;			// datastruct
	void **dcolumn;			// datablock "column" if any
	void **columns[MAX_COLS];	// column slots
	void **r;			// slot
	int ncol;			// columns
	unsigned char idx[MAX_COLS];	// index vector
} col_data;

static void *get_cell(col_data *c, int row, int col)
{
	void **cp = c->columns[col][1];
	char *v;
	int op, ofs = 0;

	if (!cp[2]) // relative
	{
		ofs = (int)cp[1];
		cp = c->dcolumn;
	}
	op = (int)cp[0];
	v = cp[1];
	if (op & WB_FFLAG) v = c->ddata + (int)v;
	if (op & WB_NFLAG) v = *(void **)v; // dereference

	return (v + ofs + row * (int)cp[2]);
}

static void set_columns(col_data *c, col_data *src, char *ddata, void **r)
{
	int i;

	/* Copy & clear the source */
	*c = *src;
	memset(src, 0, sizeof(*src));

	c->ddata = ddata;
	c->r = r;
	for (i = 0; i < MAX_COLS; i++) c->idx[i] = i;
	/* Link columns to list */
	for (i = 0; i < c->ncol; i++)
		c->columns[i][0] = c->idx + i;
}

//	LISTCC widget

typedef struct {
	int lock;		// against in-reset signals
	int wantfoc;		// delayed focus
	int h;			// max height
	int *idx;		// result field
	int *cnt;		// length field
	col_data c;
} listcc_data;

static void listcc_select(GtkList *list, GtkWidget *widget, gpointer user_data)
{
	listcc_data *dt = user_data;
	void **base, **desc, **slot = NEXT_SLOT(dt->c.r);

	if (dt->lock) return;
	/* Update the value */
	*dt->idx = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
	// !!! No touching slot outside lock: uninitialized the first time
	base = slot[0]; desc = slot[1];
	/* Call the handler */
	if (desc[1]) ((evt_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot);
}

static void listcc_select_item(GtkList *list, listcc_data *dt)
{
	GtkWidget *item;
	int idx = *dt->idx, n = dt->h - idx - 1; // backward

	if (!(item = g_list_nth_data(list->children, n))) return;
	/* Move focus if sensitive, flag to be moved later if not */
	if (!(dt->wantfoc = !GTK_WIDGET_SENSITIVE((GtkWidget *)list)))
	{
		dt->lock++; // this is strictly a visual update
		list_select_item((GtkWidget *)list, item);
		dt->lock--;
	}
	/* Signal must be reliable even with focus delayed */
	listcc_select(list, item, dt);
}

static void listcc_toggled(GtkWidget *widget, gpointer user_data)
{
	listcc_data *dt = user_data;
	void **slot, **base, **desc;
	char *v;
	int col, row;

	if (dt->lock) return;
	/* Find out what happened to what, and where */
	col = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
	row = (int)gtk_object_get_user_data(GTK_OBJECT(widget->parent->parent));

	/* Self-updating */
	v = get_cell(&dt->c, row, col);
	*(int *)v = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
	/* Now call the handler */
	slot = dt->c.columns[col];
	slot = NEXT_SLOT(slot);
	base = slot[0]; desc = slot[1];
	if (desc[1]) ((evtx_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot, (void *)row);
}

static void listcc_reset(GtkList *list, listcc_data *ld, int row)
{
	GList *col, *curr = list->children;
	int cnt = *ld->cnt;


	if (row >= 0) // specific row, not whole list
		curr = g_list_nth(curr, ld->h - row - 1); // backward
	ld->lock = TRUE;
	for (; curr; curr = curr->next)
	{
		GtkWidget *item = curr->data;
		int j, n = (int)gtk_object_get_user_data(GTK_OBJECT(item));

		if (n >= cnt)
		{
			gtk_widget_hide(item);
			continue;
		}
		
		col = GTK_BOX(GTK_BIN(item)->child)->children;
		for (j = 0; col; col = col->next , j++)
		{
			GtkWidget *widget = ((GtkBoxChild *)(col->data))->widget;
			char *v = get_cell(&ld->c, n, j);
			void **cp = ld->c.columns[j][1];
			int op = (int)cp[0] & WB_OPMASK;

			if ((op == op_TXTCOLUMN) || (op == op_XTXTCOLUMN))
				gtk_label_set_text(GTK_LABEL(widget), v);
			else if (op == op_CHKCOLUMNi0)
				gtk_toggle_button_set_active(
					GTK_TOGGLE_BUTTON(widget), *(int *)v);
		}
		gtk_widget_show(item);
		if (row >= 0) break; // one row only
	}

	if (row < 0) listcc_select_item(list, ld); // only when full reset
	ld->lock = FALSE;
}

// !!! With inlining this, problem also
GtkWidget *listcc(int *idx, char *ddata, void **pp, col_data *c, void **r)
{
	char txt[128];
	GtkWidget *widget, *list, *item, *hbox;
	listcc_data *ld;
	int j, n, h, kind, *cnt;


	cnt = (void *)(ddata + (int)pp[2]); // length pointer
	kind = (int)pp[0] & WB_OPMASK;
	h = kind == op_LISTCCHr ? (int)pp[3] : *cnt; // max for variable length

	list = gtk_list_new();
	gtk_list_set_selection_mode(GTK_LIST(list), GTK_SELECTION_BROWSE);

	/* Make datastruct */
	ld = bound_malloc(list, sizeof(listcc_data));
	gtk_object_set_user_data(GTK_OBJECT(list), ld);
	ld->idx = idx;
	ld->cnt = cnt;
	ld->h = h;
	set_columns(&ld->c, c, ddata, r);

	for (n = h - 1; n >= 0; n--) // Reverse numbering
	{
		item = gtk_list_item_new();
		gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)n);
		gtk_container_add(GTK_CONTAINER(list), item);
// !!! Spacing = 3
		hbox = gtk_hbox_new(FALSE, 3);
		gtk_container_add(GTK_CONTAINER(item), hbox);

		for (j = 0; j < ld->c.ncol; j++)
		{
			void **cp = ld->c.columns[j][1];
			int op = (int)cp[0] & WB_OPMASK, jw = (int)cp[3];

			if (op == op_CHKCOLUMNi0)
			{
#if GTK_MAJOR_VERSION == 1
			/* !!! Vertical spacing is too small without the label */
				widget = gtk_check_button_new_with_label("");
#else /* if GTK_MAJOR_VERSION == 2 */
			/* !!! Focus frame is placed wrong with the empty label */
				widget = gtk_check_button_new();
#endif
				gtk_object_set_user_data(GTK_OBJECT(widget),
					(gpointer)j);
				gtk_signal_connect(GTK_OBJECT(widget), "toggled",
					GTK_SIGNAL_FUNC(listcc_toggled), ld);
				/* !!! "i0" means 0th slot is insensitive */
				if (!n) gtk_widget_set_sensitive(widget, FALSE);
			}
			else
			{
				if ((op == op_TXTCOLUMN) || (op == op_XTXTCOLUMN))
					txt[0] = '\0'; // Init to empty string
				else /* if (op == op_IDXCOLUMN) */ // Constant
					sprintf(txt, "%d", (int)cp[1] + (int)cp[2] * n);
				widget = gtk_label_new(txt);
				gtk_misc_set_alignment(GTK_MISC(widget),
					(jw >> 16) * 0.5, 0.5);
			}
			if (jw & 0xFFFF)
				gtk_widget_set_usize(widget, jw & 0xFFFF, -2);
			(op == op_XTXTCOLUMN ? xpack : pack)(hbox, widget);
		}
		gtk_widget_show_all(hbox);
	}

	if (*cnt) listcc_reset(GTK_LIST(list), ld, -1);

	gtk_signal_connect(GTK_OBJECT(list), "select_child",
		GTK_SIGNAL_FUNC(listcc_select), ld);

	return (list);
}

//	LISTC widget

typedef struct {
	int kind;		// type of list
	int update;		// delayed update flags
	int lock;		// against in-reset signals
	int cntmax;		// maximum value from row map
	int *idx;		// result field
	int *cnt;		// length field
	int *sort;		// sort column & direction
	int **map;		// row map vector field
	void **change;		// slot for EVT_CHANGE
	void **ok;		// slot for EVT_OK
	GtkWidget *sort_arrows[MAX_COLS];
	GdkPixmap *icons[2];
	GdkBitmap *masks[2];
	col_data c;
} listc_data;

static gboolean listcx_key(GtkWidget *widget, GdkEventKey *event,
	gpointer user_data)
{
	listc_data *dt = user_data;
	GtkCList *clist = GTK_CLIST(widget);
	int row = 0;

	if (dt->lock) return (FALSE); // Paranoia
	switch (event->keyval)
	{
	case GDK_End: case GDK_KP_End:
		row = clist->rows - 1;
	case GDK_Home: case GDK_KP_Home:
		clist->focus_row = row;
		gtk_clist_select_row(clist, row, 0);
		gtk_clist_moveto(clist, row, 0, 0.5, 0);
		return (TRUE);
	case GDK_Return: case GDK_KP_Enter:
		if (!dt->ok) break;
		get_evt_1(NULL, dt->ok);
		return (TRUE);
	}
	return (FALSE);
}

static gboolean listcx_click(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data)
{
	listc_data *dt = user_data;
	GtkCList *clist = GTK_CLIST(widget);
	void **slot, **base, **desc;
	gint row, col;

	if (dt->lock) return (FALSE); // Paranoia
	if ((event->button != 3) || (event->type != GDK_BUTTON_PRESS))
		return (FALSE);
	if (!gtk_clist_get_selection_info(clist, event->x, event->y, &row, &col))
		return (FALSE);

	if (clist->focus_row != row)
	{
		clist->focus_row = row;
		gtk_clist_select_row(clist, row, 0);
	}

	slot = SLOT_N(dt->c.r, 2);
	base = slot[0]; desc = slot[1];
	if (desc[1]) ((evtx_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot, (void *)row);

	return (TRUE);
}

static void listcx_done(GtkCList *clist, listc_data *ld)
{
	int j;

	/* Free pixmaps */
	gdk_pixmap_unref(ld->icons[0]);
	gdk_pixmap_unref(ld->icons[1]);
	if (ld->masks[0]) gdk_pixmap_unref(ld->masks[0]);
	if (ld->masks[1]) gdk_pixmap_unref(ld->masks[1]);

	/* Remember column widths */
	for (j = 0; j < ld->c.ncol; j++)
	{
		void **cp = ld->c.columns[j][1];
		int op = (int)cp[0];
		int l = WB_GETLEN(op); // !!! -2 for each extra ref

		if (l > 4) inifile_set_gint32(cp[5], clist->column[j].width);
	}
}

static void listc_select_row(GtkCList *clist, gint row, gint column,
	GdkEventButton *event, gpointer user_data)
{
	listc_data *dt = user_data;
	void **slot = NEXT_SLOT(dt->c.r), **base = slot[0], **desc = slot[1];
	int dclick;

	if (dt->lock) return;
	dclick = dt->ok && event && (event->type == GDK_2BUTTON_PRESS);
	/* Update the value */
	*dt->idx = (int)gtk_clist_get_row_data(clist, row);
	/* Call the handler */
	if (desc[1]) ((evt_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot);
	/* Call the other handler */
	if (dclick) get_evt_1(NULL, dt->ok);
}

static void listc_update(GtkWidget *w, gpointer user_data)
{
	GtkCList *clist = GTK_CLIST(w);
	listc_data *ld = user_data;
	int what = ld->update;


	if (!GTK_WIDGET_MAPPED(w)) /* Is frozen anyway */
	{
		// Flag a waiting refresh
		if (what & 1) ld->update = (what & 2) | 4;
		return;
	}

	ld->update = 0;
	if (what & 4) /* Do a delayed refresh */
	{
		gtk_clist_freeze(clist);
		gtk_clist_thaw(clist);
	}
	if ((what & 2) && clist->selection) /* Do a scroll */
		gtk_clist_moveto(clist, (int)(clist->selection->data), 0, 0.5, 0);
}

static int listc_collect(gchar **row_text, gchar **row_pix, col_data *c, int row)
{
	int j, res = FALSE, ncol = c->ncol;

	if (row_pix) memset(row_pix, 0, ncol * sizeof(*row_pix));
	for (j = 0; j < ncol; j++)
	{
		char *v = get_cell(c, row, j);
		void **cp = c->columns[j][1];
		int op = (int)cp[0] & WB_OPMASK;

// !!! IDXCOLUMN not supported
		if ((op == op_RTXTCOLUMN) || (op == op_RFILECOLUMN))
			v += *(int *)v;
		if (op == op_RFILECOLUMN)
		{
			if (!row_pix || (v[0] == ' ')) v++;
			else row_pix[j] = v , v = "" , res = TRUE;
		}
		row_text[j] = v;
	}
	return (res);
}

static int listc_sort(GtkCList *clist, listc_data *ld, int reselect)
{
	void **slot;

	/* Do nothing if empty */
	if (!*ld->cnt) return (0);

	/* Call & apply external sort */
	if ((slot = ld->change))
	{
		GList *tmp, *cur, **ll, *pos = NULL;
		int i, cnt, *map;

		/* Call the EVT_CHANGE handler, to sort map vector */
		get_evt_1(NULL, slot);

		/* !!! Directly rearrange widget's internals; it's deprecated
		 * so nothing inside will change anymore */
		gtk_clist_freeze(clist);

		if (clist->selection) pos = g_list_nth(clist->row_list,
			GPOINTER_TO_INT(clist->selection->data));

		ll = calloc(ld->cntmax + 1, sizeof(*ll));
		for (cur = clist->row_list; cur; cur = cur->next)
		{
			GtkCListRow *row = cur->data;
			ll[(int)row->data] = cur;
		}

		/* Rearrange rows */
		cnt = *ld->cnt;
		map = *ld->map;
		clist->row_list = cur = ll[map[0]];
		cur->prev = NULL;
		for (i = 1; i < cnt; i++)
		{
			cur->next = tmp = ll[map[i]];
			tmp->prev = cur;
			cur = tmp;
		}
		clist->row_list_end = cur;
		cur->next = NULL;

		free(ll);

		if (pos) /* Relocate existing selection */
		{
			int n = g_list_position(clist->row_list, pos);
			clist->selection->data = GINT_TO_POINTER(n);
			clist->focus_row = n;
		}

	}
	/* Do builtin sort */	
	else gtk_clist_sort(clist);

	if (!reselect || !clist->selection)
	{
		int n = gtk_clist_find_row_from_data(clist, (gpointer)*ld->idx);
		if (n < 0) *ld->idx = (int)gtk_clist_get_row_data(clist, n = 0);
		clist->focus_row = n;
		gtk_clist_select_row(clist, n, 0);
	}

	/* Done rearranging rows */
	if (slot)
	{
		gtk_clist_thaw(clist);
		return (1); // Refresh later if invisible now
	}

	/* !!! Builtin sort doesn't move focus along with selection, do it here */
	if (clist->selection)
	{
		int n = GPOINTER_TO_INT(clist->selection->data);
		if (clist->focus_row != n)
		{
			clist->focus_row = n;
			if (GTK_WIDGET_HAS_FOCUS((GtkWidget *)clist))
				return (4); // Refresh
		}
	}
	return (0);
}

/* !!! Should not redraw old things while resetting - or at least, not refer
 * outside of new data if doing it */
static void listc_reset(GtkCList *clist, listc_data *ld)
{
	int i, j, m, n, ncol = ld->c.ncol, cnt = *ld->cnt, *map = NULL;

	ld->lock = TRUE;
	gtk_clist_freeze(clist);
	gtk_clist_clear(clist);

	if (ld->map) map = *ld->map;
	for (m = i = 0; i < cnt; i++)
	{
		gchar *row_text[MAX_COLS], *row_pix[MAX_COLS];
		int row, pix;

		n = map ? map[i] : i;
		if (m < n) m = n;
		pix = listc_collect(row_text, row_pix, &ld->c, n);
		row = gtk_clist_append(clist, row_text);
		gtk_clist_set_row_data(clist, row, (gpointer)n);
		if (!pix) continue;
		for (j = 0; j < ncol; j++)
		{
			char *s = row_pix[j];
			if (!s) continue;
			pix = s[0] == 'D';
// !!! Spacing = 4
			gtk_clist_set_pixtext(clist, row, j, s + 1, 4,
				ld->icons[pix], ld->masks[pix]);
		}
	}
	ld->cntmax = m;

	/* Adjust column widths (not for draggable list) */
	if ((ld->kind != op_LISTCd) && (ld->kind != op_LISTCX))
	{
		for (j = 0; j < ncol; j++)
		{
// !!! Spacing = 5
			gtk_clist_set_column_width(clist, j,
				5 + gtk_clist_optimal_column_width(clist, j));
		}
	}

	i = *ld->idx;
	if (i >= cnt) i = cnt - 1;
	if (!cnt) *ld->idx = 0;	/* Safer than -1 for empty list */
	/* Draggable and unordered lists aren't sorted */
	else if ((ld->kind == op_LISTCd) || (ld->kind == op_LISTCu))
	{
		if (i < 0) i = 0;
		gtk_clist_select_row(clist, i, 0);
		*ld->idx = i;
	}
	else
	{
		*ld->idx = i;
		listc_sort(clist, ld, FALSE);
	}

	gtk_clist_thaw(clist);

	/* !!! The below was added into fpick.c in 3.34.72 to fix some, left
	 * undescribed, incomplete redraws on Windows; disabled for now,
	 * in hope the glitch won't reappear - WJ */
//	gtk_widget_queue_draw((GtkWidget *)clist);

	ld->update |= 3;
	listc_update((GtkWidget *)clist, ld);
	ld->lock = FALSE;
}

static void listc_column_button(GtkCList *clist, gint col, gpointer user_data)
{
	listc_data *dt = user_data;
	int sort = *dt->sort;

	if (col < 0) col = abs(sort) - 1; /* Sort as is */
	else if (abs(sort) == col + 1) sort = -sort; /* Reverse same column */
	else /* Select another column */
	{
		gtk_widget_hide(dt->sort_arrows[abs(sort) - 1]);
		gtk_widget_show(dt->sort_arrows[col]);
		sort = col + 1;
	}
	*dt->sort = sort;

	gtk_clist_set_sort_column(clist, col);
	gtk_clist_set_sort_type(clist, sort > 0 ? GTK_SORT_ASCENDING :
		GTK_SORT_DESCENDING);
	gtk_arrow_set(GTK_ARROW(dt->sort_arrows[col]), sort > 0 ?
		GTK_ARROW_DOWN : GTK_ARROW_UP, GTK_SHADOW_IN);

	/* Sort and maybe redraw */
	dt->update |= listc_sort(clist, dt, TRUE);
	/* Scroll to selected row */
	dt->update |= 2;
	listc_update((GtkWidget *)clist, dt);
}

static void listc_prepare(GtkWidget *w, gpointer user_data)
{
	listc_data *ld = user_data;
	GtkCList *clist = GTK_CLIST(w);
	int j;

	if (ld->kind == op_LISTCX)
	{
		/* Ensure enough space for pixmaps */
		gtk_clist_set_row_height(clist, 0);
		if (clist->row_height < 16) gtk_clist_set_row_height(clist, 16);
	}

	/* Adjust width for columns which use sample text */
	for (j = 0; j < ld->c.ncol; j++)
	{
		void **cp = ld->c.columns[j][1];
		int op = (int)cp[0];
		int l = WB_GETLEN(op); // !!! -2 for each extra ref

		if (l < 6) continue;
		l = gdk_string_width(
#if GTK_MAJOR_VERSION == 1
			w->style->font,
#else /* if GTK_MAJOR_VERSION == 2 */
			gtk_style_get_font(w->style),
#endif
			cp[6]);
		if (clist->column[j].width < l)
			gtk_clist_set_column_width(clist, j, l);
	}

	/* To avoid repeating after unrealize (likely won't happen anyway) */
//	gtk_signal_disconnect_by_func(GTK_OBJECT(w),
//		GTK_SIGNAL_FUNC(listc_prepare), user_data);
}

// !!! With inlining this, problem also likely
GtkWidget *listc(int *idx, char *ddata, void **pp, col_data *c, void **r)
{
	static int zero = 0;
	GtkWidget *list, *hbox;
	GtkCList *clist;
	listc_data *ld;
	int j, w, sm, kind, *cntv, *sort = &zero, **map = NULL;


	cntv = (void *)(ddata + (int)pp[2]); // length var
	kind = (int)pp[0] & WB_OPMASK; // kind of list
	if ((kind == op_LISTCS) || (kind == op_LISTCX))
	{
		sort = (void *)(ddata + (int)pp[3]); // sort mode
		if (kind == op_LISTCX)
			map = (void *)(ddata + (int)pp[4]); // row map
	}

	list = gtk_clist_new(c->ncol);

	/* Make datastruct */
	ld = bound_malloc(list, sizeof(listc_data));
	gtk_object_set_user_data(GTK_OBJECT(list), ld);
	ld->kind = kind;
	ld->idx = idx;
	ld->cnt = cntv;
	ld->sort = sort;
	ld->map = map;
	set_columns(&ld->c, c, ddata, r);

	sm = *sort;
	clist = GTK_CLIST(list);
	for (j = 0; j < ld->c.ncol; j++)
	{
		void **cp = ld->c.columns[j][1];
		int op = (int)cp[0], jw = (int)cp[3];
		int l = WB_GETLEN(op); // !!! -2 for each extra ref

		gtk_clist_set_column_resizeable(clist, j, kind == op_LISTCX);
		if ((w = jw & 0xFFFF))
		{
			if (l > 4) w = inifile_get_gint32(cp[5], w);
			gtk_clist_set_column_width(clist, j, w);
		}
		/* Left justification is default */
		jw >>= 16;
		if (jw) gtk_clist_set_column_justification(clist, j,
			jw == 1 ? GTK_JUSTIFY_CENTER : GTK_JUSTIFY_RIGHT);

		hbox = gtk_hbox_new(FALSE, 0);
		(!jw ? pack : jw == 1 ? xpack : pack_end)(hbox,
			gtk_label_new(l > 3 ? _(cp[4]) : ""));
		gtk_widget_show_all(hbox);
		// !!! Must be before gtk_clist_column_title_passive()
		gtk_clist_set_column_widget(clist, j, hbox);

		if (sm) ld->sort_arrows[j] = pack_end(hbox,
			gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_IN));
		else gtk_clist_column_title_passive(clist, j);
		if (kind == op_LISTCX) GTK_WIDGET_UNSET_FLAGS(
			GTK_CLIST(clist)->column[j].button, GTK_CAN_FOCUS);
	}

	if (sm)
	{
		int c = abs(sm) - 1;

		gtk_widget_show(ld->sort_arrows[c]);	// Show sort arrow
		gtk_clist_set_sort_column(clist, c);
		gtk_clist_set_sort_type(clist, sm > 0 ? GTK_SORT_ASCENDING :
			GTK_SORT_DESCENDING);
		gtk_arrow_set(GTK_ARROW(ld->sort_arrows[c]), sm > 0 ?
			GTK_ARROW_DOWN : GTK_ARROW_UP, GTK_SHADOW_IN);
		gtk_signal_connect(GTK_OBJECT(clist), "click_column",
			GTK_SIGNAL_FUNC(listc_column_button), ld);
	}

	if (kind != op_LISTCu) gtk_clist_column_titles_show(clist);
	gtk_clist_set_selection_mode(clist, GTK_SELECTION_BROWSE);

	if (kind == op_LISTCX)
	{
#ifdef GTK_STOCK_DIRECTORY
		ld->icons[1] = render_stock_pixmap(mainwindow,
			GTK_STOCK_DIRECTORY, &ld->masks[1]);
#endif
#ifdef GTK_STOCK_FILE
		ld->icons[0] = render_stock_pixmap(mainwindow,
			GTK_STOCK_FILE, &ld->masks[0]);
#endif
		if (!ld->icons[1]) ld->icons[1] = gdk_pixmap_create_from_xpm_d(
			mainwindow->window, &ld->masks[1], NULL, xpm_open_xpm);
		if (!ld->icons[0]) ld->icons[0] = gdk_pixmap_create_from_xpm_d(
			mainwindow->window, &ld->masks[0], NULL, xpm_new_xpm);

		gtk_signal_connect(GTK_OBJECT(clist), "key_press_event",
			GTK_SIGNAL_FUNC(listcx_key), ld);
		gtk_signal_connect(GTK_OBJECT(clist), "button_press_event",
			GTK_SIGNAL_FUNC(listcx_click), ld);
	}

	/* For some finishing touches */
	gtk_signal_connect_after(GTK_OBJECT(clist), "realize",
		GTK_SIGNAL_FUNC(listc_prepare), ld);

	/* This will apply delayed updates when they can take effect */
	gtk_signal_connect_after(GTK_OBJECT(clist), "map",
		GTK_SIGNAL_FUNC(listc_update), ld);

	if (*cntv) listc_reset(clist, ld);

	gtk_signal_connect(GTK_OBJECT(clist), "select_row",
		GTK_SIGNAL_FUNC(listc_select_row), ld);

	if (kind == op_LISTCd) clist_enable_drag(list); // draggable rows

	return (list);
}

//	PATH widget

static void pathbox_button(GtkWidget *widget, gpointer user_data)
{
	void **slot = user_data, **desc = slot[1];
	void *xdata[2] = { _(desc[2]), slot }; // title and slot

	file_selector_x((int)desc[3], xdata);
}

// !!! With inlining this, problem also
GtkWidget *pathbox(void **r)
{
	GtkWidget *hbox, *entry, *button;

	hbox = gtk_hbox_new(FALSE, 0);
	gtk_widget_show(hbox);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 5);

	entry = xpack5(hbox, gtk_entry_new());
	button = add_a_button(_("Browse"), 2, hbox, FALSE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(pathbox_button), r);

	return (entry);
}

/* Set value of path widget */
static void set_path(GtkWidget *widget, char *s, int mode)
{
	char path[PATHTXT];
	if (mode == PATH_VALUE) s = gtkuncpy(path, s, PATHTXT);
	gtk_entry_set_text(GTK_ENTRY(widget), s);
}

//	SPINPACK widget

static void spinpack_evt(GtkAdjustment *adj, gpointer user_data)
{
	void **tp = user_data, **vp = *tp, **r = *vp;
	void **slot, **base, **desc;
	char *ddata;
	int *v, idx = (tp - vp) / 2 - 1;

	if (!r) return; // Lock
	/* Locate the data */
	slot = NEXT_SLOT(r);
	base = slot[0];
	ddata = GET_DDATA(base);
	/* Locate the cell in array */
	desc = r[1];
	v = desc[1];
	if ((int)desc[0] & WB_FFLAG) v = (void *)(ddata + (int)v);
	v += idx * 3;
	/* Read the value */
	*v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(*(tp - 1)));
	/* Call the handler */
	desc = slot[1];
	if (desc[1]) ((evtx_fn)desc[1])(ddata, base, (int)desc[0] & WB_OPMASK,
		slot, (void *)idx);
}

// !!! With inlining this, problem also
GtkWidget *tlspinpack(int *np, void **pp, void **r, GtkWidget *table)
{
	GtkWidget *widget = NULL;
	void **vp, **tp;
	int wh, row, column, i, l, n;

	n = (int)pp[2];
	wh = (int)pp[3];
	row = wh & 255;
	column = (wh >> 8) & 255;
	l = (wh >> 16) + 1;

	vp = tp = calloc(n * 2 + 1, sizeof(void **));
	*tp++ = r;

	for (i = 0; i < n; i++ , np += 3)
	{
		int x = i % l, y = i / l;
// !!! Spacing = 2
		*tp++ = widget = spin_to_table(table, row + y, column + x, 2,
			np[0], np[1], np[2]);
		/* Value might get clamped, and slot is self-reading so should
		 * reflect that */
		np[0] = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
		spin_connect(widget, GTK_SIGNAL_FUNC(spinpack_evt), tp);
		*tp++ = vp;
	}
	gtk_object_set_user_data(GTK_OBJECT(widget), vp);
	gtk_signal_connect_object(GTK_OBJECT(widget), "destroy",
		GTK_SIGNAL_FUNC(free), (gpointer)vp);
	return (widget);
}

//	TLTEXT widget

// !!! Even with inlining this, some space gets wasted
void tltext(char *v, void **pp, GtkWidget *table, int pad)
{
	char *tmp, *s;
	int x, wh, row, column;

	tmp = s = strdup(v);

	wh = (int)pp[2];
	row = wh & 255;
	x = column = (wh >> 8) & 255;

	while (TRUE)
	{
		int i = strcspn(tmp, "\t\n");
		int c = tmp[i];
		tmp[i] = '\0';
		add_to_table(tmp, table, row, x++, pad);
		if (!c) break;
		if (c == '\n') x = column , row++;
		tmp += i + 1;
	}
	free(s);
}

#if U_NLS

/* Translate array of strings */
static int n_trans(char **dest, char **src, int n)
{
	int i;
	for (i = 0; (i != n) && src[i]; i++)
		dest[i] = src[i][0] ? _(src[i]) : "";
	return (i);
}

#endif

/* Get/set window position & size from/to inifile */
void rw_pos(v_dd *vdata, int set)
{
	char name[128];
	int i, l = strlen(vdata->ininame);

	memcpy(name, vdata->ininame, l);
	name[l++] = '_'; name[l + 1] = '\0';
	for (i = 0; i < 4; i++)
	{
		name[l] = "xywh"[i];
		if (set) inifile_set_gint32(name, vdata->xywh[i]);
		else if (vdata->xywh[i] || (i < 2)) /* 0 means auto-size */
			vdata->xywh[i] = inifile_get_gint32(name, vdata->xywh[i]);
	}
}

/* Groups of codes differing only in details of packing */

typedef struct {
	unsigned short op, cmd, pk;	// orig code, group code, packing mode
	signed char tpad, cw;		// padding and border
} cmdef;

typedef struct {
	cmdef s;
	void *x;	// additional parameter
} xcmdef;

enum {
	cm_TABLE = op_LAST,
	cm_VBOX,
	cm_HBOX,
	cm_SCROLL,
	cm_SPIN,
	cm_SPINa,
	cm_FSPIN,
	cm_CHECK,
	cm_RPACK,
	cm_RPACKD,
	cm_OPT,
	cm_OPTD,
	cm_PENTRY,
	cm_OKBOX,
	cm_UOKBOX,
	cm_CANCELBTN,
	cm_BUTTON,
	cm_TOGGLE,

	cm_XDEFS_0,
	cm_LABEL = cm_XDEFS_0,
	cm_SPINSLIDE,
	cm_SPINSLIDEa,
};

#define USE_BORDER(T) (op_BOR_0 - op_BOR_##T - 1)

static cmdef cmddefs[] = {
// !!! Padding = 0
	{ op_TABLE, cm_TABLE, pk_PACK | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(TABLE) },
	{ op_ETABLE, cm_TABLE, pk_PACKEND | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(TABLE) },
	{ op_FTABLE, cm_TABLE, pk_PACK | pkf_STACK | pkf_SHOW | pkf_FRAME,
		0, USE_BORDER(FRBOX) },
// !!! Padding = 0 Border = 0
	{ op_XTABLE, cm_TABLE, pk_XPACK | pkf_STACK | pkf_SHOW },
	{ op_TLHBOX, cm_HBOX, pk_TABLE | pkf_STACK | pkf_SHOW },
	{ op_HBOX, cm_HBOX, pk_PACKp | pkf_STACK | pkf_SHOW },
	{ op_XHBOX, cm_HBOX, pk_XPACK | pkf_STACK | pkf_SHOW },
	{ op_VBOX, cm_VBOX, pk_PACKp | pkf_STACK | pkf_SHOW },
	{ op_XVBOX, cm_VBOX, pk_XPACK | pkf_STACK | pkf_SHOW },
	{ op_EVBOX, cm_VBOX, pk_PACKEND | pkf_STACK | pkf_SHOW },
// !!! Padding = 0
	{ op_FVBOX, cm_VBOX, pk_PACK | pkf_FRAME | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(FRBOX) },
	{ op_FXVBOX, cm_VBOX, pk_XPACK | pkf_FRAME | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(FRBOX) },
	{ op_FHBOX, cm_HBOX, pk_PACK | pkf_FRAME | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(FRBOX) },
// !!! Padding = 0
	{ op_SCROLL, cm_SCROLL, pk_PACK | pkf_STACK | pkf_SHOW },
	{ op_XSCROLL, cm_SCROLL, pk_XPACK | pkf_STACK | pkf_SHOW,
		USE_BORDER(XSCROLL) },
	{ op_SPIN, cm_SPIN, pk_PACKp, USE_BORDER(SPIN) },
	/* Codes can map to themselves, and some do */
// !!! Padding = 0
	{ op_SPINc, op_SPINc, pk_PACK },
	{ op_XSPIN, cm_SPIN, pk_XPACK },
	{ op_TSPIN, cm_SPIN, pk_TABLE2, USE_BORDER(SPIN) },
	{ op_TLSPIN, cm_SPIN, pk_TABLE, USE_BORDER(SPIN) },
	{ op_TLXSPIN, cm_SPIN, pk_TABLEx, USE_BORDER(SPIN) },
	{ op_SPINa, cm_SPINa, pk_PACKp, USE_BORDER(SPIN) },
	{ op_FSPIN, cm_FSPIN, pk_PACKp, USE_BORDER(SPIN) },
	{ op_TFSPIN, cm_FSPIN, pk_TABLE2, USE_BORDER(SPIN) },
	{ op_TLFSPIN, cm_FSPIN, pk_TABLE, USE_BORDER(SPIN) },
// !!! Padding = 0
	{ op_XSPINa, cm_SPINa, pk_XPACK },
	{ op_TSPINa, cm_SPINa, pk_TABLE2, USE_BORDER(SPIN) },
// !!! Padding = 0 Border = 5
	{ op_CHECK, cm_CHECK, pk_PACK | pkf_SHOW, 0, 5 },
	{ op_XCHECK, cm_CHECK, pk_XPACK | pkf_SHOW, 0, 5 },
	{ op_TLCHECK, cm_CHECK, pk_TABLE | pkf_SHOW, 0, 5 },
// !!! Padding = 0 Border = 0
	{ op_TLCHECKs, cm_CHECK, pk_TABLE | pkf_SHOW, 0, 0 },
	{ op_RPACK, cm_RPACK, pk_XPACK },
	{ op_RPACKD, cm_RPACKD, pk_XPACK },
	{ op_FRPACK, cm_RPACK, pk_PACK | pkf_FRAME, 0, USE_BORDER(FRBOX) },
	{ op_OPT, cm_OPT, pk_PACKp, USE_BORDER(OPT) },
	{ op_OPTD, cm_OPTD, pk_PACKp, USE_BORDER(OPT) },
	{ op_XOPT, cm_OPT, pk_XPACK, 0, USE_BORDER(XOPT) },
	{ op_TOPT, cm_OPT, pk_TABLE2, USE_BORDER(OPT) },
	{ op_TLOPT, cm_OPT, pk_TABLE, USE_BORDER(OPT) },
// !!! Padding = 0
	{ op_COMBO, op_COMBO, pk_PACK | pkf_SHOW },
// !!! Padding = 5
	{ op_XENTRY, op_XENTRY, pk_XPACK | pkf_SHOW, 5 },
// !!! Padding = 0 Border = 0
	{ op_MLENTRY, op_MLENTRY, pk_PACK | pkf_SHOW },
	{ op_TLENTRY, op_TLENTRY, pk_TABLE | pkf_SHOW },
// !!! Padding = 5
	{ op_XPENTRY, cm_PENTRY, pk_XPACK | pkf_SHOW, 5 },
// !!! Padding = 0
	{ op_TPENTRY, cm_PENTRY, pk_TABLE2 | pkf_SHOW },
	{ op_OKBOX, cm_OKBOX, pk_PACK | pkf_STACK | pkf_SHOW, 0,
		USE_BORDER(OKBOX) },
// !!! Padding = 5 Border = 0
	{ op_OKBOXp, cm_OKBOX, pk_PACKp | pkf_STACK | pkf_SHOW, 5, 0 },
	{ op_EOKBOX, op_EOKBOX, pk_PACK | pkf_STACK | pkf_SHOW | pkf_PARENT, 0,
		USE_BORDER(OKBOX) },
	{ op_UOKBOX, cm_UOKBOX, pk_PACK | pkf_STACK | pkf_SHOW, 0,
		USE_BORDER(OKBOX) },
// !!! Padding = 5 Border = 0
	{ op_UOKBOXp, cm_UOKBOX, pk_PACKp | pkf_STACK | pkf_SHOW, 5, 0 },
	{ op_OKBTN, op_OKBTN, pk_XPACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_CANCELBTN, cm_CANCELBTN, pk_XPACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_UCANCELBTN, cm_CANCELBTN, pk_PACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
// !!! Padding = 5 Border = 0
	{ op_ECANCELBTN, cm_CANCELBTN, pk_PACKEND | pkf_SHOW, 5 },
	{ op_OKADD, op_OKADD, pk_XPACK1 | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_BUTTON, cm_BUTTON, pk_XPACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_UBUTTON, cm_BUTTON, pk_PACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
// !!! Padding = 5 Border = 0
	{ op_EBUTTON, cm_BUTTON, pk_PACKEND | pkf_SHOW, 5 },
	{ op_TLBUTTON, cm_BUTTON, pk_TABLEp | pkf_SHOW, 5 },
	{ op_OKTOGGLE, cm_TOGGLE, pk_XPACK1 | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_UTOGGLE, cm_TOGGLE, pk_PACK | pkf_SHOW, 0, USE_BORDER(BUTTON) }
};

static xcmdef xcmddefs[] = {
	/* Labels have 2 border values and X alignment, not a regular border */
	{ { op_MLABEL, cm_LABEL, pk_PACKp | pkf_SHOW, USE_BORDER(LABEL) },
		WBppa(0, 0, 5) },
// !!! Padding = 8
	{ { op_WLABEL, op_WLABEL, pk_EPACK | pkf_SHOW, 8 }, WBppa(0, 0, 5) },
// !!! Padding = 0
	{ { op_XLABEL, cm_LABEL, pk_XPACK | pkf_SHOW }, WBppa(0, 0, 5) },
	{ { op_TLLABEL, cm_LABEL, pk_TABLEp | pkf_SHOW, USE_BORDER(TLABEL) },
		WBppa(0, 0, 0) },
	/* Spinsliders use border field for preset width & height of slider */
	{ { op_TSPINSLIDE, cm_SPINSLIDE, pk_TABLE2x, USE_BORDER(SPINSLIDE) },
		WBwh(255, 20) },
	{ { op_TLSPINSLIDE, cm_SPINSLIDE, pk_TABLE, USE_BORDER(SPINSLIDE) } },
	{ { op_TLSPINSLIDEs, cm_SPINSLIDE, pk_TABLE, USE_BORDER(SPINSLIDE) },
		WBwh(150, 0) },
	{ { op_TLSPINSLIDEx, cm_SPINSLIDE, pk_TABLEx, USE_BORDER(SPINSLIDE) } },
	{ { op_SPINSLIDEa, cm_SPINSLIDEa, pk_PACKp, USE_BORDER(SPINSLIDE) } },
// !!! Padding = 0
	{ { op_XSPINSLIDEa, cm_SPINSLIDEa, pk_XPACK } },
	{ { op_HTSPINSLIDE, op_HTSPINSLIDE, 0 } }
};

static void do_destroy(void **wdata);

/* V-code is really simple-minded; it can do 0-tests but no arithmetics, and
 * naturally, can inline only constants. Everything else must be prepared either
 * in global variables, or in fields of "ddata" structure.
 * Parameters of codes should be arrayed in fixed order:
 * result location first; frame name last; table location, or name in table,
 * directly before it (last if no frame name); builtin event(s) before that */

#define DEF_BORDER 5
#define GET_BORDER(T) (borders[op_BOR_##T - op_BOR_0] + DEF_BORDER)

void **run_create(void **ifcode, void *ddata, int ddsize)
{
	static const int scrollp[3] = { GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC,
		GTK_POLICY_ALWAYS };
	cmdef *cmds[op_LAST];
	char *ident = VCODE_KEY;
#if U_NLS
	char *tc[256];
#endif
#if GTK_MAJOR_VERSION == 1
	/* GTK+1 typecasts dislike NULLs */
	GtkWindow *tparent = (GtkWindow *)mainwindow;
	int have_sliders = FALSE;
#else /* #if GTK_MAJOR_VERSION == 2 */
	GtkWindow *tparent = GTK_WINDOW(mainwindow);
#endif
	int part = FALSE, raise = FALSE;
	int borders[op_BOR_LAST - op_BOR_0], wpos = GTK_WIN_POS_CENTER;
	GtkWidget *wstack[CONT_DEPTH], **wp = wstack + CONT_DEPTH;
	GtkWidget *window = NULL, *widget = NULL;
	GtkAccelGroup* ag = NULL;
	v_dd *vdata;
	cmdef *cmd;
	col_data c;
	void *rstack[CALL_DEPTH], **rp = rstack;
	void *v, **pp, **r = NULL, **res = NULL;
	int ld, dsize;
	int i, n, op, lp, ref, pk, cw, tpad, minw = 0;


	// Allocation size
	ld = (ddsize + sizeof(void *) - 1) / sizeof(void *);
	n = (sizeof(v_dd) + sizeof(void *) - 1) / sizeof(void *);
	dsize = (ld + n + 2 + predict_size(ifcode, ddata) * 2) * sizeof(void *);
	if (!(res = calloc(1, dsize))) return (NULL); // failed
	memcpy(res, ddata, ddsize); // Copy datastruct
	ddata = res; // And switch to using it
	vdata = (void *)(res += ld); // Locate where internal data go
	r = res += n; // Anchor after it
	*r++ = ddata; // Store struct ref at anchor
	vdata->code = WDONE; // Make internal datastruct a noop
	*r++ = vdata; // And use it as tag for anchor

	// Border sizes are DEF_BORDER-based
	memset(borders, 0, sizeof(borders));

	// Commands index
	memset(cmds, 0, sizeof(cmds));
	for (i = 0; i < sizeof(cmddefs) / sizeof(cmddefs[0]); i++)
		cmds[cmddefs[i].op] = cmddefs + i;
	for (i = 0; i < sizeof(xcmddefs) / sizeof(xcmddefs[0]); i++)
		cmds[xcmddefs[i].s.op] = &xcmddefs[i].s;

	// Column data
	memset(&c, 0, sizeof(c));

	while (TRUE)
	{
		op = (int)*ifcode++;
		ifcode = (pp = ifcode) + (lp = WB_GETLEN(op));
		ref = WB_GETREF(op);
		pk = tpad = cw = 0;
		v = lp ? pp[0] : NULL;
		if (op & WB_FFLAG) v = (void *)((char *)ddata + (int)v);
		if (op & WB_NFLAG) v = *(char **)v; // dereference a string
		op &= WB_OPMASK;
		if ((cmd = cmds[op]))
		{
			tpad = cmd->tpad;
			if (tpad < 0) tpad = borders[-tpad - 1] + DEF_BORDER;
			cw = cmd->cw;
			if (cw < 0) cw = borders[-cw - 1] + DEF_BORDER;
			pk = cmd->pk;
			i = lp - ((pk & pk_MASK) >= pk_TABLE) - !!(pk & pkf_FRAME);
			if (i <= 0) v = NULL;
			op = cmd->cmd;
		}
		switch (op)
		{
		/* Terminate */
		case op_WEND: case op_WSHOW: case op_WDIALOG:
			/* Terminate the list */
			*r++ = NULL;
			*r++ = NULL;

			gtk_object_set_data(GTK_OBJECT(window), ident,
				(gpointer)res);
			gtk_signal_connect_object(GTK_OBJECT(window), "destroy",
				GTK_SIGNAL_FUNC(do_destroy), (gpointer)res);
			/* !!! Freeing the datastruct is best to happen only when
			 * all refs to underlying object are safely dropped */
			gtk_object_weakref(GTK_OBJECT(window),
				(GtkDestroyNotify)free, (gpointer)ddata);
#if GTK_MAJOR_VERSION == 1
			/* To make Smooth theme engine render sliders properly */
			if (have_sliders) gtk_signal_connect_after(
				GTK_OBJECT(window), "show",
				GTK_SIGNAL_FUNC(gtk_widget_queue_resize), NULL);
#endif
			/* Add finishing touches to a toplevel */
			if (!part)
			{
				gtk_window_set_transient_for(GTK_WINDOW(window),
					tparent);
				/* Trigger remembered events */
				trigger_things(res);
			}
			/* Display */
			if (op != op_WEND)
			{
				cmd_showhide(GET_WINDOW(res), TRUE);
				if (raise) gdk_window_raise(window->window);
			}
			/* Wait for input */
			if (op == op_WDIALOG)
			{
				*(void ***)v = NULL; // clear result slot
				vdata->dv = v; // announce it
				while (!*(void ***)v) gtk_main_iteration();
			}
			/* Return anchor position */
			return (res);
		/* Done with a container */
		case op_WDONE: ++wp; continue;
		/* Create the main window */
		case op_MAINWINDOW:
		{
			int wh = (int)pp[2];
			GdkPixmap *icon_pix;

			gdk_rgb_init();
			init_tablet();	// Set up the tablet
//			ag = gtk_accel_group_new();

			widget = window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
			// Set minimum width/height
			gtk_widget_set_usize(window, wh >> 16, wh & 0xFFFF);
			// Set name _without_ translating
			gtk_window_set_title(GTK_WINDOW(window), v);

	/* !!! If main window receives these events, GTK+ will be able to
	 * direct them to current modal window. Which makes it possible to
	 * close popups by clicking on the main window outside popup - WJ */
			gtk_widget_add_events(window,
				GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

			// we need to realize the window because we use pixmaps
			// for items on the toolbar & menus in the context of it
			gtk_widget_realize(window);

			icon_pix = gdk_pixmap_create_from_xpm_d(window->window,
				NULL, NULL, pp[1]);
			gdk_window_set_icon(window->window, NULL, icon_pix, NULL);
//			gdk_pixmap_unref(icon_pix);

			// For use as anchor and context
			mainwindow = window;

			pk = pkf_STACK; // bin container
			break;
		}
		/* Create a toplevel window, and put a vertical box inside it */
		case op_WINDOW: case op_WINDOWm:
			widget = window = add_a_window(GTK_WINDOW_TOPLEVEL,
				*(char *)v ? _(v) : v, wpos, op != op_WINDOW);
			*--wp = add_vbox(window);
			break;
		/* Create a dialog window, with vertical & horizontal box */
		case op_DIALOGm:
			widget = window = gtk_dialog_new();
			gtk_window_set_title(GTK_WINDOW(window), _(v));
			gtk_window_set_position(GTK_WINDOW(window), wpos);
			gtk_window_set_modal(GTK_WINDOW(window), TRUE);
			gtk_container_set_border_width(GTK_CONTAINER(window), 6);
			ag = gtk_accel_group_new();
 			gtk_window_add_accel_group(GTK_WINDOW(window), ag);
			/* Both boxes go onto stack, with vbox on top */
			*--wp = GTK_DIALOG(window)->action_area;
			*--wp = GTK_DIALOG(window)->vbox;
			break;
		/* Create a fileselector window (with horizontal box inside) */
		case op_FPICKpm:
			widget = window = fpick(&wp, ddata, pp - 1, r);
			/* Initialize */
			fpick_set_filename(window, v, FALSE);
			break;
		/* Create a vbox which will serve as separate widget */
		case op_TOPVBOX:
			part = TRUE; // not toplevel
			widget = window = gtk_vbox_new(FALSE, 0);
// !!! Border = 5
			cw = 5;
			pk = pkf_STACK | pkf_SHOW;
			break;
		/* Create a widget vbox with special sizing behaviour */
		case op_TOPVBOXV:
			part = TRUE; // not toplevel
			// Fill space vertically but not horizontally
			widget = window = gtk_alignment_new(0.0, 0.5, 0.0, 1.0);
			// Keep max vertical size
			widget_set_keepsize(window, TRUE);
			*--wp = add_vbox(window);
// !!! Border = 5
			gtk_container_set_border_width(GTK_CONTAINER(wp[0]), 5);
			pk = pkf_SHOW;
			break;
		/* Add a dock widget */
		case op_DOCK:
		{
			GtkWidget *p0, *p1, *pane;

			widget = gtk_hbox_new(FALSE, 0);
			gtk_widget_show(widget);

			/* First, create the dock pane - hidden for now */
			pane = gtk_hpaned_new();
			paned_mouse_fix(pane);
			gtk_box_pack_end(GTK_BOX(widget), pane, TRUE, TRUE, 0);

			/* Create the right pane */
			p1 = gtk_vbox_new(FALSE, 0);
			gtk_widget_show(p1);
			gtk_paned_pack2(GTK_PANED(pane), p1, FALSE, TRUE);

			/* Now, create the left pane - for now, separate */
			p0 = xpack(widget, gtk_vbox_new(FALSE, 0));
			gtk_widget_show(p0);

			/* Pack everything */
			gtk_container_add(GTK_CONTAINER(*wp), widget);
			*wp-- = p1; // right page second
			*wp = p0; // left page first

			break;
		}
		/* Add a notebook page */
		case op_PAGE: case op_PAGEi:
		{
			GtkWidget *label = op == op_PAGE ?
				gtk_label_new(_(v)) : xpm_image(v);
			gtk_widget_show(label);
			widget = gtk_vbox_new(FALSE, op == op_PAGE ?
				0 : (int)pp[1]);
			gtk_notebook_append_page(GTK_NOTEBOOK(wp[0]),
				widget, label);
			pk = pkf_SHOW | pkf_STACK;
			break;
		}
		/* Add a table */
		case cm_TABLE:
			widget = gtk_table_new((int)v & 0xFFFF, (int)v >> 16, FALSE);
			if (i > 1)
			{
				int s = (int)pp[1];
				gtk_table_set_row_spacings(GTK_TABLE(widget), s);
				gtk_table_set_col_spacings(GTK_TABLE(widget), s);
			}
			break;
		/* Add a framed scrollable table with dynamic name (ugh) */
		case op_FSXTABLEp:
		{
			GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

			gtk_widget_show(scroll);
			add_with_frame(NULL, *(char **)v, scroll);
			gtk_container_set_border_width(GTK_CONTAINER(scroll),
				GET_BORDER(FRBOX));
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

			widget = gtk_table_new((int)pp[1] & 0xFFFF,
				(int)pp[1] >> 16, FALSE);
			gtk_scrolled_window_add_with_viewport(
				GTK_SCROLLED_WINDOW(scroll), widget);

			pk = pk_XPACK | pkf_SHOW | pkf_STACK | pkf_PARENT;
			break;
		}
		/* Add a box */
		case cm_VBOX: case cm_HBOX:
			widget = (op == cm_VBOX ? gtk_vbox_new :
				gtk_hbox_new)(FALSE, (int)v & 255);
			if (pk & pkf_FRAME) break;
			cw = ((int)v >> 8) & 255;
			tpad = ((int)v >> 16) & 255;
			break;
		/* Add an equal-spaced horizontal box */
		case op_EQBOX:
			widget = gtk_hbox_new(TRUE, (int)v & 255);
			cw = (int)v >> 8;
			pk = pk_PACK | pkf_STACK | pkf_SHOW;
			break;
		/* Add a scrolled window */
		case cm_SCROLL:
			widget = gtk_scrolled_window_new(NULL, NULL);
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),
				scrollp[(int)v & 255], scrollp[(int)v >> 8]);
			break;
		/* Put a notebook into scrolled window */
		case op_SNBOOK:
			widget = gtk_notebook_new();
			gtk_notebook_set_tab_pos(GTK_NOTEBOOK(widget), GTK_POS_LEFT);
//			gtk_notebook_set_scrollable(GTK_NOTEBOOK(widget), TRUE);
			pk = pk_SCROLLVP | pkf_STACK | pkf_SHOW;
			break;
		/* Add a normal notebook */
		case op_NBOOK:
			widget = gtk_notebook_new();
//			gtk_notebook_set_tab_pos(GTK_NOTEBOOK(widget), GTK_POS_TOP);
			cw = GET_BORDER(NBOOK);
			pk = pk_XPACK | pkf_STACK | pkf_SHOW;
			break;
		/* Add a plain notebook */
		case op_PLAINBOOK:
		{
			// !!! no extra args
			int n = v ? (int)v : 2; // 2 pages by default
			// !!! All pages go onto stack, with #0 on top
			wp -= n;
			widget = pack(wp[n], plain_book(wp, n));
			break;
		}
		/* Add a toggle button for controlling 2-paged notebook */
		case op_BOOKBTN:
			widget = sig_toggle_button(_(pp[1]), FALSE, v,
				GTK_SIGNAL_FUNC(toggle_vbook));
			pk = pk_PACK;
			break;
		/* Add a horizontal line */
		case op_HSEP:
			widget = gtk_hseparator_new();
			pk = pk_PACK | pkf_SHOW;
// !!! Height = 10
			if ((int)v >= 0) gtk_widget_set_usize(widget,
				lp ? (int)v : -2, 10);
			break;
		/* Add a label */
		case cm_LABEL: case op_WLABEL:
		{
			int z = (int)((xcmdef *)cmd)->x;
			widget = gtk_label_new(*(char *)v ? _(v) : v);
			if (op == op_WLABEL)
				gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
			if (i > 1) z = (int)pp[1];
			if (z & 0xFFFF) gtk_misc_set_padding(GTK_MISC(widget),
				(z >> 8) & 255, z & 255);
			gtk_label_set_justify(GTK_LABEL(widget), GTK_JUSTIFY_LEFT);
			gtk_misc_set_alignment(GTK_MISC(widget),
				(z >> 16) / 10.0, 0.5);
			break;
		}
		/* Add to table a batch of labels generated from text string */
		case op_TLTEXT:
			tltext(v, pp - 1, wp[0], GET_BORDER(TLABEL));
			break;
		/* Add a non-spinning spin to table slot */
		case op_TLNOSPIN:
		{
			int n = *(int *)v;
			widget = add_a_spin(n, n, n);
			GTK_WIDGET_UNSET_FLAGS(widget, GTK_CAN_FOCUS);
			tpad = GET_BORDER(SPIN);
			pk = pk_TABLE;
			break;
		}
		/* Add a spin, fill from field/var */
		case cm_SPIN: case op_SPINc:
			widget = add_a_spin(*(int *)v, (int)pp[1], (int)pp[2]);
#if GTK2VERSION >= 4 /* GTK+ 2.4+ */
			if (op == op_SPINc) gtk_entry_set_alignment(
				GTK_ENTRY(&(GTK_SPIN_BUTTON(widget)->entry)), 0.5);
#endif
			break;
		/* Add float spin, fill from field/var */
		case cm_FSPIN:
			widget = add_float_spin(*(int *)v / 100.0,
				(int)pp[1] / 100.0, (int)pp[2] / 100.0);
			break;
		/* Add a spin, fill from array */
		case cm_SPINa:
		{
			int *xp = v;
			widget = add_a_spin(xp[0], xp[1], xp[2]);
			break;
		}
		/* Add a grid of spins, fill from array of arrays */
		// !!! Presents one widget out of all grid (the last one)
		case op_TLSPINPACK:
			widget = tlspinpack(v, pp - 1, r, wp[0]);
			break;
		/* Add a spinslider */
		case cm_SPINSLIDE: case op_HTSPINSLIDE: case cm_SPINSLIDEa:
		{
			int z = (int)((xcmdef *)cmd)->x;
			widget = mt_spinslide_new(z > 0xFFFF ? z >> 16 : -1,
				z & 0xFFFF ? z & 0xFFFF : -1);
			if (op == cm_SPINSLIDEa) mt_spinslide_set_range(widget,
				((int *)v)[1], ((int *)v)[2]);
			else mt_spinslide_set_range(widget, (int)pp[1], (int)pp[2]);
			mt_spinslide_set_value(widget, *(int *)v);
#if GTK_MAJOR_VERSION == 1
			have_sliders = TRUE;
#endif
			if (op == op_HTSPINSLIDE)
			{
				GtkWidget *label;
				int x;

				x = next_table_level(wp[0], TRUE);
				label = gtk_label_new(_(pp[--lp]));
				gtk_widget_show(label);
				gtk_misc_set_alignment(GTK_MISC(label),
					1.0 / 3.0, 0.5);
// !!! Padding = 0
				to_table(label, wp[0], 0, x, 0);
				gtk_table_attach(GTK_TABLE(wp[0]), widget,
					x, x + 1, 1, 2,
					GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
			}
			break;
		}
		/* Add a named checkbox, fill from field/var */
		case cm_CHECK:
			widget = gtk_check_button_new_with_label(_(pp[1]));
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
				*(int *)v);
			break;
		/* Add a named checkbox, fill from inifile */
		case op_CHECKb:
			widget = sig_toggle(_(pp[2]), inifile_get_gboolean(v,
				(int)pp[1]), NULL, NULL);
			pk = pk_PACK;
			break;
		/* Add a pack of radio buttons for field/var */
		case cm_RPACK: case cm_RPACKD:
		{
			char **src = pp[1];
			int nh = (int)pp[2];
			int n = nh >> 8;
			if (op == cm_RPACKD) n = -1 ,
				src = *(char ***)((char *)ddata + (int)pp[1]);
			if (!n) n = -1;
#if U_NLS
			n = n_trans(tc, src, n);
			src = tc;
#endif
			widget = wj_radio_pack(src, n, nh & 255, *(int *)v,
				ref > 1 ? NEXT_SLOT(r) : NULL,
				ref > 1 ? GTK_SIGNAL_FUNC(get_evt_1_t) : NULL);
			break;
		}
		/* Add an option menu or combobox for field/var */
		case cm_OPT: case cm_OPTD: case op_COMBO:
		{
			char **src = pp[1];
			int n = (int)pp[2];
			if (op == cm_OPTD) n = -1 ,
				src = *(char ***)((char *)ddata + (int)pp[1]);
			if (!n) n = -1;
#if U_NLS
			n = n_trans(tc, src, n);
			src = tc;
#endif
			widget = (op == op_COMBO ? wj_combo_box : wj_option_menu)
				(src, n, *(int *)v,
				ref > 1 ? NEXT_SLOT(r) : NULL,
				ref > 1 ? GTK_SIGNAL_FUNC(get_evt_1) : NULL);
			break;
		}
		/* Add an entry widget, fill from drop-away buffer */
		case op_XENTRY: case op_MLENTRY: case op_TLENTRY:
			widget = gtk_entry_new();
			if (i > 1) gtk_entry_set_max_length(GTK_ENTRY(widget), (int)pp[1]);
			gtk_entry_set_text(GTK_ENTRY(widget), *(char **)v);
			if (op == op_MLENTRY) accept_ctrl_enter(widget);
			// Replace transient buffer - it may get freed on return
			*(const char **)v = gtk_entry_get_text(GTK_ENTRY(widget));
			break;
		/* Add a path entry or box to table */
		case cm_PENTRY:
			widget = gtk_entry_new_with_max_length((int)pp[1]);
			set_path(widget, v, PATH_VALUE);
			break;
		/* Add a path box to table */
		case op_PATHs:
			v = inifile_get(v, ""); // read and fallthrough
		case op_PATH:
			widget = pathbox(r);
			set_path(widget, v, PATH_VALUE);
			pk = pk_PACK | pkf_SHOW | pkf_PARENT | pkf_FRAME;
			break;
		/* Add a text widget, fill from drop-away buffer at pointer */
		case op_TEXT:
			widget = textarea(*(char **)v);
			*(char **)v = NULL;
// !!! Padding = 0 Border = 0
			pk = pk_XPACK | pkf_PARENT; // wrapped
			break;
		/* Add a color picker box, w/field array, & leave unfilled(!) */
		case op_COLOR: case op_TCOLOR:
			widget = cpick_create();
			cpick_set_opacity_visibility(widget, op == op_TCOLOR);
			pk = pk_PACK | pkf_SHOW;
			break;
		/* Add a colorlist box, fill from fields */
		case op_COLORLIST: case op_COLORLISTN:
			widget = colorlist(wp[0], v, ddata, pp - 1, NEXT_SLOT(r));
			break;
		/* Add a colorpad */
		case op_COLORPAD:
			widget = colorpad(v, ddata, pp - 1, NEXT_SLOT(r));
			pk = pk_PACK | pkf_SHOW;
			break;
		/* Add a buttonbar for gradient */
		case op_GRADBAR:
			widget = gradbar(v, ddata, pp - 1, NEXT_SLOT(r));
			pk = pk_PACK;
			break;
		/* Add a list with pre-defined columns */
		case op_LISTCCr: case op_LISTCCHr:
			widget = listcc(v, ddata, pp - 1, &c, r);
// !!! Border = 5 / 0
			if (op == op_LISTCCr) cw = 5;
			pk = pk_SCROLLVPv | pkf_SHOW;
			break;
		/* Add a clist with pre-defined columns */
		case op_LISTC: case op_LISTCd: case op_LISTCu:
		case op_LISTCS: case op_LISTCX:
			widget = listc(v, ddata, pp - 1, &c, r);
// !!! Border = 0
			pk = pk_BIN | pkf_SHOW;
			break;
		/* Add a box with "OK"/"Cancel", or something like */
		case cm_OKBOX: case op_EOKBOX: case cm_UOKBOX:
		{
			GtkWidget *ok_bt, *cancel_bt, *box;
			void **cancel_r;

			ag = gtk_accel_group_new();
 			gtk_window_add_accel_group(GTK_WINDOW(window), ag);

			widget = gtk_hbox_new(op != cm_UOKBOX, 0);
			if (op == op_EOKBOX) // clustered to right side
			{
				box = gtk_hbox_new(FALSE, 0);
				gtk_widget_show(box);
// !!! Min width = 260
				pack_end(box, widget_align_minsize(widget, 260, -1));
			}
			if (ref < 2) break; // empty box for separate buttons

			ok_bt = cancel_bt = gtk_button_new_with_label(_(v));
			gtk_container_set_border_width(GTK_CONTAINER(ok_bt),
				GET_BORDER(BUTTON));
			gtk_widget_show(ok_bt);
			/* OK-event */
			add_click(cancel_r = NEXT_SLOT(r), ok_bt);
			if (pp[1])
			{
				cancel_bt = xpack(widget,
					gtk_button_new_with_label(_(pp[1])));
				gtk_container_set_border_width(
					GTK_CONTAINER(cancel_bt), GET_BORDER(BUTTON));
				gtk_widget_show(cancel_bt);
				/* Cancel-event */
				add_click(cancel_r = SLOT_N(r, 2), cancel_bt);
			}
			add_del(cancel_r, window);
			xpack(widget, ok_bt);

			gtk_widget_add_accelerator(cancel_bt, "clicked", ag,
				GDK_Escape, 0, (GtkAccelFlags)0);
			gtk_widget_add_accelerator(ok_bt, "clicked", ag,
				GDK_Return, 0, (GtkAccelFlags)0);
			gtk_widget_add_accelerator(ok_bt, "clicked", ag,
				GDK_KP_Enter, 0, (GtkAccelFlags)0);
			break;
		}
		/* Add a clickable button */
		case op_OKBTN: case cm_CANCELBTN: case op_OKADD: case cm_BUTTON:
		{
			widget = gtk_button_new_with_label(_(v));
			if (op == op_OKBTN)
			{
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_Return, 0, (GtkAccelFlags)0);
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_KP_Enter, 0, (GtkAccelFlags)0);
			}
			else if (op == cm_CANCELBTN)
			{
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_Escape, 0, (GtkAccelFlags)0);
				add_del(NEXT_SLOT(r), window);
			}
			/* Click-event */
			if ((op != cm_BUTTON) || pp[1 + 1])
				add_click(NEXT_SLOT(r), widget);
			break;
		}
		/* Add a toggle button to OK-box */
		case cm_TOGGLE:
			widget = gtk_toggle_button_new_with_label(_(pp[1]));
			if (pp[3]) gtk_signal_connect(GTK_OBJECT(widget),
				"toggled", GTK_SIGNAL_FUNC(get_evt_1), NEXT_SLOT(r));
			break;
		/* Add a mount socket with custom-built separable widget */
		case op_MOUNT: case op_PMOUNT:
		{
			void **what = ((mnt_fn)pp[1])(res);
			*(int *)v = TRUE;
			widget = gtk_alignment_new(0.5, 0.5, 1.0, 1.0);
			gtk_container_add(GTK_CONTAINER(widget),
				GET_REAL_WINDOW(what));
			pk = pk_PACK | pkf_SHOW;
			if (op == op_PMOUNT) // put socket in pane w/preset size
			{
				GtkWidget *pane = gtk_vpaned_new();
				paned_mouse_fix(pane);
				gtk_paned_set_position(GTK_PANED(pane),
					inifile_get_gint32(pp[2], (int)pp[3]));
				gtk_paned_pack2(GTK_PANED(pane),
					gtk_vbox_new(FALSE, 0), TRUE, TRUE);
				gtk_widget_show_all(pane);
				gtk_paned_pack1(GTK_PANED(pane),
					widget, FALSE, TRUE);
				pk = pk_XPACK | pkf_SHOW | pkf_PARENT;
			}
			break;
		}
		/* Steal a widget from its mount socket by slot ref */
		case op_REMOUNT:
		{
			void **where = *(void ***)v;
			GtkWidget *what = GTK_BIN(where[0])->child;

			widget = gtk_alignment_new(0.5, 0.5, 1.0, 1.0);
			gtk_widget_hide(where[0]);
			gtk_widget_reparent(what, widget);
			get_evt_1(where[0], NEXT_SLOT(where));
			pk = pk_XPACK | pkf_SHOW;
			break;
		}
		/* Call a function */
		case op_EXEC:
			r = ((ext_fn)v)(r, &wp, res);
			continue;
		/* Do a V-code direct jump */
		case op_GOTO:
			ifcode = v;
			continue;
		/* Call a V-code subroutine, indirect from field/var */
// !!! Maybe add opcode for direct call too
		case op_CALLp:
			*rp++ = ifcode;
			ifcode = *(void **)v;
			continue;
		/* Return from V-code subroutine */
		case op_RET:
			ifcode = *--rp;
			continue;
		/* Skip next token(s) if/unless field/var is unset */
		case op_IF: case op_UNLESS:
			if (!*(int *)v ^ (op != op_IF))
				ifcode = skip_if(pp - 1);
			continue;
		/* Skip next token(s) unless inifile var, set by default, is unset */
		case op_UNLESSbt:
			if (inifile_get_gboolean(v, TRUE))
				ifcode = skip_if(pp - 1);
			continue;
		/* Store a reference to whatever is next into field */
		case op_REF:
			*(void **)v = r;
			continue;
		/* Make toplevel window shrinkable */
		case op_MKSHRINK:
			gtk_window_set_policy(GTK_WINDOW(window), TRUE, TRUE, FALSE);
			continue;
		/* Make toplevel window non-resizable */
		case op_NORESIZE:
			gtk_window_set_policy(GTK_WINDOW(window), FALSE, FALSE, TRUE);
			continue;
		/* Make scrolled window request max size */
		case op_WANTMAX:
			gtk_signal_connect(GTK_OBJECT(widget), "size_request",
				GTK_SIGNAL_FUNC(scroll_max_size_req), NULL);
			continue;
		/* Make widget keep max requested width */
		case op_KEEPWIDTH:
			widget_set_keepsize(widget, FALSE);
			continue;
		/* Use saved size & position for window */
		case op_WXYWH:
		{
			unsigned int n = (unsigned)pp[1];
			vdata->ininame = v;
			vdata->xywh[2] = n >> 16;
			vdata->xywh[3] = n & 0xFFFF;
			if (v) rw_pos(vdata, FALSE);
			continue;
		}
		/* Make toplevel window be positioned at mouse */
		case op_WPMOUSE: wpos = GTK_WIN_POS_MOUSE; continue;
		/* Make toplevel window be positioned anywhere WM pleases */
		case op_WPWHEREVER: wpos = GTK_WIN_POS_NONE; continue;
		/* Make last referrable widget hidden */
		case op_HIDDEN:
			gtk_widget_hide(*origin_slot(PREV_SLOT(r)));
			continue;
		/* Make last referrable widget insensitive */
		case op_INSENS:
			gtk_widget_set_sensitive(*origin_slot(PREV_SLOT(r)), FALSE);
			continue;
		/* Make last referrable widget focused */
		case op_FOCUS:
			gtk_widget_grab_focus(*origin_slot(PREV_SLOT(r)));
			continue;
		/* Set fixed/minimum width for next widget */
		case op_WIDTH:
			minw = (int)v;
			continue;
		/* Make window transient to given widget-map */
		case op_ONTOP:
			tparent = !v ? NULL :
				GTK_WINDOW(GET_REAL_WINDOW(*(void ***)v));
			continue;
		/* Change identifier, for reusable toplevels */
		case op_IDENT:
			ident = v;
			continue;
		/* Raise window after displaying */
		case op_RAISED:
			raise = TRUE;
			continue;
		/* Start group of list columns */
		case op_WLIST:
			memset(&c, 0, sizeof(c));
			continue;
		/* Add a datablock pseudo-column */
		case op_COLUMNDATA:
			c.dcolumn = pp - 1;
			continue;
		/* Add a regular list column */
		case op_IDXCOLUMN: case op_TXTCOLUMN: case op_XTXTCOLUMN:
		case op_RTXTCOLUMN: case op_RFILECOLUMN: case op_CHKCOLUMNi0:
			c.columns[c.ncol++] = r;
			widget = NULL;
			break;
		/* Install activate event handler */
		case op_EVT_OK:
		{
			void **slot = origin_slot(PREV_SLOT(r));
			int what = GET_OP(slot);
// !!! Support only what actually used on, and their brethren
			switch (what)
			{
			case op_XENTRY: case op_MLENTRY: case op_TLENTRY:
			case op_XPENTRY: case op_TPENTRY:
			case op_PATH: case op_PATHs:
				gtk_signal_connect(GTK_OBJECT(*slot), "activate",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_LISTC: case op_LISTCd: case op_LISTCu:
			case op_LISTCS: case op_LISTCX:
			{
				listc_data *ld = gtk_object_get_user_data(
					GTK_OBJECT(*slot));
				ld->ok = r;
				break;
			}
			}
			widget = NULL;
			break;
		}
		/* Install destroy event handler (onto window) */
		case op_EVT_CANCEL:
			add_del(r, window);
			widget = NULL;
			break;
		/* Install deallocation event handler */
		case op_EVT_DESTROY:
			vdata->destroy = r;
			widget = NULL;
			break;
		/* Install key event handler */
		case op_EVT_KEY:
		{
			void **slot = origin_slot(PREV_SLOT(r));
			gtk_signal_connect(GTK_OBJECT(*slot), "key_press_event",
				GTK_SIGNAL_FUNC(get_evt_key), r);
			widget = NULL;
			break;
		}
		/* Install Change-event handler */
		case op_EVT_CHANGE:
		{
			void **slot = origin_slot(PREV_SLOT(r));
			int what = GET_OP(slot);
// !!! Support only what actually used on, and their brethren
			switch (what)
			{
			case op_SPIN: case op_SPINc: case op_XSPIN:
			case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
			case op_SPINa: case op_XSPINa: case op_TSPINa:
			case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
				spin_connect(*slot,
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_TSPINSLIDE: case op_TLSPINSLIDE:
			case op_TLSPINSLIDEs: case op_TLSPINSLIDEx:
			case op_HTSPINSLIDE:
			case op_SPINSLIDEa: case op_XSPINSLIDEa:
				mt_spinslide_connect(*slot,
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_CHECK: case op_XCHECK: case op_TLCHECK:
			case op_CHECKb:
				gtk_signal_connect(GTK_OBJECT(*slot), "toggled",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_COLOR: case op_TCOLOR:
				gtk_signal_connect(GTK_OBJECT(*slot), "color_changed",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_TEXT:
#if GTK_MAJOR_VERSION == 2 /* In GTK+1, same handler as for GtkEntry */
				g_signal_connect(GTK_TEXT_VIEW(*slot)->buffer,
					"changed", GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
#endif
			case op_XENTRY: case op_MLENTRY: case op_TLENTRY:
			case op_XPENTRY: case op_TPENTRY:
			case op_PATH: case op_PATHs:
				gtk_signal_connect(GTK_OBJECT(*slot), "changed",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_LISTCX:
			{
				listc_data *ld = gtk_object_get_user_data(
					GTK_OBJECT(*slot));
				ld->change = r;
				break;
			}
			}
		} // fallthrough
		/* Remember that event needs triggering here */
		/* Or remember start of a group of widgets */
		/* Or a cleanup location */
		case op_TRIGGER: case op_GROUP: case op_CLEANUP:
			widget = NULL;
			break;
		/* Set nondefault border size */
		case op_BOR_TABLE: case op_BOR_NBOOK: case op_BOR_XSCROLL:
		case op_BOR_SPIN: case op_BOR_SPINSLIDE:
		case op_BOR_LABEL: case op_BOR_TLABEL:
		case op_BOR_OPT: case op_BOR_XOPT:
		case op_BOR_FRBOX: case op_BOR_OKBOX: case op_BOR_BUTTON:
			borders[op - op_BOR_0] = lp ? (int)v - DEF_BORDER : 0;
			continue;
		default: continue;
		}
		/* Remember this */
		if (ref)
		{
			*r++ = widget ? (void *)widget : res;
			*r++ = pp - 1;
		}
		*(wp - 1) = widget; // pre-stack
		/* Show this */
		if (pk & pkf_SHOW) gtk_widget_show(widget);
		/* Border this */
		if (cw) gtk_container_set_border_width(GTK_CONTAINER(widget), cw);
		/* Unwrap this */
		if (pk & pkf_PARENT)
			while (widget->parent) widget = widget->parent;
		/* Frame this */
		if (pk & pkf_FRAME)
			widget = add_with_frame(NULL, _(pp[--lp]), widget);
		/* Set min width for this */
// !!! For now, always use wrapper
		if (minw < 0) widget = widget_align_minsize(widget, -minw, -2);
		/* Or fixed width */
		else if (minw) gtk_widget_set_usize(widget, minw, -2);
		minw = 0;
		/* Pack this */
		switch (n = pk & pk_MASK)
		{
		case pk_PACK: tpad = 0;
		case pk_PACKp:
			gtk_box_pack_start(GTK_BOX(wp[0]), widget,
				FALSE, FALSE, tpad);
			break;
		case pk_XPACK: case pk_XPACK1: case pk_EPACK:
			gtk_box_pack_start(GTK_BOX(wp[0]), widget,
				TRUE, n != pk_EPACK, tpad);
			if (n == pk_XPACK1)
				gtk_box_reorder_child(GTK_BOX(wp[0]), widget, 1);
			break;
		case pk_PACKEND:
			gtk_box_pack_end(GTK_BOX(wp[0]), widget,
				FALSE, FALSE, tpad);
			break;
		case pk_TABLE: case pk_TABLEx: case pk_TABLEp:
			table_it(wp[0], widget, (int)pp[--lp], tpad, n);
			break;
		case pk_TABLE2: case pk_TABLE2x:
		{
			int y = next_table_level(wp[0], FALSE);
			add_to_table(_(pp[--lp]), wp[0], y, 0, GET_BORDER(TLABEL));
			gtk_table_attach(GTK_TABLE(wp[0]), widget, 1, 2,
				y, y + 1, GTK_EXPAND | GTK_FILL,
				n == pk_TABLE2x ? GTK_FILL : 0, 0, tpad);
			break;
		}
		case pk_SCROLLVP: case pk_SCROLLVPn: case pk_SCROLLVPv:
		{
			GtkWidget *tw = wp[0];
			GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(tw);

			wp[0] = *(wp - 1); ++wp; // unstack
			gtk_scrolled_window_add_with_viewport(sw, widget);
			if (n == pk_SCROLLVPv)
			{
				gtk_container_set_focus_vadjustment(GTK_CONTAINER(widget),
					gtk_scrolled_window_get_vadjustment(sw));
			}
			if (n != pk_SCROLLVPn) break;
			/* Set viewport to shadowless */
			tw = GTK_BIN(tw)->child;
			gtk_viewport_set_shadow_type(GTK_VIEWPORT(tw), GTK_SHADOW_NONE);
			vport_noshadow_fix(tw);
			break;
		}
		case pk_BIN:
			gtk_container_add(GTK_CONTAINER(wp[0]), widget);
			wp[0] = *(wp - 1); ++wp; // unstack
			break;
		}
		/* Stack this */
		if (pk & pkf_STACK) --wp;
		/* Remember events */
		if (ref > 2)
		{
			*r++ = res;
			*r++ = pp + lp - 4;
		}
		if (ref > 1)
		{
			*r++ = res;
			*r++ = pp + lp - 2;
		}
	}
}

static void do_destroy(void **wdata)
{
	void **pp, *v = NULL;
	char *data = GET_DDATA(wdata);
	v_dd *vdata = GET_VDATA(wdata);
	int op;

	if (vdata->done) return; // Paranoia
	vdata->done = TRUE;

	if (vdata->destroy)
	{
		void **base = vdata->destroy[0], **desc = vdata->destroy[1];
		((evt_fn)desc[1])(GET_DDATA(base), base,
			(int)desc[0] & WB_OPMASK, vdata->destroy);
	}

	for (wdata = GET_WINDOW(wdata); (pp = wdata[1]); wdata += 2)
	{
		op = (int)*pp++;
		v = pp[0];
		if (op & WB_FFLAG) v = data + (int)v;
		op &= WB_OPMASK;
		if (op == op_CLEANUP) free(*(void **)v);
		else if (op == op_TEXT) g_free(*(char **)v);
		else if (op == op_REMOUNT)
		{
			void **where = *(void ***)v;
			GtkWidget *what = GTK_BIN(*wdata)->child;

			gtk_widget_reparent(what, where[0]);
			gtk_widget_show(where[0]);
			get_evt_1(where[0], NEXT_SLOT(where));
		}
		else if (op == op_LISTCX) listcx_done(GTK_CLIST(*wdata),
			gtk_object_get_user_data(GTK_OBJECT(*wdata)));
		else if (op == op_MAINWINDOW) gtk_main_quit();
	}
}

static void *do_query(char *data, void **wdata, int mode)
{
	void **pp, *v = NULL;
	int op;

	for (; (pp = wdata[1]); wdata += 2)
	{
		op = (int)*pp++;
		v = op & (~0 << WB_LSHIFT) ? pp[0] : NULL;
		if (op & WB_FFLAG) v = data + (int)v;
		op &= WB_OPMASK;
		switch (op)
		{
		case op_FPICKpm:
			fpick_get_filename(*wdata, v, PATHBUF, FALSE);
			break;
		case op_SPIN: case op_SPINc: case op_XSPIN:
		case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
		case op_SPINa: case op_XSPINa: case op_TSPINa:
			*(int *)v = mode & 1 ? gtk_spin_button_get_value_as_int(
				GTK_SPIN_BUTTON(*wdata)) : read_spin(*wdata);
			break;
		case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
			*(int *)v = rint((mode & 1 ?
				GTK_SPIN_BUTTON(*wdata)->adjustment->value :
				read_float_spin(*wdata)) * 100);
			break;
		case op_TSPINSLIDE:
		case op_TLSPINSLIDE: case op_TLSPINSLIDEs: case op_TLSPINSLIDEx:
		case op_HTSPINSLIDE: case op_SPINSLIDEa: case op_XSPINSLIDEa:
			*(int *)v = (mode & 1 ? mt_spinslide_read_value :
				mt_spinslide_get_value)(*wdata);
			break;
		case op_CHECK: case op_XCHECK: case op_TLCHECK:
		case op_OKTOGGLE: case op_UTOGGLE:
			*(int *)v = gtk_toggle_button_get_active(
				GTK_TOGGLE_BUTTON(*wdata));
			break;
		case op_CHECKb:
			inifile_set_gboolean(v, gtk_toggle_button_get_active(
				GTK_TOGGLE_BUTTON(*wdata)));
			break;
		case op_COLORLIST: case op_COLORLISTN:
		case op_COLORPAD: case op_GRADBAR:
		case op_LISTCCr: case op_LISTCCHr:
		case op_LISTC: case op_LISTCd: case op_LISTCu:
		case op_LISTCS: case op_LISTCX:
			break; // self-reading
		case op_COLOR:
			*(int *)v = cpick_get_colour(*wdata, NULL);
			break;
		case op_TCOLOR:
			*(int *)v = cpick_get_colour(*wdata, (int *)v + 1);
			break;
		case op_RPACK: case op_RPACKD: case op_FRPACK:
			*(int *)v = wj_radio_pack_get_active(*wdata);
			break;
		case op_OPT: case op_XOPT: case op_TOPT: case op_TLOPT:
		case op_OPTD:
			*(int *)v = wj_option_menu_get_history(*wdata);
			break;
		case op_COMBO:
			*(int *)v = wj_combo_box_get_history(*wdata);
			break;
		case op_XENTRY: case op_MLENTRY: case op_TLENTRY:
			*(const char **)v = gtk_entry_get_text(GTK_ENTRY(*wdata));
			break;
		case op_XPENTRY: case op_TPENTRY: case op_PATH:
			gtkncpy(v, gtk_entry_get_text(GTK_ENTRY(*wdata)),
				op != op_PATH ? (int)pp[1] : PATHBUF);
			break;
		case op_PATHs:
		{
			char path[PATHBUF];
			gtkncpy(path, gtk_entry_get_text(GTK_ENTRY(*wdata)), PATHBUF);
			inifile_set(v, path);
			break;
		}
		case op_TEXT:
			g_free(*(char **)v);
			*(char **)v = read_textarea(*wdata);
			break;
		case op_MOUNT: case op_PMOUNT:
			*(int *)v = !!GTK_BIN(*wdata)->child;
			break;
		default: v = NULL; break;
		}
		if (mode > 1) return (v);
	}
	return (NULL);
}

void run_query(void **wdata)
{
	update_window_spin(GET_REAL_WINDOW(wdata));
	do_query(GET_DDATA(wdata), GET_WINDOW(wdata), 0);
}

void run_destroy(void **wdata)
{
	v_dd *vdata = GET_VDATA(wdata);
	if (vdata->done) return; // already destroyed
	if (vdata->ininame && vdata->ininame[0])
		cmd_showhide(GET_WINDOW(wdata), FALSE); // save position & size
	destroy_dialog(GET_REAL_WINDOW(wdata));
}

void cmd_reset(void **slot, void *ddata)
{
// !!! Support only what actually used on, and their brethren
	void *v, **pp, **wdata = slot;
	int op, group, cgroup;

	cgroup = group = -1;
	for (; (pp = wdata[1]); wdata += 2)
	{
		op = (int)*pp++;
		v = WB_GETLEN(op) ? pp[0] : NULL;
		if (op & WB_FFLAG) v = (char *)ddata + (int)v;
		op &= WB_OPMASK;
		if ((op != op_GROUP) && (cgroup != group)) continue;
		switch (op)
		{
		case op_GROUP:
			group = (int)v;
			if (cgroup < 0) cgroup = group;
			break;
		case op_SPIN: case op_SPINc: case op_XSPIN:
		case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
		case op_SPINa: case op_XSPINa: case op_TSPINa:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(*wdata),
				*(int *)v);
			break;
		case op_TLSPINPACK:
		{
			void **vp = gtk_object_get_user_data(GTK_OBJECT(*wdata));
			int i, n = (int)pp[1];

			*vp = NULL; // Lock
			for (i = 0; i < n; i++)
			{
				GtkSpinButton *spin = GTK_SPIN_BUTTON(vp[i * 2 + 1]);
				int *p = (int *)v + i * 3;

				gtk_spin_button_set_value(spin, *p);
				/* Value might get clamped, and slot is
				 * self-reading so should reflect that */
				*p = gtk_spin_button_get_value_as_int(spin);
			}
			*vp = wdata; // Unlock
			break;
		}
		case op_TSPINSLIDE:
		case op_TLSPINSLIDE: case op_TLSPINSLIDEs: case op_TLSPINSLIDEx:
		case op_HTSPINSLIDE: case op_SPINSLIDEa: case op_XSPINSLIDEa:
			mt_spinslide_set_value(*wdata, *(int *)v);
			break;
		case op_CHECK: case op_XCHECK: case op_TLCHECK:
		case op_OKTOGGLE: case op_UTOGGLE:
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(*wdata),
				*(int *)v);
			break;
		case op_OPT: case op_XOPT: case op_TOPT: case op_TLOPT:
		case op_OPTD:
			gtk_option_menu_set_history(GTK_OPTION_MENU(*wdata),
				*(int *)v);
			break;
		case op_LISTCCr: case op_LISTCCHr:
			listcc_reset(GTK_LIST(*wdata),
				gtk_object_get_user_data(GTK_OBJECT(*wdata)), -1);
			/* !!! Or the changes will be ignored if the list wasn't
			 * yet displayed (as in inactive dock tab) - WJ */
			gtk_widget_queue_resize(*wdata);
			break;
		case op_LISTC: case op_LISTCd: case op_LISTCu:
		case op_LISTCS: case op_LISTCX:
			listc_reset(GTK_CLIST(*wdata),
				gtk_object_get_user_data(GTK_OBJECT(*wdata)));
			break;
		case op_XENTRY: case op_MLENTRY: case op_TLENTRY:
			gtk_entry_set_text(GTK_ENTRY(*wdata), *(char **)v);
			// Replace transient buffer - it may get freed on return
			*(const char **)v = gtk_entry_get_text(GTK_ENTRY(*wdata));
			break;
		case op_PATHs:
			v = inifile_get(v, ""); // read and fallthrough
		case op_XPENTRY: case op_TPENTRY: case op_PATH:
			set_path(*wdata, v, PATH_VALUE);
			break;
#if 0 /* Not needed for now */
		case op_FPICKpm:
			fpick_set_filename(*wdata, v, FALSE);
			break;
		case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(*wdata),
				*(int *)v * 0.01);
			break;
		case op_CHECKb:
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(*wdata),
				inifile_get_gboolean(v, (int)pp[1]));
			break;
		case op_RPACK: case op_RPACKD: case op_FRPACK:
		case op_COLORLIST: case op_COLORLISTN:
// !!! No ready setter functions for these (and no need of them yet)
			break;
		case op_COLOR:
			cpick_set_colour(slot[0], *(int *)v, 255);
			break;
		case op_TCOLOR:
			cpick_set_colour(slot[0], ((int *)v)[0], ((int *)v)[1]);
			break;
#endif
		}
		if (cgroup < 0) return;
	}
}

void cmd_sensitive(void **slot, int state)
{
	int op = GET_OP(slot);

	if (op >= op_EVT_0) return; // only widgets
	gtk_widget_set_sensitive(slot[0], state);
	/* Move focus in LISTCC when delayed by insensitivity */
	if (state && ((op == op_LISTCCr) || (op == op_LISTCCHr)))
	{
		listcc_data *dt = gtk_object_get_user_data(GTK_OBJECT(slot[0]));
		if (dt->wantfoc) listcc_select_item(GTK_LIST(slot[0]), dt);
	}
}

void cmd_showhide(void **slot, int state)
{
	if (GET_OP(slot) >= op_EVT_0) return; // only widgets
	if (!GTK_WIDGET_VISIBLE(slot[0]) ^ !!state) return; // no use
	if (GET_OP(PREV_SLOT(slot)) == op_WDONE) // toplevels are special
	{
		v_dd *vdata = GET_VDATA(PREV_SLOT(slot));
		GtkWidget *w = GTK_WIDGET(slot[0]);

		if (state) // show - apply stored size & position
		{
			if (vdata->ininame) gtk_widget_set_uposition(w,
				vdata->xywh[0], vdata->xywh[1]);
			else vdata->ininame = ""; // first time
			gtk_window_set_default_size(GTK_WINDOW(w),
				vdata->xywh[2] ? vdata->xywh[2] : -1,
				vdata->xywh[3] ? vdata->xywh[3] : -1);
		}
		else // hide - remember size & position
		{
			gdk_window_get_size(w->window,
				vdata->xywh + 2, vdata->xywh + 3);
			gdk_window_get_root_origin(w->window,
				vdata->xywh + 0, vdata->xywh + 1);
			if (vdata->ininame && vdata->ininame[0])
				rw_pos(vdata, TRUE);
		}
	}
	widget_showhide(slot[0], state);
}

void cmd_set(void **slot, int v)
{
	slot = origin_slot(slot);
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_DOCK:
	{
		GtkWidget *window, *vbox, *pane = BOX_CHILD_0(slot[0]);
		char *ini = ((void **)slot[1])[1];
		int w, w2;

		if (!v ^ !!GTK_PANED(pane)->child1) return; // nothing to do

		window = gtk_widget_get_toplevel(slot[0]);
		if (GTK_WIDGET_VISIBLE(window))
			gdk_window_get_size(window->window, &w2, NULL);
		/* Window size isn't yet valid */
		else gtk_object_get(GTK_OBJECT(window), "default_width", &w2, NULL);

		if (v)
		{
			/* Restore dock size if set, autodetect otherwise */
			w = inifile_get_gint32(ini, -1);
			if (w >= 0) gtk_paned_set_position(GTK_PANED(pane), w2 - w);
			/* Now, let's juggle the widgets */
			vbox = BOX_CHILD_1(slot[0]);
			gtk_widget_ref(vbox);
			gtk_container_remove(GTK_CONTAINER(slot[0]), vbox);
			gtk_paned_pack1(GTK_PANED(pane), vbox, TRUE, TRUE);
			gtk_widget_show(pane);
		}
		else
		{
			inifile_set_gint32(ini, w2 - GTK_PANED(pane)->child1_size);
			gtk_widget_hide(pane);
			vbox = GTK_PANED(pane)->child1;
			gtk_widget_ref(vbox);
			gtk_container_remove(GTK_CONTAINER(pane), vbox);
			xpack(slot[0], vbox);
		}
		gtk_widget_unref(vbox);
		break;
	}
	case op_TSPINSLIDE:
	case op_TLSPINSLIDE: case op_TLSPINSLIDEs: case op_TLSPINSLIDEx:
	case op_HTSPINSLIDE: case op_SPINSLIDEa: case op_XSPINSLIDEa:
		mt_spinslide_set_value(slot[0], v);
		break;
	case op_TLNOSPIN:
		spin_set_range(slot[0], v, v);
		break;
	case op_SPIN: case op_SPINc: case op_XSPIN:
	case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
	case op_SPINa: case op_XSPINa: case op_TSPINa:
		gtk_spin_button_set_value(slot[0], v);
		break;
	case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
		gtk_spin_button_set_value(slot[0], v / 100.0);
		break;
	case op_CHECK: case op_XCHECK: case op_TLCHECK:
	case op_OKTOGGLE: case op_UTOGGLE:
		gtk_toggle_button_set_active(slot[0], v);
		break;
	case op_OPT: case op_XOPT: case op_TOPT: case op_TLOPT: case op_OPTD:
		gtk_option_menu_set_history(slot[0], v);
		break;
	case op_COLORPAD:
		colorpad_set(slot, v);
		break;
	case op_PLAINBOOK:
		gtk_notebook_set_page(slot[0], v);
		break;
	case op_LISTCCr: case op_LISTCCHr:
	{
		listcc_data *dt = gtk_object_get_user_data(GTK_OBJECT(slot[0]));
		if ((v < 0) || (v >= *dt->cnt)) break; // Ensure sanity
		*dt->idx = v;
		listcc_select_item(GTK_LIST(slot[0]), dt);
		break;
	}
	case op_LISTC: case op_LISTCd: case op_LISTCu:
	case op_LISTCS: case op_LISTCX:
	{
		GtkWidget *widget = GTK_WIDGET(slot[0]);
		GtkCList *clist = GTK_CLIST(slot[0]);
		int row = gtk_clist_find_row_from_data(clist, (gpointer)v);

		if (row < 0) break; // Paranoia
		gtk_clist_select_row(clist, row, 0);
		/* !!! Focus fails to follow selection in browse mode - have to
		 * move it here, but a full redraw is necessary afterwards */
		if (clist->focus_row == row) break;
		clist->focus_row = row;
		if (GTK_WIDGET_HAS_FOCUS(widget) && !clist->freeze_count)
			gtk_widget_queue_draw(widget);
		break;
	}
	}
}

void cmd_set2(void **slot, int v0, int v1)
{
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_COLOR: case op_TCOLOR:
		cpick_set_colour(slot[0], v0, v1);
		break;
	case op_COLORLIST: case op_COLORLISTN:
	{
		colorlist_set_color(slot[0], v0, v1);
		break;
	}
	}
}

void cmd_set3(void **slot, int *v)
{
	int n = v[0];
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_TSPINSLIDE:
	case op_TLSPINSLIDE: case op_TLSPINSLIDEs: case op_TLSPINSLIDEx:
	case op_HTSPINSLIDE: case op_SPINSLIDEa: case op_XSPINSLIDEa:
		mt_spinslide_set_range(slot[0], v[1], v[2]);
		mt_spinslide_set_value(slot[0], n);
		break;
	case op_SPIN: case op_SPINc: case op_XSPIN:
	case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
	case op_SPINa: case op_XSPINa: case op_TSPINa:
		spin_set_range(slot[0], v[1], v[2]);
		gtk_spin_button_set_value(slot[0], n);
		break;
	}
}

void cmd_set4(void **slot, int *v)
{
// !!! Support only what actually used on, and their brethren
	int op = GET_OP(slot);
	if ((op == op_COLOR) || (op == op_TCOLOR))
	{
		cpick_set_colour_previous(slot[0], v[2], v[3]);
		cpick_set_colour(slot[0], v[0], v[1]);
	}
}

void cmd_setlist(void **slot, char *map, int n)
{
// !!! Support only what actually used on, and their brethren
	int op = GET_OP(slot);
	if ((op == op_OPT) || (op == op_XOPT) || (op == op_TOPT) ||
		(op == op_TLOPT) || (op == op_OPTD))
	{
		GList *items = GTK_MENU_SHELL(gtk_option_menu_get_menu(
			GTK_OPTION_MENU(slot[0])))->children;
		int i, j, k;

		for (i = j = 0; items; items = items->next , i++)
		{
			k = i < n ? map[i] : 0; // show/hide
			if (k > 1) j = i; // select
			widget_showhide(GTK_WIDGET(items->data), k);
		}
		gtk_option_menu_set_history(GTK_OPTION_MENU(slot[0]), j);
	}
}

/* Passively query one slot, show where the result went */
void *cmd_read(void **slot, void *ddata)
{
	return (do_query(ddata, origin_slot(slot), 3));
}

void cmd_peekv(void **slot, void *res, int size, int idx)
{
// !!! Support only what actually used on
	int op = GET_OP(slot);
	switch (op)
	{
	case op_FPICKpm: fpick_get_filename(slot[0], res, size, idx); break;
	case op_XPENTRY: case op_TPENTRY: case op_PATH:
	{
		char *s = (char *)gtk_entry_get_text(GTK_ENTRY(slot[0]));
		if (idx == PATH_VALUE) gtkncpy(res, s, size);
		else strncpy0(res, s, size); // PATH_RAW
		break;
	}
	case op_LISTC: case op_LISTCd: case op_LISTCu:
	case op_LISTCS: case op_LISTCX:
	{
		GtkCList *clist = GTK_CLIST(slot[0]);
		/* if (idx == LISTC_ORDER) */
		int i, l = size / sizeof(int);

		for (i = 0; i < l; i++)
			((int *)res)[i] = -1; // nowhere by default
		if (l > clist->rows) l = clist->rows;
		for (i = 0; i < l; i++)
			((int *)res)[(int)gtk_clist_get_row_data(clist, i)] = i;
#if 0 /* Getting raw selection - not needed for now */
		*(int *)res = (clist->selection ? (int)clist->selection->data : 0);
#endif
	}
	}
}

void cmd_setv(void **slot, void *res, int idx)
{
// !!! Support only what actually used on
	int op = GET_OP(slot);
	switch (op)
	{
	case op_FPICKpm: fpick_set_filename(slot[0], res, idx); break;
	case op_NBOOK: case op_SNBOOK:
		gtk_notebook_set_show_tabs(GTK_NOTEBOOK(slot[0]), (int)res);
		break;
	case op_MLABEL: case op_WLABEL: case op_XLABEL: case op_TLLABEL:
		gtk_label_set_text(GTK_LABEL(slot[0]), res);
		break;
	case op_TEXT: set_textarea(slot[0], res); break;
	case op_XENTRY: case op_MLENTRY: case op_TLENTRY:
		gtk_entry_set_text(GTK_ENTRY(slot[0]), res); break;
	case op_XPENTRY: case op_TPENTRY: case op_PATH: case op_PATHs:
		set_path(slot[0], res, idx);
		break;
	case op_LISTCCr: case op_LISTCCHr:
		listcc_reset(GTK_LIST(slot[0]),
			gtk_object_get_user_data(GTK_OBJECT(slot[0])), (int)res);
// !!! May be needed if LISTCC_RESET_ROW gets used to display an added row
//		gtk_widget_queue_resize(slot[0]);
		break;
	case op_LISTC: case op_LISTCd: case op_LISTCu:
	case op_LISTCS: case op_LISTCX:
	{
		GtkCList *clist = GTK_CLIST(slot[0]);
		listc_data *ld = gtk_object_get_user_data(GTK_OBJECT(slot[0]));

		if (idx == LISTC_RESET_ROW)
		{
			gchar *row_text[MAX_COLS];
			int i, row, n = (int)res, ncol = ld->c.ncol;

			// !!! No support for anything but text columns
			listc_collect(row_text, NULL, &ld->c, n);
			row = gtk_clist_find_row_from_data(clist, (gpointer)n);
			for (i = 0; i < ncol; i++) gtk_clist_set_text(clist,
				row, i, row_text[i]);
		}
		else if (idx == LISTC_SORT)
		{
			if (!ld->sort) break;
			*ld->sort = (int)res;
			listc_column_button(clist, -1, ld);
		}
		break;
#if 0 /* Moving raw selection - not needed for now */
		gtk_clist_select_row(slot[0], (int)res, 0);
#endif
	}
	}
}

void cmd_repaint(void **slot)
{
	int op = GET_OP(slot);
	if ((op == op_COLORLIST) || (op == op_COLORLISTN))
	/* Stupid GTK+ does nothing for gtk_widget_queue_draw(allcol_list) */
		gtk_container_foreach(GTK_CONTAINER(slot[0]),
			(GtkCallback)gtk_widget_queue_draw, NULL);
	else gtk_widget_queue_draw(slot[0]);
}

void cmd_scroll(void **slot, int idx)
{
	int op = GET_OP(slot);
	if ((op == op_COLORLIST) || (op == op_COLORLISTN))
		colorlist_scroll_in(slot[0], idx);
}

// !!! GTK-specific for now
void cmd_cursor(void **slot, GdkCursor *cursor)
{
	gdk_window_set_cursor(GTK_WIDGET(slot[0])->window, cursor);
}
