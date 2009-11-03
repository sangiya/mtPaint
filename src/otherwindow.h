/*	otherwindow.h
	Copyright (C) 2004-2008 Mark Tyler and Dmitry Groshev

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

#include <png.h>


#define COLSEL_OVERLAYS  1
#define COLSEL_EDIT_AB   2
#define COLSEL_EDIT_CSEL 3
#define COLSEL_GRID      4
#define COLSEL_EDIT_ALL  256

typedef int (*filter_hook)(GtkWidget *content, gpointer user_data);
typedef void (*colour_hook)(int what);

png_color brcosa_palette[256];
int mem_preview, mem_preview_clip, brcosa_auto;
int sharper_reduce;
int spal_mode;

void generic_new_window(int type);

void pressed_add_cols();
void pressed_brcosa();
void pressed_bacteria();
void pressed_scale_size(int mode);

void pressed_sort_pal();
void pressed_quantize(int palette);
void pressed_pick_gradient();

void choose_pattern(int typ);				// Bring up pattern chooser

void colour_selector( int cs_type );			// Bring up GTK+ colour wheel

int do_new_one(int nw, int nh, int nc, png_color *pal, int bpp, int undo);
void do_new_chores(int undo);
void reset_tools();

void filter_window(gchar *title, GtkWidget *content, filter_hook filt, gpointer fdata, int istool);
void memory_errors(int type);

void gradient_setup(int mode);

void pressed_skew();

void bkg_setup();
