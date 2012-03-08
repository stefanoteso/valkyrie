/* 
 * Valkyrie
 * Copyright (C) 2011, Stefano Teso
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/* TODO: move to a scheduler-based system, so that machines register their
 * components and the scheduler executes them. */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <getopt.h>
#include <signal.h>

#include <SDL/SDL.h>

#include "vk/machine.h"
#include "vk/input.h"
#include "vk/renderer.h"
#include "vk/games.h"

#ifdef VK_HAVE_SATURN
#include "mach/saturn/mach.h"
#endif
#ifdef VK_HAVE_DREAMCAST
#include "mach/dreamcast/mach.h"
#endif
#ifdef VK_HAVE_HIKARU
#include "mach/hikaru/hikaru.h"
#endif

#define VK_VERSION_MINOR 1
#define VK_VERSION_MAJOR 0

static struct {
	char rom_path[256];
	char rom_name[256];
} options;

static vk_game_list_t *game_list;
static vk_game_t *game;
static vk_machine_t *mach;

static int
load_or_save_state (vk_machine_t *mach, bool flag)
{
	FILE *fp = NULL;
	char *path;
	int ret;

	ret = asprintf (&path, "%s.vkstate", mach->game->name);
	if (ret <= 0)
		goto fail;

	fp = fopen (path, flag ? "rb" : "wb");
	if (!fp) {
		ret = -1;
		goto fail;
	}

	ret = flag ? vk_machine_load_state (mach, fp) :
	             vk_machine_save_state (mach, fp);

	if (!ret) {
		VK_LOG ("%s state '%s'", flag ? "loaded" : "saved", path);
	} else {
		VK_LOG ("failed to %s state '%s'", flag ? "load" : "save", path);
	}
fail:
	if (fp)
		fclose (fp);
	free (path);
	return ret;
}

static bool
process_events (void)
{
	SDL_Event event;
	bool quit = false;

	while (SDL_PollEvent (&event)) {
		switch (event.type) {
		case SDL_KEYDOWN:
			vk_input_set_key (event.key.keysym.sym, true);
			if (event.key.keysym.sym == SDLK_ESCAPE)
				quit = true;
			break;
		case SDL_KEYUP:
			vk_input_set_key (event.key.keysym.sym, false);
			break;
		case SDL_QUIT:
			quit = true;
			break;
		}
	}
	if (vk_input_get_key (SDLK_F1))
		load_or_save_state (mach, true);
	else if (vk_input_get_key (SDLK_F2))
		load_or_save_state (mach, false);
	return quit;
}

static void
main_loop (vk_machine_t *mach)
{
	while (!process_events ()) {
//		vk_renderer_begin_frame (mach->renderer);
		vk_machine_run_frame (mach);
//		vk_renderer_end_frame (mach->renderer);
	}
}

static const char global_opts[] = "R:r:h?";
static const char global_help[] = "Usage: %s [options]\n"
"	-R <path>	Path to the ROM directory\n"
"	-r <string>	Name of the game to run\n"
"	-h              Show this help\n";

static void
show_help (int argc, char **argv)
{
	fprintf (stdout, global_help, argv[0]);
}

static int
parse_global_opts (int argc, char **argv)
{
	int opt;

	memset (&options, 0, sizeof (options));

	if (argc < 2) {
		show_help (argc, argv);
		return -1;
	}

	while ((opt = getopt (argc, argv, global_opts)) != -1) {
		switch (opt) {
		case 'R':
			strncpy (options.rom_path, optarg, 256);
			break;
		case 'r':
			strncpy (options.rom_name, optarg, 32);
			break;
		case 'h':
		case '?':
			show_help (argc, argv);
			return -1;
		}
	}
	return 0;
}

static vk_machine_t *
get_machine_for_game (vk_game_t *game)
{
	char *mach = game->mach;
	if (!strcmp (mach, "hikaru"))
		return hikaru_new (game);
	VK_ERROR ("unrecognized machine '%s'", mach);
	return NULL;
}

static void
finalize (void)
{
	/* XXX free the game list and the game data */
	VK_LOG ("Finalizing");
	if (mach)
		vk_machine_delete (&mach);
}

int
main (const int argc, char **argv)
{
	fprintf (stdout, "Valkyrie, Copyright(C) 2011,2012 Stefano Teso\n");
	fprintf (stdout, "Version %u.%u. Released under the GPL2 License.\n",
	         VK_VERSION_MAJOR, VK_VERSION_MINOR);
	if (VK_VERSION_MINOR % 2)
		fprintf (stdout, " ** Warning: this is an experimental snapshot. **\n");

	if (parse_global_opts (argc, argv))
		goto fail;

	game_list = vk_game_list_new ("/home/stefano/vk-games.json");
	if (!game_list) {
		game_list = vk_game_list_new ("/usr/shader/valkyrie/vk-games.json");
		if (!game_list) {
			VK_ERROR ("failed to load the game list");
			goto fail;
		}
	}

	/* XXX retry on known locations too */

	game = vk_game_new (game_list, options.rom_path, options.rom_name);
	if (!game) {
		VK_ERROR ("failed to load '%s'", options.rom_name);
		goto fail;
	}

	mach = get_machine_for_game (game);
	if (!mach) {
		VK_LOG ("failed to create machine");
		goto fail;
	}

	atexit (finalize);
	//signal (SIGINT,  finalize);
	//signal (SIGKILL, finalize);

	if (vk_machine_parse_args (mach, argc, argv))
		goto fail;

	mach->reset (mach, VK_RESET_TYPE_HARD);

	VK_LOG ("Running");
	main_loop (mach);

fail:
	return 0;
}

