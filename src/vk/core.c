/* 
 * Valkyrie
 * Copyright (C) 2011, 2012, Stefano Teso
 * 
 * Valkyrie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Valkyrie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Valkyrie.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "vk/types.h"
#include "vk/core.h"

bool
vk_util_get_bool_option (const char *name, bool fallback)
{
	char *env = getenv (name);
	if (env) {
		if (!strcasecmp (env, "TRUE") || !strcmp (env, "1"))
			return true;
		if (!strcasecmp (env, "FALSE") || !strcmp (env, "0"))
			return false;
	}
	return fallback;
}

int
vk_util_get_int_option (const char *name, int fallback)
{
	char *env = getenv (name);
	if (env)
		return atoi (env);
	return fallback;
}
