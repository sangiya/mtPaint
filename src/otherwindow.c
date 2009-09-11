/*	otherwindow.c
	Copyright (C) 2004-2006 Mark Tyler and Dmitry Groshev

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

#include <math.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#if GTK_MAJOR_VERSION == 1
	#include <unistd.h>
#endif

#include "global.h"

#include "memory.h"
#include "otherwindow.h"
#include "mygtk.h"
#include "mainwindow.h"
#include "viewer.h"
#include "inifile.h"
#include "canvas.h"
#include "png.h"
#include "quantizer.h"
#include "layer.h"
#include "wu.h"
#include "ani.h"
#include "channels.h"
#include "toolbar.h"
#include "csel.h"


///	NEW IMAGE WINDOW

int new_window_type = 0;
GtkWidget *new_window, *new_radio[4];
GtkWidget *spinbutton_height, *spinbutton_width, *spinbutton_cols;


gint delete_new( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	gtk_widget_destroy(new_window);

	return FALSE;
}

void reset_tools()
{
	float old_zoom = can_zoom;

	notify_unchanged();

	mem_mask_setall(0);		// Clear all mask info
	mem_col_A = 1, mem_col_B = 0;
	mem_col_A24 = mem_pal[mem_col_A];
	mem_col_B24 = mem_pal[mem_col_B];
	tool_pat = 0;
	init_pal();

	can_zoom = -1;
	if ( inifile_get_gboolean("zoomToggle", FALSE) )
		align_size(1);			// Always start at 100%
	else
		align_size(old_zoom);

	pressed_opacity( 255 );		// Set opacity to 100% to start with
	update_menus();
}

void do_new_chores()
{
	reset_tools();
	set_new_filename( _("Untitled") );
	update_all_views();
	gtk_widget_queue_draw(drawing_col_prev);
}

int do_new_one(int nw, int nh, int nc, int nt, int bpp)
{
	int res;

	if ( nt != 1) mem_pal_copy( mem_pal, mem_pal_def );
#ifdef U_GUADALINEX
	else mem_scale_pal( 0, 255,255,255, nc-1, 0,0,0 );
#else
	else mem_scale_pal( 0, 0,0,0, nc-1, 255,255,255 );
#endif

	mtMIN( nw, nw, MAX_WIDTH )
	mtMAX( nw, nw, MIN_WIDTH )
	mtMIN( nh, nh, MAX_HEIGHT )
	mtMAX( nh, nh, MIN_HEIGHT )
	mtMAX( nc, nc, 2 )

	mem_cols = nc;
	res = mem_new( nw, nh, bpp, CMASK_IMAGE );
	if ( res!= 0 )			// Not enough memory!
	{
		memory_errors(1);
	}
	do_new_chores();

	return res;
}

gint create_new( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int nw, nh, nc, nt = 2, i, j=4, bpp = 1, err=0;

	if ( new_window_type == 1 ) j=3;

	for ( i=0; i<j; i++ )
		if ( gtk_toggle_button_get_active(
			&(GTK_RADIO_BUTTON( new_radio[i] )->check_button.toggle_button)
			) ) nt = i;

	gtk_spin_button_update( GTK_SPIN_BUTTON(spinbutton_width) );
	gtk_spin_button_update( GTK_SPIN_BUTTON(spinbutton_height) );	// All needed in GTK+2 for late changes
	gtk_spin_button_update( GTK_SPIN_BUTTON(spinbutton_cols) );

	nw = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(spinbutton_width) );
	nh = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(spinbutton_height) );
	nc = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(spinbutton_cols) );

	if ( nt == 0 ) bpp = 3;

	if ( nt == 3 )		// Grab Screenshot
	{
#if GTK_MAJOR_VERSION == 1
		gdk_window_lower( main_window->window );
		gdk_window_lower( new_window->window );

		gdk_flush();
		while (gtk_events_pending()) gtk_main_iteration();	// Wait for minimize

		sleep(1);			// Wait a second for screen to redraw

		grab_screen();
		do_new_chores();
		notify_changed();

		gdk_window_raise( main_window->window );
#endif
#if GTK_MAJOR_VERSION == 2
		gtk_window_set_transient_for( GTK_WINDOW(new_window), NULL );
		gdk_window_iconify( new_window->window );
		gdk_window_iconify( main_window->window );

		gdk_flush();
		while (gtk_events_pending()) gtk_main_iteration();	// Wait for minimize

		g_usleep(400000);		// Wait 0.4 of a second for screen to redraw

		grab_screen();
		do_new_chores();
		notify_changed();

		gdk_window_deiconify( main_window->window );
		gdk_window_raise( main_window->window );
#endif
	}

	if ( nt < 3 && new_window_type == 0 )		// New image
	{
		err = do_new_one( nw, nh, nc, nt, bpp );

		if ( err>0 )		// System was unable to allocate memory for image, using 8x8 instead
		{
			nw = mem_width;
			nh = mem_height;  
		}

		if ( layers_total>0 ) layers_notify_changed();

		inifile_set_gint32("lastnewWidth", nw );
		inifile_set_gint32("lastnewHeight", nh );
		inifile_set_gint32("lastnewCols", nc );
		inifile_set_gint32("lastnewType", nt );

		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(icon_buttons[PAINT_TOOL_ICON]), TRUE );
			// Set tool to square for new image - easy way to lose a selection marquee
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(icon_buttons[DEFAULT_TOOL_ICON]), TRUE );
	}

	if ( new_window_type == 1 ) layer_new( nw, nh, 3-nt, nc, CMASK_IMAGE);
	else
	{
		gtk_adjustment_value_changed( gtk_scrolled_window_get_hadjustment(
			GTK_SCROLLED_WINDOW(scrolledwindow_canvas) ) );
		gtk_adjustment_value_changed( gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(scrolledwindow_canvas) ) );
		// These 2 are needed to synchronize the scrollbars & image view
	}

	gtk_widget_destroy(new_window);
	
	return FALSE;
}

void generic_new_window(int type)	// 0=New image, 1=New layer
{
	char *rad_txt[] = {_("24 bit RGB"), _("Greyscale"), _("Indexed Palette"), _("Grab Screenshot")},
		*title_txt[] = {_("New Image"), _("New Layer")};
	int w = mem_width, h = mem_height, c = mem_cols, im_type = 3 - mem_img_bpp;
	GSList *group;

	GtkWidget *vbox1, *hbox3;
	GtkWidget *table1;
	GtkWidget *button_create, *button_cancel;

	GtkAccelGroup* ag = gtk_accel_group_new();

	new_window_type = type;

	if ( type == 0 && check_for_changes() == 1 ) return;

	new_window = add_a_window( GTK_WINDOW_TOPLEVEL, title_txt[type], GTK_WIN_POS_CENTER, TRUE );

	vbox1 = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox1);
	gtk_container_add (GTK_CONTAINER (new_window), vbox1);

	table1 = add_a_table( 3, 2, 5, vbox1 );

	if ( type == 0 )
	{
		w = inifile_get_gint32("lastnewWidth", DEFAULT_WIDTH);
		h = inifile_get_gint32("lastnewHeight", DEFAULT_HEIGHT);
		c = inifile_get_gint32("lastnewCols", 256);
		im_type = inifile_get_gint32("lastnewType", 2);
		if ( im_type<0 || im_type>2 ) im_type = 0;
	}

	spin_to_table( table1, &spinbutton_width, 0, 1, 5, w, MIN_WIDTH, MAX_WIDTH );
	spin_to_table( table1, &spinbutton_height, 1, 1, 5, h, MIN_WIDTH, MAX_HEIGHT );
	spin_to_table( table1, &spinbutton_cols, 2, 1, 5, c, 2, 256 );

	add_to_table( _("Width"), table1, 0, 0, 5, GTK_JUSTIFY_LEFT, 0, 0.5 );
	add_to_table( _("Height"), table1, 1, 0, 5, GTK_JUSTIFY_LEFT, 0, 0.5 );
	add_to_table( _("Colours"), table1, 2, 0, 5, GTK_JUSTIFY_LEFT, 0, 0.5 );

	new_radio[0] = add_radio_button( rad_txt[0], NULL,  NULL, vbox1, 0 );
	group = gtk_radio_button_group( GTK_RADIO_BUTTON(new_radio[0]) );
	new_radio[1] = add_radio_button( rad_txt[1], group, NULL, vbox1, 1 );
	new_radio[2] = add_radio_button( rad_txt[2], NULL,  new_radio[1], vbox1, 2 );
	if ( type == 0 )
		new_radio[3] = add_radio_button( rad_txt[3], NULL,  new_radio[1], vbox1, 3 );

	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( new_radio[im_type]), TRUE );

	add_hseparator( vbox1, 200, 10 );

	hbox3 = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox3);
	gtk_box_pack_start (GTK_BOX (vbox1), hbox3, FALSE, TRUE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (hbox3), 5);

	button_cancel = add_a_button(_("Cancel"), 5, hbox3, TRUE);
	gtk_signal_connect_object( GTK_OBJECT(button_cancel), "clicked",
			GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(new_window));
	gtk_widget_add_accelerator (button_cancel, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);

	button_create = add_a_button(_("Create"), 5, hbox3, TRUE);
	gtk_signal_connect_object( GTK_OBJECT(button_create), "clicked",
			GTK_SIGNAL_FUNC(create_new), GTK_OBJECT(new_window));
	gtk_widget_add_accelerator (button_create, "clicked", ag, GDK_Return, 0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator (button_create, "clicked", ag, GDK_KP_Enter, 0, (GtkAccelFlags) 0);

	gtk_signal_connect_object (GTK_OBJECT (new_window), "delete_event",
		GTK_SIGNAL_FUNC (delete_new), NULL);

	gtk_window_set_transient_for( GTK_WINDOW(new_window), GTK_WINDOW(main_window) );
	gtk_widget_show (new_window);
	gtk_window_add_accel_group(GTK_WINDOW (new_window), ag);
}

void pressed_new( GtkMenuItem *menu_item, gpointer user_data )
{
	generic_new_window(0);
}


///	PATTERN & BRUSH CHOOSER WINDOW

static GtkWidget *pat_window, *draw_pat;
static int pat_brush;
static unsigned char *mem_patch = NULL;


static gint delete_pat( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	if ( pat_brush == 0 )
	{
		if ( mem_patch != NULL ) free(mem_patch);
		mem_patch = NULL;
	}
	gtk_widget_destroy(pat_window);

	return FALSE;
}

static gint key_pat( GtkWidget *widget, GdkEventKey *event )
{
	if ( event->keyval!=65505 && event->keyval!=65507 ) delete_pat( widget, NULL, NULL );
		// Xine sends 6550x key values so don't delete on this

	return FALSE;
}

static gint click_pat( GtkWidget *widget, GdkEventButton *event )
{
	int pat_no = 0, mx, my;

	mx = event->x;
	my = event->y;

	pat_no = mx / (PATCH_WIDTH/9) + 9*( my / (PATCH_HEIGHT/9) );
	mtMAX(pat_no, pat_no, 0)
	mtMIN(pat_no, pat_no, 80)

	if ( event->button == 1 )
	{
		if ( pat_brush == 0 )
		{
			tool_pat = pat_no;
			mem_pat_update();				// Update memory
			gtk_widget_queue_draw( drawing_col_prev );	// Update widget
			if ( marq_status >= MARQUEE_PASTE && text_paste )
			{
				render_text( drawing_col_prev );
				check_marquee();
				gtk_widget_queue_draw( drawing_canvas );
			}
		}
		else
		{
			mem_set_brush(pat_no);
			brush_tool_type = tool_type;
			toolbar_update_settings();	// Update spin buttons
			gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(icon_buttons[PAINT_TOOL_ICON]),
					TRUE );			// Update toolbar
			set_cursor();
		}
	}

	return FALSE;
}

static gint expose_pat( GtkWidget *widget, GdkEventExpose *event )
{
	gdk_draw_rgb_image( draw_pat->window, draw_pat->style->black_gc,
				event->area.x, event->area.y, event->area.width, event->area.height,
				GDK_RGB_DITHER_NONE,
				mem_patch + 3*( event->area.x + PATCH_WIDTH*event->area.y ),
				PATCH_WIDTH*3
				);
	return FALSE;
}

void choose_pattern(int typ)			// Bring up pattern chooser (0) or brush (1)
{
	int pattern, pixel, r, g, b, row, column, sx, sy, ex, ey;

	pat_brush = typ;

	if ( typ == 0 )
	{
		mem_patch = grab_memory( 3*PATCH_WIDTH*PATCH_HEIGHT, 0 );

		for ( pattern = 0; pattern < 81; pattern++ )
		{
			sy = 2 + (pattern / 9) * 36;		// Start y pixel on main image
			sx = 2 + (pattern % 9) * 36;		// Start x pixel on main image
			for ( column = 0; column < 8; column++ )
			{
				for ( row = 0; row < 8; row++ )
				{
					pixel = mem_patterns[pattern][column][row];
					if ( pixel == 1 )
					{
						r = mem_col_A24.red;
						g = mem_col_A24.green;
						b = mem_col_A24.blue;
					}
					else
					{
						r = mem_col_B24.red;
						g = mem_col_B24.green;
						b = mem_col_B24.blue;
					}
					for ( ey=0; ey<4; ey++ )
					{
					 for ( ex=0; ex<4; ex++ )
					 {
					  mem_patch[ 0+3*( sx+row+8*ex + PATCH_WIDTH*(sy+column+8*ey) ) ] = r;
					  mem_patch[ 1+3*( sx+row+8*ex + PATCH_WIDTH*(sy+column+8*ey) ) ] = g;
					  mem_patch[ 2+3*( sx+row+8*ex + PATCH_WIDTH*(sy+column+8*ey) ) ] = b;
					 }
					}
				}
			}
		}
	}
	else
	{
		mem_patch = mem_brushes;
	}

	pat_window = add_a_window( GTK_WINDOW_POPUP, _("Pattern Chooser"), GTK_WIN_POS_MOUSE, TRUE );
	gtk_container_set_border_width (GTK_CONTAINER (pat_window), 4);

	draw_pat = gtk_drawing_area_new ();
	gtk_widget_set_usize( draw_pat, PATCH_WIDTH, PATCH_HEIGHT );
	gtk_container_add (GTK_CONTAINER (pat_window), draw_pat);
	gtk_widget_show( draw_pat );
	gtk_signal_connect_object( GTK_OBJECT(draw_pat), "expose_event",
		GTK_SIGNAL_FUNC (expose_pat), GTK_OBJECT(draw_pat) );
	gtk_signal_connect_object( GTK_OBJECT(draw_pat), "button_press_event",
		GTK_SIGNAL_FUNC (click_pat), GTK_OBJECT(draw_pat) );
	gtk_signal_connect_object( GTK_OBJECT(draw_pat), "button_release_event",
		GTK_SIGNAL_FUNC (delete_pat), NULL );
	gtk_signal_connect_object (GTK_OBJECT (pat_window), "key_press_event",
		GTK_SIGNAL_FUNC (key_pat), NULL);
	gtk_widget_set_events (draw_pat, GDK_ALL_EVENTS_MASK);

	gtk_widget_show (pat_window);
}



///	ADD COLOURS TO PALETTE WINDOW


GtkWidget *add_col_window;
GtkWidget *spinbutton_col_add;


gint delete_col_add( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	gtk_widget_destroy(add_col_window);

	return FALSE;
}

gint click_col_add_ok( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int i, to_add;

	gtk_spin_button_update( GTK_SPIN_BUTTON(spinbutton_col_add) );
	to_add = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(spinbutton_col_add) );

	if ( to_add != mem_cols )
	{
		spot_undo(UNDO_PAL);

		if ( to_add>mem_cols )
			for ( i=mem_cols; i<to_add; i++ )
			{
				mem_pal[i].red = 0;
				mem_pal[i].green = 0;
				mem_pal[i].blue = 0;
			}

		mem_cols = to_add;
		if ( mem_img_bpp == 1 )
		{
			if ( mem_col_A >= mem_cols ) mem_col_A = 0;
			if ( mem_col_B >= mem_cols ) mem_col_B = 0;
		}
		init_pal();
	}

	gtk_widget_destroy(add_col_window);

	return FALSE;
}


void pressed_add_cols( GtkMenuItem *menu_item, gpointer user_data )
{
	GtkWidget *vbox5, *hbox6;
	GtkWidget *button_cancel, *button_ok;

	GtkAccelGroup* ag = gtk_accel_group_new();

	add_col_window = add_a_window( GTK_WINDOW_TOPLEVEL, _("Set Palette Size"),
		GTK_WIN_POS_CENTER, TRUE );

	gtk_widget_set_usize (add_col_window, 320, -2);

	vbox5 = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox5);
	gtk_container_add (GTK_CONTAINER (add_col_window), vbox5);

	add_hseparator( vbox5, -2, 10 );

	spinbutton_col_add = add_a_spin( 256, 2, 256 );
	gtk_box_pack_start (GTK_BOX (vbox5), spinbutton_col_add, FALSE, FALSE, 5);

	add_hseparator( vbox5, -2, 10 );

	hbox6 = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox6);
	gtk_box_pack_start (GTK_BOX (vbox5), hbox6, FALSE, FALSE, 0);

	button_cancel = add_a_button(_("Cancel"), 5, hbox6, TRUE);
	gtk_signal_connect(GTK_OBJECT(button_cancel), "clicked", GTK_SIGNAL_FUNC(delete_col_add), NULL);
	gtk_signal_connect_object (GTK_OBJECT (add_col_window), "delete_event",
		GTK_SIGNAL_FUNC (delete_col_add), NULL);
	gtk_widget_add_accelerator (button_cancel, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);

	button_ok = add_a_button(_("OK"), 5, hbox6, TRUE);
	gtk_signal_connect(GTK_OBJECT(button_ok), "clicked", GTK_SIGNAL_FUNC(click_col_add_ok), NULL);
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_Return, 0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_KP_Enter, 0, (GtkAccelFlags) 0);

	gtk_widget_show (add_col_window);
	gtk_window_add_accel_group(GTK_WINDOW (add_col_window), ag);
}


///	CREATE PALETTE SCALE

void pressed_create_pscale( GtkMenuItem *menu_item, gpointer user_data )
{
	int i = mem_col_A, j = mem_col_B;

	mtMAX( i, i, 0 )
	mtMAX( j, j, 0 )
	mtMIN( i, i, mem_cols-1 )
	mtMIN( j, j, mem_cols-1 )

	if ( abs(i-j)>1 )		// Only do this if we have something to do
	{
		spot_undo(UNDO_PAL);

		mem_scale_pal( i, mem_col_A24.red, mem_col_A24.green, mem_col_A24.blue,
			j, mem_col_B24.red, mem_col_B24.green, mem_col_B24.blue );

		init_pal();
		update_all_views();
		gtk_widget_queue_draw( drawing_col_prev );
	}
}


void pal_refresher()
{
	update_all_views();
	init_pal();
	gtk_widget_queue_draw(drawing_col_prev);
}


/* Generic code to handle UI needs of common image transform tasks */

