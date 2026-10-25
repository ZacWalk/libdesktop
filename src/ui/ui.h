// ui.h — the libdesktop-ui umbrella header.
//
// libdesktop-ui is the reusable presentation layer that sits on libdesktop-core:
// widgets, text views, markdown rendering and the agent chat panel. It operates on
// a UTF-8 text buffer it is handed. It never opens a file, never knows a path, and
// never names an application — loading and saving belong to the application.
//
// Everything here lives in pf::ui. Include the individual headers when you only need
// part of it; include this one to get the lot.

#pragma once

#include "platform.h"

#include "ui/theme.h"
#include "ui/text_types.h"
#include "ui/view_host.h"
#include "ui/syntax.h"
#include "ui/spell.h"
#include "ui/text_line.h"
#include "ui/text_buffer.h"
#include "ui/markdown.h"
#include "ui/widgets.h"
#include "ui/table_layout.h"
