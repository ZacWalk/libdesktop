// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: the text models and views umbrella - opt-in, outside ui.h.

#pragma once

#include "libdesktop/ui.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/text/text_line.h"
#include "libdesktop/ui/text/text_buffer.h"
#include "libdesktop/ui/text/view_host.h"
#include "libdesktop/ui/text/spell.h"
#include "libdesktop/ui/text/markdown.h"
#include "libdesktop/ui/text/table_layout.h"
#include "libdesktop/ui/text/clipboard.h"
#include "libdesktop/ui/views/view_theme.h"
#include "libdesktop/ui/views/scrollbar.h"
#include "libdesktop/ui/views/split_element.h"
#include "libdesktop/ui/views/view_base.h"
#include "libdesktop/ui/views/text_view.h"
#include "libdesktop/ui/views/doc_view.h"
#include "libdesktop/ui/views/read_only_doc_view.h"
#include "libdesktop/ui/views/edit_doc_view.h"
#include "libdesktop/ui/views/list_view.h"
#include "libdesktop/ui/views/hex_view.h"
#include "libdesktop/ui/views/csv_view.h"
#include "libdesktop/ui/views/markdown_view.h"