GtkWidget *filter_win, *filter_cont;
filter_hook filter_func;
gpointer filter_data;

void run_filter(GtkButton *button, gpointer user_data)
{
	if (filter_func(filter_cont, filter_data))
		gtk_widget_destroy(filter_win);
	update_all_views();
}

void filter_window(gchar *title, GtkWidget *content, filter_hook filt, gpointer fdata)
{
	GtkWidget *hbox7, *button_cancel, *button_apply, *vbox6;
	GtkAccelGroup* ag = gtk_accel_group_new();

	filter_cont = content;
	filter_func = filt;
	filter_data = fdata;
	filter_win = add_a_window(GTK_WINDOW_TOPLEVEL, title, GTK_WIN_POS_CENTER, TRUE);
	gtk_widget_set_usize(filter_win, 300, -2);

	vbox6 = gtk_vbox_new(FALSE, 0);
	gtk_widget_show(vbox6);
	gtk_container_add(GTK_CONTAINER(filter_win), vbox6);

	add_hseparator(vbox6, -2, 10);

	gtk_box_pack_start(GTK_BOX(vbox6), content, FALSE, FALSE, 5);

	add_hseparator(vbox6, -2, 10);

	hbox7 = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox7);
	gtk_box_pack_start(GTK_BOX(vbox6), hbox7, FALSE, FALSE, 0);

	button_cancel = add_a_button(_("Cancel"), 5, hbox7, TRUE);
	gtk_signal_connect_object(GTK_OBJECT(button_cancel), "clicked",
		GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(filter_win));
	gtk_signal_connect(GTK_OBJECT(filter_win), "delete_event",
		GTK_SIGNAL_FUNC(gtk_widget_destroy), NULL);
	gtk_widget_add_accelerator(button_cancel, "clicked", ag, GDK_Escape,
		0, (GtkAccelFlags) 0);

	button_apply = add_a_button(_("Apply"), 5, hbox7, TRUE);
	gtk_signal_connect(GTK_OBJECT(button_apply), "clicked",
		GTK_SIGNAL_FUNC(run_filter), NULL);
	gtk_widget_add_accelerator(button_apply, "clicked", ag, GDK_Return,
		0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator(button_apply, "clicked", ag, GDK_KP_Enter,
		0, (GtkAccelFlags) 0);

	gtk_widget_show(filter_win);
	gtk_window_add_accel_group(GTK_WINDOW(filter_win), ag);
}

///	BACTERIA EFFECT

int do_bacteria(GtkWidget *spin, gpointer fdata)
{
	int i;

	gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
	i = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
	spot_undo(UNDO_FILT);
	mem_bacteria(i);

	return FALSE;
}

void pressed_bacteria(GtkMenuItem *menu_item, gpointer user_data)
{
	GtkWidget *spin = add_a_spin(10, 1, 100);
	filter_window(_("Bacteria Effect"), spin, do_bacteria, NULL);
}


///	SORT PALETTE COLOURS

GtkWidget *spal_window, *spal_spins[2], *spal_radio[8];


