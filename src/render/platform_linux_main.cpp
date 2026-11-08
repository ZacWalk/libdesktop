// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Linux entry point, Linux's wWinMain. It is the object libdesktop::render_main, which an
// application links itself, and never a member of the archive: GNU ld takes main from the first archive
// on the line that defines one, and an application's other archives can carry a tool's main.

int libdesktop_linux_main(int argc, char* argv[]);

int main(const int argc, char* argv[])
{
	return libdesktop_linux_main(argc, argv);
}
