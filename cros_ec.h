/*
 * This file is part of the flashrom project.
 *
 * Copyright 2013 Google Inc.
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
 *
 */

#ifndef __CROS_EC_H_
#define __CROS_EC_H_

/* FIXME: We should be able to forward declare enum ec_current_image here
 * instead of including cros_ec_ec_commands.h */
#include "cros_ec_commands.h"
#include "programmer.h"

int cros_ec_block_erase(struct flashctx *flash,
                    unsigned int blockaddr, unsigned int len);
int cros_ec_command(int command, int version,
                    const void *outdata, int outsize,
                    void *indata, int insize);

/* used in cros_ec_wp_dep.c */
int cros_ec_get_region_info(enum ec_flash_region region, struct ec_response_flash_region_info *info);
int cros_ec_cold_reboot(int flags);

/* cros_ec_wp.c */
enum flashrom_wp_result cros_ec_wp_read_cfg(struct flashrom_wp_cfg *cfg, struct flashctx *flash);
enum flashrom_wp_result cros_ec_wp_write_cfg(struct flashctx *flash, const struct flashrom_wp_cfg *cfg);
enum flashrom_wp_result cros_ec_wp_get_available_ranges(struct flashrom_wp_ranges **list, struct flashctx *flash);

#endif	/* __CROS_EC_H_ */