gint click_spal_apply( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int i, type = 2, index1 = 0, index2 = 1;
	gboolean reverse;

	for ( i=0; i<8; i++ )
	{
		if ( i!=3 )
			if ( gtk_toggle_button_get_active(
				&(GTK_RADIO_BUTTON( spal_radio[i] )->check_button.toggle_button)
					) ) type = i;
	}
	inifile_set_gint32("lastspalType", type );
	if ( type >=3 ) type--;

	reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(spal_radio[3]));
	inifile_set_gboolean( "palrevSort", reverse );

	gtk_spin_button_update( GTK_SPIN_BUTTON(spal_spins[0]) );
	gtk_spin_button_update( GTK_SPIN_BUTTON(spal_spins[1]) );

	index1 = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(spal_spins[0]) );
	index2 = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(spal_spins[1]) );

	if ( index1 == index2 ) return FALSE;

	spot_undo(UNDO_XPAL);
	mem_pal_sort(type, index1, index2, reverse);
	init_pal();
	update_all_views();
	gtk_widget_queue_draw( drawing_col_prev );

	return FALSE;
}

gint click_spal_ok( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	click_spal_apply( NULL, NULL, NULL );
	gtk_widget_destroy(spal_window);

	return FALSE;
}

void pressed_sort_pal( GtkMenuItem *menu_item, gpointer user_data )
{
	char *rad_txt[] = {_("Hue"), _("Saturation"), _("Luminance"), "",
				_("Red"), _("Green"), _("Blue"), _("Frequency") };
	int i;

	GSList *group;
	GtkWidget *vbox1, *hbox3, *hbox, *vbox[2], *table1, *button;
	GtkAccelGroup* ag = gtk_accel_group_new();

	spal_window = add_a_window( GTK_WINDOW_TOPLEVEL, _("Sort Palette Colours"),
		GTK_WIN_POS_CENTER, TRUE );

	vbox1 = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox1);
	gtk_container_add (GTK_CONTAINER (spal_window), vbox1);

	table1 = add_a_table( 2, 2, 5, vbox1 );

	spin_to_table( table1, &spal_spins[0], 0, 1, 5, 0, 0, mem_cols-1 );
	spin_to_table( table1, &spal_spins[1], 1, 1, 5, mem_cols-1, 0, mem_cols-1 );

	add_to_table( _("Start Index"), table1, 0, 0, 5, GTK_JUSTIFY_LEFT, 0, 0.5 );
	add_to_table( _("End Index"), table1, 1, 0, 5, GTK_JUSTIFY_LEFT, 0, 0.5 );

	hbox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox);
	gtk_box_pack_start (GTK_BOX (vbox1), hbox, FALSE, TRUE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (hbox), 5);

	for ( i=0; i<2; i++ )
	{
		vbox[i] = gtk_vbox_new (FALSE, 0);
		gtk_widget_show (vbox[i]);
		gtk_container_add (GTK_CONTAINER (hbox), vbox[i]);
	}

	spal_radio[0] = add_radio_button( rad_txt[0], NULL,  NULL, vbox[0], 0 );
	group = gtk_radio_button_group( GTK_RADIO_BUTTON(spal_radio[0]) );
	spal_radio[1] = add_radio_button( rad_txt[1], group, NULL, vbox[0], 1 );
	spal_radio[2] = add_radio_button( rad_txt[2], NULL,  spal_radio[1], vbox[0], 2 );

	spal_radio[3] = add_a_toggle( _("Reverse Order"), vbox[0],
		inifile_get_gboolean("palrevSort", FALSE) );

	for ( i=4; i<8; i++ )
		spal_radio[i] = add_radio_button( rad_txt[i], NULL,  spal_radio[1], vbox[1], i );

	i = inifile_get_gint32("lastspalType", 2);
	if ( mem_img_bpp == 3 )
	{
		if ( i == 7 ) i--;
		gtk_widget_set_sensitive( spal_radio[7], FALSE );
	}
	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(spal_radio[i]), TRUE );


	add_hseparator( vbox1, 200, 10 );

	hbox3 = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox3);
	gtk_box_pack_start (GTK_BOX (vbox1), hbox3, FALSE, TRUE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (hbox3), 5);

	button = add_a_button(_("Cancel"), 5, hbox3, TRUE);
	gtk_signal_connect_object( GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(spal_window));
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);

	button = add_a_button(_("Apply"), 5, hbox3, TRUE);
	gtk_signal_connect_object( GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(click_spal_apply), GTK_OBJECT(spal_window));

	button = add_a_button(_("OK"), 5, hbox3, TRUE);
	gtk_signal_connect_object( GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(click_spal_ok), GTK_OBJECT(spal_window));
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_Return, 0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_KP_Enter, 0, (GtkAccelFlags) 0);

	gtk_widget_show (spal_window);
	gtk_window_add_accel_group(GTK_WINDOW (spal_window), ag);
}


///	BRIGHTNESS-CONTRAST-SATURATION WINDOW

#define BRCOSA_ITEMS 6

static GtkWidget *brcosa_window;
static GtkWidget *brcosa_scales[BRCOSA_ITEMS], *brcosa_toggles[6],
		 *brcosa_spins[BRCOSA_ITEMS+2];		// Extra 2 used for palette limits
static GtkWidget *brcosa_buttons[5];

static int brcosa_values[BRCOSA_ITEMS], brcosa_pal_lim[2];
png_color brcosa_pal[256];

static void brcosa_buttons_sensitive() // Set 4 brcosa button as sensitive if the user has assigned changes
{
	int i, vals[] = {0, 0, 0, 8, 100};
	gboolean state = FALSE;

	if ( brcosa_buttons[0] == NULL ) return;

	for ( i=0; i<BRCOSA_ITEMS; i++ ) if ( brcosa_values[i] != vals[i] ) state = TRUE;
	for ( i=2; i<5; i++ ) gtk_widget_set_sensitive( brcosa_buttons[i], state );
}

static void brcosa_update_sliders()
{
	int i;

	for ( i=0; i<BRCOSA_ITEMS; i++ )
		gtk_adjustment_set_value( GTK_HSCALE(brcosa_scales[i])->scale.range.adjustment,
			brcosa_values[i] );
}

static void brcosa_update_spins()
{
	int i;

	for ( i=0; i<BRCOSA_ITEMS; i++ )
	{
		gtk_spin_button_update( GTK_SPIN_BUTTON(brcosa_spins[i]) );
		gtk_spin_button_set_value( GTK_SPIN_BUTTON(brcosa_spins[i]), brcosa_values[i] );
	}
}

static gint brcosa_spin_moved()
{
	int i;

	for ( i=0; i<BRCOSA_ITEMS; i++ )
		brcosa_values[i] = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(brcosa_spins[i]) );

	brcosa_buttons_sensitive();
	brcosa_update_sliders();

	return FALSE;
}

static gint click_brcosa_preview( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int i, p1, p2;
	gboolean do_pal = FALSE;	// RGB palette processing

	mem_pal_copy( mem_pal, brcosa_pal );	// Get back normal palette
	if (mem_img_bpp == 3)
	{
		do_pal = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(brcosa_toggles[4]) );
		if ( !do_pal && widget == brcosa_toggles[4] )
		{
			pal_refresher();			// User has just cleared toggle
		}
	}

	for ( i=0; i<BRCOSA_ITEMS; i++ )
	{
		mem_prev_bcsp[i] = brcosa_values[i];
		if ( i<3 ) mem_brcosa_allow[i] =
			gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(brcosa_toggles[i+1]) );
	}

	if ( mem_img_bpp == 1 || do_pal )
	{
		mtMIN( p1, brcosa_pal_lim[0], brcosa_pal_lim[1] )
		mtMAX( p2, brcosa_pal_lim[0], brcosa_pal_lim[1] )
		transform_pal(mem_pal, brcosa_pal, p1, p2);
		pal_refresher();
	}
	if ( mem_img_bpp == 3 )
	{
		gtk_widget_queue_draw_area( drawing_canvas, margin_main_x, margin_main_y,
			mem_width*can_zoom + 1, mem_height*can_zoom + 1);
	}

	return FALSE;
}

static void brcosa_pal_lim_change()
{
	int i;

	for ( i=0; i<2; i++ )
		brcosa_pal_lim[i] = gtk_spin_button_get_value_as_int(
				GTK_SPIN_BUTTON(brcosa_spins[BRCOSA_ITEMS+i]) );

	click_brcosa_preview( NULL, NULL, NULL );	// Update everything
}

static gint click_brcosa_preview_toggle( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	if ( brcosa_buttons[1] == NULL ) return FALSE;		// Traps call during initialisation

	if ( gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(brcosa_toggles[0]) ) )
	{
		click_brcosa_preview( widget, NULL, NULL );
		gtk_widget_hide(brcosa_buttons[1]);
	}
	else 	gtk_widget_show(brcosa_buttons[1]);

	return FALSE;
}

static gint click_brcosa_RGB_toggle( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	if ( gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(brcosa_toggles[5]) ) )
		mem_preview = 1;
	else	mem_preview = 0;

	click_brcosa_preview( widget, NULL, NULL );

	return FALSE;
}

static gint brcosa_slider_moved( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int i;

	for ( i=0; i<BRCOSA_ITEMS; i++ )
		brcosa_values[i] = GTK_HSCALE(brcosa_scales[i])->scale.range.adjustment->value;

	brcosa_buttons_sensitive();

	brcosa_update_spins();

	if ( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(brcosa_toggles[0]) ) )
		click_brcosa_preview( NULL, NULL, NULL );

	return FALSE;
}

static gint delete_brcosa( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	inifile_set_gboolean( "autopreviewToggle",
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(brcosa_toggles[0]) ) );
	gtk_widget_destroy(brcosa_window);

	mem_preview = 0;		// If in RGB mode this is required to disable live preview

	return FALSE;
}

static gint click_brcosa_cancel( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	mem_pal_copy( mem_pal, brcosa_pal );
	pal_refresher();
	delete_brcosa( NULL, NULL, NULL );

	return FALSE;
}

static gint click_brcosa_apply( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	unsigned char *mask, *mask0, *tmp;
	int i;

	mem_pal_copy( mem_pal, brcosa_pal );

	if ( brcosa_values[0] != 0 || brcosa_values[1] != 0 ||
		brcosa_values[2] != 0 || brcosa_values[3] != 8 || brcosa_values[4] != 100 ||
		brcosa_values[5] != 0 )
	{
		spot_undo(UNDO_COL);

		click_brcosa_preview( NULL, NULL, NULL );
		update_all_views();
		if ( mem_img_bpp == 3 && mem_preview == 1 )	// Only do if toggle set
		{
			mask = malloc(mem_width);
			if (mask)
			{
				mask0 = mem_img[CHN_MASK];
				tmp = mem_img[CHN_IMAGE];
				for (i = 0; i < mem_height; i++)
				{
					prep_mask(0, 1, mem_width, mask, mask0, tmp);
					do_transform(0, 1, mem_width, mask, tmp, tmp);
					if (mask0) mask0 += mem_width;
					tmp += mem_width * 3;
				}
				free(mask);
			}
		}

		if ( mem_img_bpp == 1 ) mem_pal_copy( brcosa_pal, mem_pal );
		else
		{
			if ( gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(brcosa_toggles[4]))
				&& (widget != NULL)	// Don't do this when clicking OK
				)
			{
				mem_pal_copy( brcosa_pal, mem_pal );
				click_brcosa_preview(NULL, NULL, NULL);
			}
		}	// Update palette values in RGB/indexed mode as required
	}

	return FALSE;
}

