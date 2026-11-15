// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
#include "libdesktop/ui/table_element.h"
#include "libdesktop/ui/text/clipboard.h"
#include "libdesktop/ui/views/view_theme.h"
#include "libdesktop/ui/views/document_panel.h"
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
