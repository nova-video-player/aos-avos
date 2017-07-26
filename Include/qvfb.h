/****************************************************************************
**
** Qt/Embedded virtual framebuffer
**
** Created : 20000605
**
** Copyright (C) 1992-2000 Trolltech AS.  All rights reserved.
**
** This file is part of the kernel module of the Qt GUI Toolkit.
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.
**
** Licensees holding valid Qt Enterprise Edition or Qt Professional Edition
** licenses for Qt/Embedded may use this file in accordance with the
** Qt Embedded Commercial License Agreement provided with the Software.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.trolltech.com/pricing.html or email sales@trolltech.com for
**   information about Qt Commercial License Agreements.
** See http://www.trolltech.com/gpl/ for GPL licensing information.
**
** Contact info@trolltech.com if any conditions of this licensing are
** not clear to you.
**
**********************************************************************/

#ifndef QVFBHDR_H

#define QT_VFB_MOUSE_PIPE	"/tmp/.qtvfb_mouse-0"
#define QT_VFB_KEYBOARD_PIPE	"/tmp/.qtvfb_keyboard-0"

typedef struct qrect
{
	int x1;
	int y1;
	int x2;
	int y2;
} QRect;

typedef struct qrgb
{
	int rgba;
} QRgb;

struct QVFbHeader
{
	int width;
	int height;
	int depth;
	int linestep;
	QRect update;
	int dirty;
	int numcols;
	QRgb clut[256];

	int osd_offset[2];
	int alpha_offset;
	int video0_offset;
	int video1_offset;

	int osd_actbuf;
	int osd_active;
	int osd_mix;
	int osd_blend;
	
	int alpha_active;
	int video0_active;
	int video1_active;

	int backlight;
	int brightness;
};

struct QVFbKeyData
{
	unsigned int unicode;
	unsigned int modifiers;
	unsigned char press;
	unsigned char repeat;
};

#endif