static void click_brcosa_show_toggle( GtkWidget *widget, GtkWidget *data )
{
	gboolean toggle = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

	inifile_set_gboolean( "transcol_show", toggle );
	if ( toggle ) gtk_widget_show( GTK_WIDGET(data) );
	else
	{
		gtk_widget_hide( GTK_WIDGET(data) );
	}
}

/*
static void click_brcosa_store()
{
}
*/

static gint click_brcosa_ok()
{
	click_brcosa_apply( NULL, NULL, NULL );
	delete_brcosa( NULL, NULL, NULL );

	return FALSE;
}

static gint click_brcosa_reset()
{
	int i;

	mem_pal_copy( mem_pal, brcosa_pal );

	for ( i=0; i<BRCOSA_ITEMS; i++ )
	{
		if (i<3 || i==5) brcosa_values[i] = 0;
		if (i==3) brcosa_values[i] = 8;
		if (i==4) brcosa_values[i] = 100;
		gtk_adjustment_set_value( GTK_HSCALE(brcosa_scales[i])->scale.range.adjustment,
			brcosa_values[i] );
		gtk_spin_button_set_value( GTK_SPIN_BUTTON(brcosa_spins[i]), brcosa_values[i] );
		gtk_spin_button_update( GTK_SPIN_BUTTON(brcosa_spins[i]) );
	}
	pal_refresher();

	return FALSE;
}

void pressed_brcosa( GtkMenuItem *menu_item, gpointer user_data )
{
	GtkWidget *vbox, *vbox5, *table2, *hbox, *label, *button;

	int	mins[] = {-255, -100, -100, 1, 20, -1529},
		maxs[] = {255, 100, 100, 8, 500, 1529},
		vals[] = {0, 0, 0, 8, 100, 0},
		order[] = {1, 2, 3, 5, 0, 4},
		i;
	char	*tog_txt[] = {	_("Auto-Preview"), _("Red"), _("Green"), _("Blue"), _("Palette"),
				_("Image") },
		*tab_txt[] = {	_("Brightness"), _("Contrast"), _("Saturation"), _("Posterize"),
				_("Gamma"), _("Hue") };

	GtkAccelGroup* ag = gtk_accel_group_new();

	mem_pal_copy( brcosa_pal, mem_pal );		// Remember original palette

	for ( i=0; i<BRCOSA_ITEMS; i++ ) mem_prev_bcsp[i] = vals[i];

	for ( i=0; i<4; i++ ) brcosa_buttons[i] = NULL;
			// Enables preview_toggle code to detect an initialisation call

	mem_preview = 1;		// If in RGB mode this is required to enable live preview

	brcosa_window = add_a_window( GTK_WINDOW_TOPLEVEL, _("Transform Colour"),
		GTK_WIN_POS_MOUSE, TRUE );

	gtk_window_set_policy( GTK_WINDOW(brcosa_window), FALSE, FALSE, TRUE );
			// Automatically grow/shrink window

	vbox = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox);
	gtk_container_add (GTK_CONTAINER (brcosa_window), vbox);

	table2 = add_a_table( 4, 3, 10, vbox );

	for ( i=0; i<BRCOSA_ITEMS; i++ )
		add_to_table( tab_txt[i], table2, order[i], 0, 0, GTK_JUSTIFY_LEFT, 0, 0.5);

	for ( i=0; i<BRCOSA_ITEMS; i++ )
	{
		brcosa_values[i] = vals[i];
		brcosa_scales[i] = add_slider2table( vals[i], mins[i], maxs[i],
						table2, order[i], 1, 255, 20 );
		gtk_signal_connect( GTK_OBJECT(GTK_HSCALE(brcosa_scales[i])->scale.range.adjustment),
			"value_changed", GTK_SIGNAL_FUNC(brcosa_slider_moved), NULL);
		spin_to_table( table2, &brcosa_spins[i], order[i], 2, 1, vals[i], mins[i], maxs[i] );

#if GTK_MAJOR_VERSION == 2
		gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(brcosa_spins[i])->entry ),
			"value_changed", GTK_SIGNAL_FUNC(brcosa_spin_moved), NULL);
#else
		gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(brcosa_spins[i])->entry ),
			"changed", GTK_SIGNAL_FUNC(brcosa_spin_moved), NULL);
#endif
	}



///	MIDDLE SECTION

	add_hseparator( vbox, -2, 10 );

	hbox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

	vbox5 = gtk_vbox_new (FALSE, 0);
	if ( inifile_get_gboolean( "transcol_show", FALSE ) ) gtk_widget_show (vbox5);
	gtk_box_pack_start (GTK_BOX (vbox), vbox5, FALSE, FALSE, 0);

	button = add_a_toggle( _("Show Detail"), hbox, inifile_get_gboolean( "transcol_show", FALSE ) );
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(click_brcosa_show_toggle), vbox5);
/*
	button = gtk_button_new_with_label(_("Store Values"));
	gtk_widget_show (button);
	gtk_box_pack_end (GTK_BOX (hbox), button, FALSE, FALSE, 4);

	gtk_signal_connect(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(click_brcosa_store), NULL);
*/

///	OPTIONAL SECTION

	hbox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox);
	gtk_box_pack_start (GTK_BOX (vbox5), hbox, FALSE, FALSE, 0);

	if ( mem_img_bpp == 1 )
	{
		label = gtk_label_new( _("Palette") );
		gtk_box_pack_start( GTK_BOX(hbox), label, FALSE, FALSE, 5 );
		gtk_widget_show( label );
	}
	else
	{
		for ( i=5; i>3; i-- )
		{
			brcosa_toggles[i] = add_a_toggle( tog_txt[i], hbox, TRUE );
			gtk_signal_connect(GTK_OBJECT(brcosa_toggles[i]), "clicked",
				GTK_SIGNAL_FUNC(click_brcosa_RGB_toggle), NULL);
		}
	}
	brcosa_pal_lim[0] = 0;
	brcosa_pal_lim[1] = mem_cols-1;
	brcosa_spins[BRCOSA_ITEMS] = add_a_spin( 0, 0, mem_cols-1 );
	brcosa_spins[BRCOSA_ITEMS+1] = add_a_spin( mem_cols-1, 0, mem_cols-1 );
	gtk_box_pack_start (GTK_BOX (hbox), brcosa_spins[BRCOSA_ITEMS], FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (hbox), brcosa_spins[BRCOSA_ITEMS+1], FALSE, FALSE, 0);

	for ( i=0; i<2; i++ )
	{
#if GTK_MAJOR_VERSION == 2
		gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(brcosa_spins[BRCOSA_ITEMS+i])->entry ),
			"value_changed", GTK_SIGNAL_FUNC(brcosa_pal_lim_change), NULL);
#else
		gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(brcosa_spins[BRCOSA_ITEMS+i])->entry ),
			"changed", GTK_SIGNAL_FUNC(brcosa_pal_lim_change), NULL);
#endif
	}

	hbox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox);
	gtk_box_pack_start (GTK_BOX (vbox5), hbox, FALSE, FALSE, 0);

	for ( i=0; i<4; i++ )
	{
		brcosa_toggles[i] = add_a_toggle( tog_txt[i], hbox, TRUE );
		if ( i == 0 ) gtk_signal_connect(GTK_OBJECT(brcosa_toggles[i]), "clicked",
			GTK_SIGNAL_FUNC(click_brcosa_preview_toggle), NULL);
		if ( i>0 ) gtk_signal_connect(GTK_OBJECT(brcosa_toggles[i]), "clicked",
			GTK_SIGNAL_FUNC(click_brcosa_preview), NULL);
	}

	if ( mem_img_bpp == 3 )
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON( brcosa_toggles[4] ), FALSE);

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON( brcosa_toggles[0] ),
		inifile_get_gboolean("autopreviewToggle", TRUE));



///	BOTTOM AREA

	add_hseparator( vbox, -2, 10 );

	hbox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (hbox), 5);

	button = add_a_button(_("Cancel"), 4, hbox, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(click_brcosa_cancel), NULL);
	gtk_signal_connect_object (GTK_OBJECT (brcosa_window), "delete_event",
		GTK_SIGNAL_FUNC (click_brcosa_cancel), NULL);
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);
	brcosa_buttons[0] = button;

	button = add_a_button(_("Preview"), 4, hbox, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(click_brcosa_preview), NULL);
	brcosa_buttons[1] = button;

	button = add_a_button(_("Reset"), 4, hbox, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(click_brcosa_reset), NULL);
	brcosa_buttons[2] = button;

	button = add_a_button(_("Apply"), 4, hbox, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(click_brcosa_apply), NULL);
	brcosa_buttons[3] = button;

	button = add_a_button(_("OK"), 4, hbox, TRUE);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(click_brcosa_ok), NULL);
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_KP_Enter, 0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator (button, "clicked", ag, GDK_Return, 0, (GtkAccelFlags) 0);
	brcosa_buttons[4] = button;

	gtk_widget_show (brcosa_window);
	gtk_window_add_accel_group(GTK_WINDOW (brcosa_window), ag);

	click_brcosa_preview_toggle( NULL, NULL, NULL );		// Show/hide preview button
	brcosa_buttons_sensitive();					// Disable buttons
	gtk_window_set_transient_for( GTK_WINDOW(brcosa_window), GTK_WINDOW(main_window) );
}


///	RESIZE/RESCALE WINDOWS

GtkWidget *sisca_window, *sisca_table;
GtkWidget *sisca_spins[6], *sisca_toggles[2];
gboolean sisca_scale;


void sisca_off_lim( int spin, int dim )
{
	int nw = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[spin-2]) );
	int min, max, val;
	gboolean state = TRUE;
	GtkAdjustment *adj;

	if ( sisca_scale ) return;			// Only do this if we are resizing

	adj = gtk_spin_button_get_adjustment( GTK_SPIN_BUTTON(sisca_spins[spin]) );
	val = adj -> value;
	if ( nw == dim )
	{
		state = FALSE;
		min = 0;
		max = 0;
		val = 0;
	}
	else
	{
		if ( nw<dim )			// Size is shrinking
		{
			max = 0;
			min = nw - dim;
		}
		else					// Size is expanding
		{
			max = nw - dim;
			min = 0;
		}
	}
	mtMIN( val, val, max )
	mtMAX( val, val, min )

	adj -> lower = min;
	adj -> upper = max;
	adj -> value = val;

	gtk_adjustment_value_changed( adj );
	gtk_adjustment_changed( adj );
	gtk_widget_set_sensitive( sisca_spins[spin], state );
	gtk_spin_button_update( GTK_SPIN_BUTTON(sisca_spins[spin]) );
}

void sisca_reset_offset_y()
{	sisca_off_lim( 3, mem_height ); }

void sisca_reset_offset_x()
{	sisca_off_lim( 2, mem_width ); }

