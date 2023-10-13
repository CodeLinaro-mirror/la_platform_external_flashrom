/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2020 The Chromium OS Authors. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "programmer.h"
#include "hwaccess_physmap.h"

static int cros_host_alias_init(const struct programmer_cfg *cfg)
{
	msg_pdbg("%s(): Redirecting dispatch -> internal_init().\n", __func__);
	return internal_init(cfg);
}

const struct programmer_entry programmer_google_host_alias = {
	.name			= "host",
	.type			= OTHER,
	.devs.note		= "Google host alias mechanism.\n",
	.init			= cros_host_alias_init,
};
