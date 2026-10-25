// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The resource identifiers of the desktop UI's own Windows resources - the Direct3D shaders, the
// drop shadow and the cursors, which platform_win_render.rc carries - and of the two an application's
// resource script supplies for it: the window icon and the icon font the chrome draws with.

#ifndef PLATFORM_WIN_RENDER_RES_H
#define PLATFORM_WIN_RENDER_RES_H

// Supplied by the application's resource script.
#define IDF_ICONS   115
#define IDI_APP_ICON   117

// Carried by platform_win_render.rc.
#define IDB_SHADOW   108
#define IDC_COLORSELECT   109
#define IDC_HANDDOWN   110
#define IDC_HANDLINK   111
#define IDC_HANDUP   112
#define IDC_MOVE   113
#define IDC_SELECT   114
#define IDC_ZOOM   116
#define IDR_SHADER_FONT   121
#define IDR_SHADER_RGB   122
#define IDR_SHADER_RGB_BICUBIC   123
#define IDR_SHADER_SOLID   124
#define IDR_SHADER_VERTEX   125
#define IDR_SHADER_YUV   126
#define IDR_SHADER_YUV_BICUBIC   127
#define IDR_SHADER_CIRCLE   128
#define IDR_SHADER_PANO   129
#define IDR_SHADER_YUV_HDR   130
#define IDR_SHADER_YUV_HDR_BICUBIC   131

#endif