gint sisca_width_moved( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int nw, nh, oh;

	sisca_reset_offset_x();
	if ( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sisca_toggles[0])) )
	{
		nw = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[0]) );
		oh = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[1]) );
		nh = mt_round( nw * ((float) mem_height) / ((float) mem_width) );
		mtMIN( nh, nh, MAX_HEIGHT )
		mtMAX( nh, nh, 1 )
		if ( nh != oh )
		{
			gtk_spin_button_update( GTK_SPIN_BUTTON(sisca_spins[1]) );
			gtk_spin_button_set_value( GTK_SPIN_BUTTON(sisca_spins[1]), nh );
			sisca_reset_offset_y();
		}
	}

	return FALSE;
}

gint sisca_height_moved( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int nw, nh, ow;

	sisca_reset_offset_y();
	if ( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sisca_toggles[0])) )
	{
		ow = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[0]) );
		nh = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[1]) );
		nw = mt_round( nh * ((float) mem_width) / ((float) mem_height) );
		mtMIN( nw, nw, MAX_WIDTH )
		mtMAX( nw, nw, 1 )
		if ( nw != ow )
		{
			gtk_spin_button_update( GTK_SPIN_BUTTON(sisca_spins[0]) );
			gtk_spin_button_set_value( GTK_SPIN_BUTTON(sisca_spins[0]), nw );
			sisca_reset_offset_x();
		}
	}

	return FALSE;
}

static int scale_mode = 7;

static void scale_mode_changed(GtkWidget *widget, gpointer name)
{
	if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) return;
	scale_mode = (int) name;
}

gint click_sisca_cancel( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	gtk_widget_destroy(sisca_window);
	return FALSE;
}

gint click_sisca_ok( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int nw, nh, ox, oy, res = 1, scale_type = 0;

	gtk_spin_button_update( GTK_SPIN_BUTTON(sisca_spins[0]) );
	gtk_spin_button_update( GTK_SPIN_BUTTON(sisca_spins[1]) );

	nw = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[0]) );
	nh = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[1]) );
	if ( nw != mem_width || nh != mem_height )
	{
		// Needed in Windows to stop GTK+ lowering the main window below window underneath
		gtk_window_set_transient_for( GTK_WINDOW(sisca_window), NULL );

		if ( sisca_scale )
		{
			if ( mem_img_bpp == 3 ) scale_type = scale_mode;
			res = mem_image_scale( nw, nh, scale_type );
		}
		else 
		{
			ox = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[2]) );
			oy = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[3]) );
			res = mem_image_resize( nw, nh, ox, oy );
		}
		if ( res == 0 )
		{
			canvas_undo_chores();
			click_sisca_cancel( NULL, NULL, NULL );
		}
		else	memory_errors(res);
	}
	else alert_box(_("Error"), _("New geometry is the same as now - nothing to do."),
			_("OK"), NULL, NULL);

	return FALSE;
}

void memory_errors(int type)
{
	if ( type == 1 )
		alert_box(_("Error"), _("The operating system cannot allocate the memory for this operation."), _("OK"), NULL, NULL);
	if ( type == 2 )
		alert_box(_("Error"), _("You have not allocated enough memory in the Preferences window for this operation."), _("OK"), NULL, NULL);
}

gint click_sisca_centre( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int nw = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[0]) );
	int nh = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(sisca_spins[1]) );

	nw = (nw - mem_width) / 2;
	nh = (nh - mem_height) / 2;

	gtk_spin_button_set_value( GTK_SPIN_BUTTON(sisca_spins[2]), nw );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON(sisca_spins[3]), nh );

	return FALSE;
}

void sisca_init( char *title )
{
	gchar* scale_fnames[] = {
		_("Nearest Neighbour"),
		_("Bilinear / Area Mapping"),
		_("Bicubic"),
		_("Bicubic edged"),
		_("Bicubic better"),
		_("Bicubic sharper"),
		_("Lanczos3"),
		_("Blackman-Harris"),
		NULL
	};

	GtkWidget *button_ok, *button_cancel, *button_centre, *sisca_vbox, *sisca_hbox;
	GtkAccelGroup* ag = gtk_accel_group_new();

	sisca_window = add_a_window( GTK_WINDOW_TOPLEVEL, title, GTK_WIN_POS_CENTER, TRUE );

	sisca_vbox = gtk_vbox_new(FALSE, 0);
	gtk_widget_show(sisca_vbox);
	gtk_container_add(GTK_CONTAINER (sisca_window), sisca_vbox);

	sisca_table = add_a_table(3, 3, 5, sisca_vbox);

	add_to_table( _("Width     "), sisca_table, 0, 1, 0, GTK_JUSTIFY_LEFT, 0, 0 );
	add_to_table( _("Height    "), sisca_table, 0, 2, 0, GTK_JUSTIFY_LEFT, 0, 0 );

	add_to_table( _("Original      "), sisca_table, 1, 0, 0, GTK_JUSTIFY_LEFT, 0, 0.5);
	spin_to_table( sisca_table, &sisca_spins[0], 1, 1, 5, mem_width, mem_width, mem_width );
	spin_to_table( sisca_table, &sisca_spins[1], 1, 2, 5, mem_height, mem_height, mem_height );
	GTK_WIDGET_UNSET_FLAGS (sisca_spins[0], GTK_CAN_FOCUS);
	GTK_WIDGET_UNSET_FLAGS (sisca_spins[1], GTK_CAN_FOCUS);

	add_to_table( _("New"), sisca_table, 2, 0, 0, GTK_JUSTIFY_LEFT, 0, 0.5);
	spin_to_table( sisca_table, &sisca_spins[0], 2, 1, 5, mem_width, 1, MAX_WIDTH );
	spin_to_table( sisca_table, &sisca_spins[1], 2, 2, 5, mem_height, 1, MAX_HEIGHT );

#if GTK_MAJOR_VERSION == 2
	gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(sisca_spins[0])->entry ),
		"value_changed", GTK_SIGNAL_FUNC(sisca_width_moved), NULL);
	gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(sisca_spins[1])->entry ),
		"value_changed", GTK_SIGNAL_FUNC(sisca_height_moved), NULL);
#else
	gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(sisca_spins[0])->entry ),
		"changed", GTK_SIGNAL_FUNC(sisca_width_moved), NULL);
	gtk_signal_connect( GTK_OBJECT( &GTK_SPIN_BUTTON(sisca_spins[1])->entry ),
		"changed", GTK_SIGNAL_FUNC(sisca_height_moved), NULL);
#endif
	// Interesting variation between GTK+1/2 here.  I want to update each of the spinbuttons
	// when either:
	// i)  Up/down button clicked
	// ii) Manual changed followed by a tab keypress
	// MT 19-10-2004

	if ( !sisca_scale )
	{
		add_to_table( _("Offset"), sisca_table, 3, 0, 0, GTK_JUSTIFY_LEFT, 0, 0.5);
		spin_to_table( sisca_table, &sisca_spins[2], 3, 1, 5, 0, 0, 0 );
		spin_to_table( sisca_table, &sisca_spins[3], 3, 2, 5, 0, 0, 0 );

		button_centre = gtk_button_new_with_label(_("Centre"));

		gtk_widget_show(button_centre);
		gtk_table_attach (GTK_TABLE (sisca_table), button_centre, 0, 1, 4, 5,
			(GtkAttachOptions) (GTK_FILL),
			(GtkAttachOptions) (0), 5, 5);
		gtk_signal_connect(GTK_OBJECT(button_centre), "clicked",
			GTK_SIGNAL_FUNC(click_sisca_centre), NULL);
	}
	add_hseparator( sisca_vbox, -2, 10 );

	sisca_toggles[0] = add_a_toggle( _("Fix Aspect Ratio"), sisca_vbox, TRUE );
	gtk_signal_connect(GTK_OBJECT(sisca_toggles[0]), "clicked",
		GTK_SIGNAL_FUNC(sisca_width_moved), NULL);
	if ( mem_img_bpp == 3 && sisca_scale )
	{
		GtkWidget *btn = NULL;
		int i;

		add_hseparator( sisca_vbox, -2, 10 );
		for (i = 0; scale_fnames[i]; i++)
		{
			btn = add_radio_button(scale_fnames[i], NULL, btn,
				sisca_vbox, i + 1);
			if (scale_mode == i)
				gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);
			gtk_signal_connect(GTK_OBJECT(btn), "toggled",
				GTK_SIGNAL_FUNC(scale_mode_changed),
				(gpointer)(i));
		}
		if (scale_mode >= i) scale_mode = 0;
	}

	add_hseparator( sisca_vbox, -2, 10 );

	sisca_hbox = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (sisca_hbox);
	gtk_box_pack_start (GTK_BOX (sisca_vbox), sisca_hbox, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (sisca_hbox), 5);

	button_cancel = add_a_button( _("Cancel"), 4, sisca_hbox, TRUE );
	gtk_signal_connect(GTK_OBJECT(button_cancel), "clicked",
		GTK_SIGNAL_FUNC(click_sisca_cancel), NULL);
	gtk_signal_connect_object (GTK_OBJECT (sisca_window), "delete_event",
		GTK_SIGNAL_FUNC (click_sisca_cancel), NULL);
	gtk_widget_add_accelerator (button_cancel, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);

	button_ok = add_a_button( _("OK"), 4, sisca_hbox, TRUE );
	gtk_signal_connect(GTK_OBJECT(button_ok), "clicked",
		GTK_SIGNAL_FUNC(click_sisca_ok), NULL);
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_Return, 0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_KP_Enter, 0, (GtkAccelFlags) 0);

	gtk_window_set_transient_for( GTK_WINDOW(sisca_window), GTK_WINDOW(main_window) );
	gtk_widget_show (sisca_window);
	gtk_window_add_accel_group(GTK_WINDOW (sisca_window), ag);

	sisca_reset_offset_x();
	sisca_reset_offset_y();
}

void pressed_scale( GtkMenuItem *menu_item, gpointer user_data )
{
	sisca_scale = TRUE;
	sisca_init(_("Scale Canvas"));
}

void pressed_size( GtkMenuItem *menu_item, gpointer user_data )
{
	sisca_scale = FALSE;
	sisca_init(_("Resize Canvas"));
}


///	PALETTE EDITOR WINDOW

typedef struct {
	guint16 r, g, b, a;
} RGBA16;

static GtkWidget *allcol_window, *allcol_list;
static RGBA16 *ctable;
static int allcol_idx, cs_old_overlay[NUM_CHANNELS][4];
static colour_hook allcol_hook;


static void allcol_ok(colour_hook chook)
{
	chook(2);
	gtk_widget_destroy(allcol_window);
	free(ctable);
}

static void allcol_preview(colour_hook chook)
{
	chook(1);
}

static gboolean allcol_cancel(colour_hook chook)
{
	chook(0);
	gtk_widget_destroy(allcol_window);
	free(ctable);
	return (FALSE);
}

static void color_refresh()
{
	gtk_list_select_item(GTK_LIST(allcol_list), allcol_idx);

	/* Stupid GTK+ does nothing for gtk_widget_queue_draw(allcol_list) */
	gtk_container_foreach(GTK_CONTAINER(allcol_list),
		(GtkCallback)gtk_widget_queue_draw, NULL);
}

static gboolean color_expose( GtkWidget *widget, GdkEventExpose *event, gpointer user_data )
{
	RGBA16 *cc = user_data;
	unsigned char r = cc->r / 257, g = cc->g / 257, b = cc->b / 257, *rgb;
	int x = event->area.x, y = event->area.y, w = event->area.width, h = event->area.height;
	int i, j = w * h * 3;

	rgb = malloc(j);
	if (rgb)
	{
		for (i = 0; i < j; i += 3)
		{
			rgb[i] = r;
			rgb[i + 1] = g;
			rgb[i + 2] = b;
		}
		gdk_draw_rgb_image( widget->window, widget->style->black_gc, x, y, w, h, GDK_RGB_DITHER_NONE, rgb, w*3 );
		free(rgb);
	}

	return FALSE;
}


static void color_set( GtkColorSelection *selection, gpointer user_data )
{
	gdouble color[4];
	GtkWidget *widget;
	RGBA16 *cc;
	GdkColor c;

	gtk_color_selection_get_color( selection, color );
	widget = GTK_WIDGET(gtk_object_get_user_data(GTK_OBJECT(selection)));

	cc = gtk_object_get_user_data(GTK_OBJECT(widget));
	cc->r = rint(color[0] * 65535.0);
	cc->g = rint(color[1] * 65535.0);
	cc->b = rint(color[2] * 65535.0);
	cc->a = rint(color[3] * 65535.0);
	c.pixel = 0;
	c.red   = cc->r;
	c.green = cc->g;
	c.blue  = cc->b;
	gdk_colormap_alloc_color( gdk_colormap_get_system(), &c, FALSE, TRUE );
	gtk_widget_queue_draw(widget);
	allcol_hook(4);
}

static void color_select( GtkList *list, GtkWidget *widget, gpointer user_data )
{
	GtkColorSelection *cs = GTK_COLOR_SELECTION(user_data);
	RGBA16 *cc = gtk_object_get_user_data(GTK_OBJECT(widget));
	gdouble color[4];

	gtk_object_set_user_data( GTK_OBJECT(cs), widget );
	color[0] = ((gdouble)(cc->r)) / 65535.0;
	color[1] = ((gdouble)(cc->g)) / 65535.0;
	color[2] = ((gdouble)(cc->b)) / 65535.0;
	color[3] = ((gdouble)(cc->a)) / 65535.0;

	gtk_signal_disconnect_by_func(GTK_OBJECT(cs), GTK_SIGNAL_FUNC(color_set), NULL);

	gtk_color_selection_set_color( cs, color );

#if GTK_MAJOR_VERSION == 1
	gtk_color_selection_set_color( cs, color);
#endif
#if GTK_MAJOR_VERSION == 2
	GdkColor c = {0, cc->r, cc->g, cc->b};
	gtk_color_selection_set_previous_color(cs, &c);
	gtk_color_selection_set_previous_alpha(cs, cc->a);
#endif
	allcol_idx = cc - ctable;

	gtk_signal_connect( GTK_OBJECT(cs), "color_changed", GTK_SIGNAL_FUNC(color_set), NULL );
	allcol_hook(3);
}

void colour_window(GtkWidget *win, GtkWidget *extbox, int cnt, char **cnames,
	int alpha, colour_hook chook)
{
	GtkWidget *vbox, *hbox, *hbut, *button_ok, *button_preview, *button_cancel;
	GtkWidget *col_list, *l_item, *hbox2, *label, *drw, *swindow, *viewport;
	GtkWidget *cs;
	char txt[64], *tmp = txt;
	int i;

	GtkAccelGroup* ag = gtk_accel_group_new();

	cs = gtk_color_selection_new();

#if GTK_MAJOR_VERSION == 1
	gtk_color_selection_set_opacity(GTK_COLOR_SELECTION(cs), alpha);
#endif
#if GTK_MAJOR_VERSION == 2
	gtk_color_selection_set_has_opacity_control(GTK_COLOR_SELECTION(cs), alpha);

	gtk_color_selection_set_has_palette (GTK_COLOR_SELECTION(cs), TRUE);
#endif
	gtk_signal_connect( GTK_OBJECT(cs), "color_changed", GTK_SIGNAL_FUNC(color_set), NULL );

	allcol_window = win;
	allcol_hook = chook;

	gtk_signal_connect_object(GTK_OBJECT(allcol_window), "delete_event",
		GTK_SIGNAL_FUNC(allcol_cancel), (gpointer)chook);

	vbox = gtk_vbox_new( FALSE, 5 );
	gtk_widget_show( vbox );
	gtk_container_set_border_width( GTK_CONTAINER(vbox), 5 );
	gtk_container_add( GTK_CONTAINER(allcol_window), vbox );

	hbox = gtk_hbox_new( FALSE, 10 );
	gtk_widget_show( hbox );
	gtk_box_pack_start( GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

	swindow = gtk_scrolled_window_new (NULL, NULL);
	gtk_widget_show (swindow);
	gtk_box_pack_start (GTK_BOX (hbox), swindow, FALSE, TRUE, 0);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (swindow),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	viewport = gtk_viewport_new (NULL, NULL);
	gtk_widget_show(viewport);
	gtk_container_add (GTK_CONTAINER (swindow), viewport);

	allcol_idx = 0;
	allcol_list = col_list = gtk_list_new();
	gtk_signal_connect(GTK_OBJECT(col_list), "select_child",
		GTK_SIGNAL_FUNC(color_select), cs);
	gtk_list_set_selection_mode( GTK_LIST(col_list), GTK_SELECTION_BROWSE );
	gtk_container_add ( GTK_CONTAINER(viewport), col_list );
	gtk_widget_show( col_list );

	for (i = 0; i < cnt; i++)
	{
		l_item = gtk_list_item_new();
		gtk_object_set_user_data( GTK_OBJECT(l_item), (gpointer)(&ctable[i]));
		gtk_container_add( GTK_CONTAINER(col_list), l_item );
		gtk_widget_show( l_item );

		hbox2 = gtk_hbox_new( FALSE, 3 );
		gtk_widget_show( hbox2 );
		gtk_container_set_border_width( GTK_CONTAINER(hbox2), 3 );
		gtk_container_add( GTK_CONTAINER(l_item), hbox2 );

		drw = gtk_drawing_area_new();
		gtk_drawing_area_size( GTK_DRAWING_AREA(drw), 20, 20 );
		gtk_signal_connect(GTK_OBJECT(drw), "expose_event",
			GTK_SIGNAL_FUNC(color_expose), (gpointer)(&ctable[i]));
		gtk_box_pack_start( GTK_BOX(hbox2), drw, FALSE, FALSE, 0 );
		gtk_widget_show( drw );

		if (cnames) tmp = cnames[i];
		else sprintf(txt, "%i", i);
		label = gtk_label_new(tmp);
		gtk_widget_show( label );
		gtk_misc_set_alignment( GTK_MISC(label), 0.0, 1.0 );
		gtk_box_pack_start( GTK_BOX(hbox2), label, TRUE, TRUE, 0 );
	}

	gtk_box_pack_start( GTK_BOX(hbox), cs, TRUE, TRUE, 0 );

	if (extbox) gtk_box_pack_start(GTK_BOX(vbox), extbox, FALSE, FALSE, 0);

	hbut = gtk_hbox_new(FALSE, 3);
	gtk_widget_show( hbut );
	gtk_box_pack_start( GTK_BOX(vbox), hbut, FALSE, FALSE, 0 );

	button_ok = gtk_button_new_with_label(_("OK"));
	gtk_widget_show( button_ok );
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_KP_Enter, 0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_Return, 0, (GtkAccelFlags) 0);
	gtk_signal_connect_object(GTK_OBJECT(button_ok), "clicked",
		GTK_SIGNAL_FUNC(allcol_ok), (gpointer)chook);
	gtk_box_pack_end( GTK_BOX(hbut), button_ok, FALSE, FALSE, 5 );
	gtk_widget_set_usize(button_ok, 80, -2);

	button_preview = gtk_button_new_with_label(_("Preview"));
	gtk_widget_show( button_preview );
	gtk_signal_connect_object(GTK_OBJECT(button_preview), "clicked",
		GTK_SIGNAL_FUNC(allcol_preview), (gpointer)chook);
	gtk_box_pack_end( GTK_BOX(hbut), button_preview, FALSE, FALSE, 5 );
	gtk_widget_set_usize(button_preview, 80, -2);

	button_cancel = gtk_button_new_with_label(_("Cancel"));
	gtk_widget_show( button_cancel );
	gtk_widget_add_accelerator (button_cancel, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);
	gtk_signal_connect_object(GTK_OBJECT(button_cancel), "clicked",
		GTK_SIGNAL_FUNC(allcol_cancel), (gpointer)chook);
	gtk_box_pack_end( GTK_BOX(hbut), button_cancel, FALSE, FALSE, 5 );
	gtk_widget_set_usize(button_cancel, 80, -2);

	gtk_widget_show( cs );
	gtk_window_set_transient_for( GTK_WINDOW(allcol_window), GTK_WINDOW(main_window) );
	gtk_widget_show( allcol_window );
	gtk_window_add_accel_group( GTK_WINDOW(allcol_window), ag );

#if GTK_MAJOR_VERSION == 1
	while (gtk_events_pending()) gtk_main_iteration();
	gtk_list_select_item( GTK_LIST(col_list), 0 );
		// grubby hack needed to start with proper opacity in GTK+1
#endif
}

static void do_allcol()
{
	int i;

	for (i = 0; i < mem_cols; i++)
	{
		mem_pal[i].red = (ctable[i].r + 128) / 257;
		mem_pal[i].green = (ctable[i].g + 128) / 257;
		mem_pal[i].blue = (ctable[i].b + 128) / 257;
	}

	init_pal();
	update_all_views();
	gtk_widget_queue_draw( drawing_col_prev );
}

static void do_allover()
{
	channel_rgb[CHN_ALPHA][0] = (ctable[0].r + 128) / 257;
	channel_rgb[CHN_ALPHA][1] = (ctable[0].g + 128) / 257;
	channel_rgb[CHN_ALPHA][2] = (ctable[0].b + 128) / 257;
	channel_opacity[CHN_ALPHA] = (ctable[0].a + 128) / 257;
	channel_rgb[CHN_SEL][0] = (ctable[1].r + 128) / 257;
	channel_rgb[CHN_SEL][1] = (ctable[1].g + 128) / 257;
	channel_rgb[CHN_SEL][2] = (ctable[1].b + 128) / 257;
	channel_opacity[CHN_SEL] = (ctable[1].a + 128) / 257;
	channel_rgb[CHN_MASK][0] = (ctable[2].r + 128) / 257;
	channel_rgb[CHN_MASK][1] = (ctable[2].g + 128) / 257;
	channel_rgb[CHN_MASK][2] = (ctable[2].b + 128) / 257;
	channel_opacity[CHN_MASK] = (ctable[2].a + 128) / 257;
	update_all_views();
}

static void do_AB(int idx)
{
	png_color *A0, *B0;
	A0 = mem_img_bpp == 1 ? &mem_pal[mem_col_A] : &mem_col_A24;
	B0 = mem_img_bpp == 1 ? &mem_pal[mem_col_B] : &mem_col_B24;

	A0->red = (ctable[idx].r + 128) / 257;
	A0->green = (ctable[idx].g + 128) / 257;
	A0->blue = (ctable[idx].b + 128) / 257;
	idx++;
	B0->red = (ctable[idx].r + 128) / 257;
	B0->green = (ctable[idx].g + 128) / 257;
	B0->blue = (ctable[idx].b + 128) / 257;
	pal_refresher();
}

static void set_csel()
{
	csel_data->center = RGB_2_INT((ctable[0].r + 128) / 257,
		(ctable[0].g + 128) / 257, (ctable[0].b + 128) / 257);
	csel_data->center_a = (ctable[0].a + 128) / 257;
	csel_data->limit = RGB_2_INT((ctable[1].r + 128) / 257,
		(ctable[1].g + 128) / 257, (ctable[1].b + 128) / 257);
	csel_data->limit_a = (ctable[1].a + 128) / 257;
	csel_preview = RGB_2_INT((ctable[2].r + 128) / 257,
		(ctable[2].g + 128) / 257, (ctable[2].b + 128) / 257);
	csel_preview_a = (ctable[2].a + 128) / 257;
}

static void select_colour(int what)
{
	switch (what)
	{
	case 0: /* Cancel */
		mem_pal_copy(mem_pal, brcosa_pal);
		pal_refresher();
		break;
	case 1: /* Preview */
		do_allcol();
		break;
	case 2: /* OK */
		mem_pal_copy(mem_pal, brcosa_pal);
		pal_refresher();
		spot_undo(UNDO_PAL);
		do_allcol();
		break;
	}
}

static void select_overlay(int what)
{
	char txt[64];
	int i, j;

	switch (what)
	{
	case 0: /* Cancel */
		for (i = 0; i < NUM_CHANNELS; i++)	// Restore original values
		{
			for (j = 0; j < 3; j++)
			{
				channel_rgb[i][j] = cs_old_overlay[i][j];
			}
			channel_opacity[i] = cs_old_overlay[i][3];
		}
		update_all_views();
		break;
	case 1: /* Preview */
		do_allover();
		break;
	case 2: /* OK */
		do_allover();
		for (i = 0; i < NUM_CHANNELS; i++)	// Save all settings to ini file
		{
			for (j = 0; j < 4; j++)
			{
				sprintf(txt, "overlay%i%i", i, j);
				inifile_set_gint32(txt, j < 3 ? channel_rgb[i][j] :
					channel_opacity[i]);
			}
		}
		break;
	}
}

static void select_AB(int what)
{
	switch (what)
	{
	case 0: /* Cancel */
		do_AB(2);
		break;
	case 2: /* OK */
		do_AB(2);
		spot_undo(UNDO_PAL);
	case 1: /* Preview */
		do_AB(0);
		break;
	}
}

static void posterize_AB(GtkButton *button, gpointer user_data)
{
	static int posm[8] = {0, 0xFF00, 0x5500, 0x2480, 0x1100,
				 0x0840, 0x0410, 0x0204};
	int i, pm, ps;

	ps = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(user_data));
	inifile_set_gint32("posterizeInt", ps);
	if (ps >= 8) return;
	pm = posm[ps]; ps = 8 - ps;

	i = (ctable[0].r + 128) / 257;
	ctable[0].r = (((i >> ps) * pm) >> 8) * 257;
	i = (ctable[0].g + 128) / 257;
	ctable[0].g = (((i >> ps) * pm) >> 8) * 257;
	i = (ctable[0].b + 128) / 257;
	ctable[0].b = (((i >> ps) * pm) >> 8) * 257;
	i = (ctable[1].r + 128) / 257;
	ctable[1].r = (((i >> ps) * pm) >> 8) * 257;
	i = (ctable[1].g + 128) / 257;
	ctable[1].g = (((i >> ps) * pm) >> 8) * 257;
	i = (ctable[1].b + 128) / 257;
	ctable[1].b = (((i >> ps) * pm) >> 8) * 257;
	color_refresh();
}

static GtkWidget *csel_spin, *csel_toggle;
static unsigned char csel_save[CSEL_SVSIZE];
static int csel_preview0, csel_preview_a0;

static void select_csel(int what)
{
	int old_over = csel_overlay;
	switch (what)
	{
	case 0: /* Cancel */
		csel_overlay = 0;
		memcpy(csel_data, csel_save, CSEL_SVSIZE);
		csel_preview = csel_preview0;
		csel_preview_a = csel_preview_a0;
		csel_reset(csel_data);
		break;
	case 1: /* Preview */
	case 2: /* OK */
		csel_overlay = 0;
		gtk_spin_button_update(GTK_SPIN_BUTTON(csel_spin));
		csel_data->range = gtk_spin_button_get_value_as_float(GTK_SPIN_BUTTON(csel_spin));
		csel_data->invert = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(csel_toggle));
		set_csel();
		csel_reset(csel_data);
		if (what != 1) break;
		old_over = 0;
		csel_overlay = 1;
		break;
	case 4: /* Set */
		csel_overlay = 0;
		if (allcol_idx != 1) break; /* Only for limit */
		set_csel();
		csel_data->range = csel_eval(csel_data->mode, csel_data->center,
			csel_data->limit);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(csel_spin), csel_data->range);
		break;
	}
	if ((old_over != csel_overlay) && drawing_canvas)
		gtk_widget_queue_draw(drawing_canvas);
}

static void csel_mode_changed(GtkToggleButton *widget, gpointer user_data)
{
	int old_over;

	if (!gtk_toggle_button_get_active(widget)) return;
	if (csel_data->mode == (int)user_data) return;
	old_over = csel_overlay;
	csel_overlay = 0;
	csel_data->mode = (int)user_data;
	set_csel();
	csel_data->range = csel_eval(csel_data->mode, csel_data->center,
		csel_data->limit);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(csel_spin), csel_data->range);
	if (old_over && drawing_canvas) gtk_widget_queue_draw(drawing_canvas);
}

void colour_selector( int cs_type )		// Bring up GTK+ colour wheel
{
	GtkWidget *win, *extbox, *button, *spin;
	int i, j;

	if (cs_type == COLSEL_EDIT_ALL)
	{
		mem_pal_copy( brcosa_pal, mem_pal );	// Remember old settings
		ctable = malloc(mem_cols * sizeof(RGBA16));
		for (i = 0; i < mem_cols; i++)
		{
			ctable[i].r = mem_pal[i].red * 257;
			ctable[i].g = mem_pal[i].green * 257;
			ctable[i].b = mem_pal[i].blue * 257;
			ctable[i].a = 65535;
		}
		win = add_a_window(GTK_WINDOW_TOPLEVEL, _("Palette Editor"),
			GTK_WIN_POS_MOUSE, TRUE);
		colour_window(win, NULL, mem_cols, NULL, FALSE, select_colour);
	}

	if (cs_type == COLSEL_OVERLAYS)
	{
		char *ovl_txt[] = { _("Alpha"), _("Selection"), _("Mask") };
		for (i = 0; i < NUM_CHANNELS; i++)	// Needed if Cancel is pressed
		{
			for (j = 0; j < 3; j++)
			{
				cs_old_overlay[i][j] = channel_rgb[i][j];
			}
			cs_old_overlay[i][3] = channel_opacity[i];
		}

		ctable = malloc(3 * sizeof(RGBA16));
		ctable[0].r = channel_rgb[CHN_ALPHA][0] * 257;
		ctable[0].g = channel_rgb[CHN_ALPHA][1] * 257;
		ctable[0].b = channel_rgb[CHN_ALPHA][2] * 257;
		ctable[0].a = channel_opacity[CHN_ALPHA] * 257;
		ctable[1].r = channel_rgb[CHN_SEL][0] * 257;
		ctable[1].g = channel_rgb[CHN_SEL][1] * 257;
		ctable[1].b = channel_rgb[CHN_SEL][2] * 257;
		ctable[1].a = channel_opacity[CHN_SEL] * 257;
		ctable[2].r = channel_rgb[CHN_MASK][0] * 257;
		ctable[2].g = channel_rgb[CHN_MASK][1] * 257;
		ctable[2].b = channel_rgb[CHN_MASK][2] * 257;
		ctable[2].a = channel_opacity[CHN_MASK] * 257;

		win = add_a_window(GTK_WINDOW_TOPLEVEL, _("Configure Overlays"),
			GTK_WIN_POS_CENTER, TRUE);
		colour_window(win, NULL, 3, ovl_txt, TRUE, select_overlay);
	}

	if (cs_type == COLSEL_EDIT_AB)
	{
		static char *AB_txt[] = { "A", "B" };
		png_color *A0, *B0;
		A0 = mem_img_bpp == 1 ? &mem_pal[mem_col_A] : &mem_col_A24;
		B0 = mem_img_bpp == 1 ? &mem_pal[mem_col_B] : &mem_col_B24;

		ctable = malloc(4 * sizeof(RGBA16));
		ctable[0].r = A0->red * 257;
		ctable[0].g = A0->green * 257;
		ctable[0].b = A0->blue * 257;
		ctable[0].a = 65535;
		ctable[1].r = B0->red * 257;
		ctable[1].g = B0->green * 257;
		ctable[1].b = B0->blue * 257;
		ctable[1].a = 65535;
		/* Save previous values right here */
		ctable[2] = ctable[0];
		ctable[3] = ctable[1];

		/* Prepare posterize controls */
		extbox = gtk_hbox_new(FALSE, 0);
		gtk_widget_show(extbox);
		button = add_a_button(_("Posterize"), 4, extbox, TRUE);
		spin = add_a_spin(inifile_get_gint32("posterizeInt", 1), 1, 8);
		gtk_box_pack_start(GTK_BOX(extbox), spin, FALSE, FALSE, 0);
		gtk_signal_connect(GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(posterize_AB), spin);

		win = add_a_window(GTK_WINDOW_TOPLEVEL, _("Colour Editor"),
			GTK_WIN_POS_MOUSE, TRUE);
		colour_window(win, extbox, 2, AB_txt, FALSE, select_AB);
	}
	if (cs_type == COLSEL_EDIT_CSEL)
	{
		char *csel_txt[] = { _("Centre"), _("Limit"), _("Preview") };
		char *csel_modes[] = { _("Sphere"), _("Angle"), _("Cube"), NULL };

		if (!csel_data)
		{
			csel_data = csel_init();
			if (!csel_data) return;
		}
		/* Save previous values */
		memcpy(csel_save, csel_data, CSEL_SVSIZE);
		csel_preview0 = csel_preview;
		csel_preview_a0 = csel_preview_a;

		ctable = malloc(3 * sizeof(RGBA16));
		ctable[0].r = INT_2_R(csel_data->center) * 257;
		ctable[0].g = INT_2_G(csel_data->center) * 257;
		ctable[0].b = INT_2_B(csel_data->center) * 257;
		ctable[0].a = csel_data->center_a * 257;
		ctable[1].r = INT_2_R(csel_data->limit) * 257;
		ctable[1].g = INT_2_G(csel_data->limit) * 257;
		ctable[1].b = INT_2_B(csel_data->limit) * 257;
		ctable[1].a = csel_data->limit_a * 257;
		ctable[2].r = INT_2_R(csel_preview) * 257;
		ctable[2].g = INT_2_G(csel_preview) * 257;
		ctable[2].b = INT_2_B(csel_preview) * 257;
		ctable[2].a = csel_preview_a * 257;

		/* Prepare extra controls */
		extbox = gtk_hbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(extbox), gtk_label_new(_("Range")),
			FALSE, FALSE, 0);
		spin = csel_spin = add_a_spin(0, 0, 765);
		gtk_box_pack_start(GTK_BOX(extbox), spin, FALSE, FALSE, 0);
		gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 2);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), csel_data->range);
		csel_toggle = add_a_toggle(_("Inverse"), extbox, csel_data->invert);
		i = 0;
		button = gtk_radio_button_new_with_label(NULL, csel_modes[0]);
		while (1)
		{
			gtk_container_set_border_width(GTK_CONTAINER(button), 5);
			gtk_box_pack_start(GTK_BOX(extbox), button, FALSE, FALSE, 0);
			if (i == csel_data->mode) gtk_toggle_button_set_active(
				GTK_TOGGLE_BUTTON(button), TRUE);
			gtk_signal_connect(GTK_OBJECT(button), "toggled",
				GTK_SIGNAL_FUNC(csel_mode_changed), (gpointer)i);
			if (!csel_modes[++i]) break;
			button = gtk_radio_button_new_with_label_from_widget(
				GTK_RADIO_BUTTON(button), csel_modes[i]);
		}
		gtk_widget_show_all(extbox);

		win = add_a_window(GTK_WINDOW_TOPLEVEL, _("Colour-Selective Mode"),
			GTK_WIN_POS_CENTER, TRUE);
/* !!! Alpha ranges not implemented yet !!! */
//		colour_window(win, extbox, 3, csel_txt, TRUE, select_csel);
		colour_window(win, extbox, 3, csel_txt, FALSE, select_csel);
	}
}

void pressed_allcol( GtkMenuItem *menu_item, gpointer user_data )
{
	colour_selector( COLSEL_EDIT_ALL );
}

void choose_colours()
{
	colour_selector(COLSEL_EDIT_AB);
}

///	QUANTIZE WINDOW

GtkWidget *quantize_window, *quantize_spin, *quantize_radio[5], *quantize_radio2[4];
int quantize_cols;


gint delete_quantize( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	gtk_widget_destroy(quantize_window);

	return FALSE;
}

gint click_quantize_radio( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	int i;
	gboolean value = FALSE;

	if ( gtk_toggle_button_get_active(
		&(GTK_RADIO_BUTTON( quantize_radio[0] )->check_button.toggle_button) ) )
		value = FALSE;
	else
		value = TRUE;

	if ( !value )
		// If the user wants an exact transfer, don't allow user to choose dither/floyd etc
	{
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(quantize_radio2[0]), TRUE );
	}

	for ( i=1; i<4; i++ ) gtk_widget_set_sensitive(quantize_radio2[i], value);

	return FALSE;
}

gint click_quantize_ok( GtkWidget *widget, GdkEvent *event, gpointer data )
{
	unsigned char *old_image = mem_img[CHN_IMAGE], newpal[3][256];
	int rad1=0, rad2=0, i, k, new_cols, dither;

	for ( i=0; i<5; i++ )
		if ( gtk_toggle_button_get_active(
			&(GTK_RADIO_BUTTON( quantize_radio[i] )->check_button.toggle_button)
				) ) rad1 = i;

	for ( i=0; i<4; i++ )
		if ( gtk_toggle_button_get_active(
			&(GTK_RADIO_BUTTON( quantize_radio2[i] )->check_button.toggle_button)
				) ) rad2 = i;

	gtk_spin_button_update( GTK_SPIN_BUTTON(quantize_spin) );
	new_cols = gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(quantize_spin) );
	mtMAX( new_cols, new_cols, 2 )
	mtMIN( new_cols, new_cols, 256 )

	delete_quantize( NULL, NULL, NULL );

	if ( rad1 == 0 )			// Reduce to indexed using exact canvas pixels
	{
		new_cols = quantize_cols;
		i = mem_convert_indexed();
	}
	else
	{
		i = undo_next_core(2, mem_width, mem_height, 1, CMASK_IMAGE);
		if ( i == 1 ) i=2;

		if ( i == 0 )
		{
			if ( rad1 == 1 )
				for ( k=0; k<new_cols; k++ )
				{
					newpal[0][k] = mem_pal[k].red;
					newpal[1][k] = mem_pal[k].green;
					newpal[2][k] = mem_pal[k].blue;
				}

			if ( rad1 == 2 ) i = dl1quant(old_image, mem_width, mem_height,
						new_cols, newpal);
			if ( rad1 == 3 ) i = dl3quant(old_image, mem_width, mem_height,
						new_cols, newpal);
			if ( rad1 == 4 ) i = wu_quant(old_image, mem_width, mem_height,
						new_cols, newpal);
			for ( k=0; k<new_cols; k++ )
			{
				mem_pal[k].red = newpal[0][k];
				mem_pal[k].green = newpal[1][k];
				mem_pal[k].blue = newpal[2][k];
			}
			if ( i == 0 )
			{
				dither = rad2 % 2;
				if ( rad2 < 2 ) i = dl3floste(old_image, mem_img[CHN_IMAGE],
					mem_width, mem_height, new_cols, dither, newpal);
						// Floyd-Steinberg
				if ( rad2 > 1 && rad2 < 4 )
					i = mem_quantize( old_image, new_cols, rad2 );
						// Dither/scatter
			}
		}
	}

	if ( i!=0 ) memory_errors(i);
	else
	{
		if ( tool_type == TOOL_SELECT && marq_status >= MARQUEE_PASTE )
			pressed_select_none( NULL, NULL );
		if ( tool_type == TOOL_SMUDGE )
			gtk_toggle_button_set_active(
				GTK_TOGGLE_BUTTON(icon_buttons[DEFAULT_TOOL_ICON]), TRUE );
					// If the user is pasting or smudging, lose it!

		mem_cols = new_cols;
		update_menus();
		init_pal();
		update_all_views();
		gtk_widget_queue_draw( drawing_col_prev );
	}

	return FALSE;
}

void pressed_quantize( GtkMenuItem *menu_item, gpointer user_data )
{
	int i = mem_cols_used(257), j = i;
	char *rad_txt[] = {_("Exact Conversion"), _("Use Current Palette"),
		_("DL1 Quantize (fastest)"), _("DL3 Quantize (very slow, better quality)"),
		_("Wu Quantize (best method for small palettes)")
		};

	char *rad_txt2[] = {_("Flat Colour"), _("Floyd-Steinberg"),
		_("Dithered"), _("Scattered"),
		};

	GSList *group;
	GtkWidget *vbox4, *vbox5, *hbox6, *table3, *frame;
	GtkWidget *button_cancel, *button_ok;
	GtkAccelGroup* ag = gtk_accel_group_new();

	if ( i<2 )
	{
		alert_box( _("Error"), _("You don't have enough unique RGB pixels to reduce to indexed - you must have at least 2."), _("OK"), NULL, NULL );
		return;
	}

	quantize_cols = j;
	if ( j>256 ) j = mem_cols;

	quantize_window = add_a_window( GTK_WINDOW_TOPLEVEL, _("Convert To Indexed"),
		GTK_WIN_POS_CENTER, TRUE );

	vbox5 = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox5);
	gtk_container_add (GTK_CONTAINER (quantize_window), vbox5);

	table3 = add_a_table( 2, 2, 10, vbox5 );
	add_to_table( _("Indexed Colours To Use"), table3, 1, 0, 0, GTK_JUSTIFY_LEFT, 0, 0.5 );
	spin_to_table( table3, &quantize_spin, 1, 1, 5, j, 2, 256 );

///	Palette FRAME

	frame = gtk_frame_new (_("Palette"));
	gtk_widget_show (frame);
	gtk_box_pack_start (GTK_BOX (vbox5), frame, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (frame), 5);

	vbox4 = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox4);
	gtk_container_add (GTK_CONTAINER (frame), vbox4);

	quantize_radio[0] = add_radio_button( rad_txt[0], NULL,  NULL, vbox4, 0 );
	gtk_signal_connect(GTK_OBJECT(quantize_radio[0]), "clicked",
			GTK_SIGNAL_FUNC(click_quantize_radio), NULL);
	group = gtk_radio_button_group( GTK_RADIO_BUTTON(quantize_radio[0]) );
	quantize_radio[1] = add_radio_button( rad_txt[1], group, NULL, vbox4, 1 );
	gtk_signal_connect(GTK_OBJECT(quantize_radio[1]), "clicked",
			GTK_SIGNAL_FUNC(click_quantize_radio), NULL);

	for ( i=2; i<5; i++ )
	{
		quantize_radio[i] = add_radio_button( rad_txt[i], NULL,  quantize_radio[1], vbox4, i );
		gtk_signal_connect(GTK_OBJECT(quantize_radio[i]), "clicked",
			GTK_SIGNAL_FUNC(click_quantize_radio), NULL);
	}

///	Image FRAME

	frame = gtk_frame_new (_("Image"));
	gtk_widget_show (frame);
	gtk_box_pack_start (GTK_BOX (vbox5), frame, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (frame), 5);

	vbox4 = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox4);
	gtk_container_add (GTK_CONTAINER (frame), vbox4);

	quantize_radio2[0] = add_radio_button( rad_txt2[0], NULL,  NULL, vbox4, 0 );
	group = gtk_radio_button_group( GTK_RADIO_BUTTON(quantize_radio2[0]) );
	quantize_radio2[1] = add_radio_button( rad_txt2[1], group, NULL, vbox4, 1 );

	for ( i=2; i<4; i++ )
	{
		quantize_radio2[i] = add_radio_button( rad_txt2[i], NULL,  quantize_radio2[1],
						vbox4, i );
	}


	if ( quantize_cols > 256 )
	{
		gtk_widget_hide (quantize_radio[0]);
		gtk_widget_set_sensitive( quantize_radio[0], FALSE );
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(quantize_radio[4]), TRUE );
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(quantize_radio2[1]), TRUE );
	}

	hbox6 = gtk_hbox_new (FALSE, 0);
	gtk_widget_show (hbox6);
	gtk_box_pack_start (GTK_BOX (vbox5), hbox6, FALSE, FALSE, 0);

	button_cancel = add_a_button(_("Cancel"), 5, hbox6, TRUE);
	gtk_signal_connect(GTK_OBJECT(button_cancel), "clicked", GTK_SIGNAL_FUNC(delete_quantize), NULL);
	gtk_widget_add_accelerator (button_cancel, "clicked", ag, GDK_Escape, 0, (GtkAccelFlags) 0);

	button_ok = add_a_button(_("OK"), 5, hbox6, TRUE);
	gtk_signal_connect(GTK_OBJECT(button_ok), "clicked", GTK_SIGNAL_FUNC(click_quantize_ok), NULL);
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_Return, 0, (GtkAccelFlags) 0);
	gtk_widget_add_accelerator (button_ok, "clicked", ag, GDK_KP_Enter, 0, (GtkAccelFlags) 0);

	gtk_window_set_transient_for( GTK_WINDOW(quantize_window), GTK_WINDOW(main_window) );
	gtk_widget_show (quantize_window);
	gtk_window_add_accel_group(GTK_WINDOW (quantize_window), ag);

	click_quantize_radio( NULL, NULL, NULL );	// Grey out radio buttons if needed
}
