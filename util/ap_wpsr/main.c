/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2022 Google Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "flash.h"
#include "programmer.h"
#include "libflashrom.h"
#include "writeprotect.h"


static const struct flashchip *find_chip(const char *name)
{
	for (const struct flashchip *chip = flashchips; chip && chip->name; chip++)
		if (!strcmp(chip->name, name))
			return chip;
	return NULL;
}

static const char *get_wp_error_str(int err)
{
	switch (err) {
	case FLASHROM_WP_ERR_CHIP_UNSUPPORTED:
		return "WP operations are not implemented for this chip";
	case FLASHROM_WP_ERR_READ_FAILED:
		return "failed to read the current WP configuration";
	case FLASHROM_WP_ERR_WRITE_FAILED:
		return "failed to write the new WP configuration";
	case FLASHROM_WP_ERR_VERIFY_FAILED:
		return "unexpected WP configuration read back from chip";
	case FLASHROM_WP_ERR_MODE_UNSUPPORTED:
		return "the requested protection mode is not supported";
	case FLASHROM_WP_ERR_RANGE_UNSUPPORTED:
		return "the requested protection range is not supported";
	case FLASHROM_WP_ERR_RANGE_LIST_UNAVAILABLE:
		return "could not determine what protection ranges are available";
	case FLASHROM_WP_ERR_UNSUPPORTED_STATE:
		return "can't operate on current WP configuration of the chip";
	}
	return "unknown WP error";
}

void chip_4ba_feature_decode(const uint32_t feature_bits)
{
	if (feature_bits | FEATURE_4BA_ENTER)
	       printf(" > Can enter/exit 4BA mode with instructions 0xb7/0xe9 w/o WREN\n");
	if (feature_bits | FEATURE_4BA_ENTER_WREN)
		printf(" > Can enter/exit 4BA mode with instructions 0xb7/0xe9 after WREN\n");
	if (feature_bits | FEATURE_4BA_ENTER_EAR7)
		printf(" > Can enter/exit 4BA mode by setting bit7 of the ext addr reg\n");
	if (feature_bits | FEATURE_4BA_EAR_C5C8)
		printf(" > Regular 3-byte operations can be used by writing the most"\
				"significant address byte into an extended address register"\
				"(using 0xc5/0xc8 instructions).\n");
	if (feature_bits | FEATURE_4BA_EAR_1716)
		printf(" > Like FEATURE_4BA_EAR_C5C8 but with 0x17/0x16 instructions.\n");
	if (feature_bits | FEATURE_4BA_READ)
		printf(" > Native 4BA read instruction (0x13) is supported.\n");
	if (feature_bits | FEATURE_4BA_FAST_READ)
		printf(" > Native 4BA fast read instruction (0x0c) is supported.\n");
	if (feature_bits | FEATURE_4BA_WRITE)
		printf(" > Native 4BA byte program (0x12) is supported.\n");
	putchar('\n');
}

void print_register_state(uint8_t *reg_values, uint8_t *wp_bit_masks)
{
	/*
	 * The value of last_reg determines how many register values are printed.
	 *
	 * TODO: We look through wp_bit_masks to find the last non-zero
	 * register, but it might be better to just print SR1/SR2/SR3.
	 * I.e. set last_reg = STATUS3;
	 */
	enum flash_reg last_reg = STATUS1;
	for (enum flash_reg reg = STATUS1; reg < MAX_REGISTERS; reg++) {
		if (wp_bit_masks[reg] != 0)
			last_reg = reg;
	}

	printf("\n * SR = {");
	for (enum flash_reg reg = STATUS1; reg <= last_reg; reg++) {
		if (reg != STATUS1)
			printf(", ");
		printf("0x%02x", reg_values[reg]);
	}
	printf("}.\n");

	printf(" * SR mask = {");
	for (enum flash_reg reg = STATUS1; reg <= last_reg; reg++) {
		if (reg != STATUS1)
			printf(", ");
		printf("0x%02x", wp_bit_masks[reg]);
	}
	printf("}.\n");

	printf(" * SR Value/Mask = ");
	for (enum flash_reg reg = STATUS1; reg <= last_reg; reg++) {
		if (reg != STATUS1)
			printf(" ");
		printf("0x%02X 0x%02X", reg_values[reg], wp_bit_masks[reg]);
	}
	printf("\n");
}

enum flashrom_wp_result print_wp_regmasks(const char *name, uint32_t wp_start, uint32_t wp_len)
{
	struct registered_master r_mst = {0};
	struct flashctx flash = { .mst = &r_mst };

	flash.chip = (struct flashchip *) find_chip(name);
	if (flash.chip)
		printf(" > found match '%s' in chip db.\n\n", flash.chip->name);
	else {
		fprintf(stderr, " no match found for '%s' in chip db.\n", name);
		exit(1);
	}

	chip_4ba_feature_decode(flash.chip->feature_bits);

	struct flashrom_wp_cfg *cfg = NULL;
	enum flashrom_wp_result ret = flashrom_wp_cfg_new(&cfg);

	if (ret != FLASHROM_WP_OK) {
		fprintf(stderr, " wp init err %d.\n", ret);
		return ret;
	}

	flashrom_wp_set_range(cfg, wp_start, wp_len);
	flashrom_wp_set_mode(cfg, FLASHROM_WP_MODE_HARDWARE);

	uint8_t reg_values[MAX_REGISTERS] = {0};
	uint8_t wp_bit_masks[MAX_REGISTERS] = {0};
	uint8_t unused[MAX_REGISTERS];
	ret = wp_cfg_to_reg_values(reg_values, wp_bit_masks, unused, &flash, cfg);
	flashrom_wp_cfg_release(cfg);

	if (ret != FLASHROM_WP_OK) {
		fprintf(stderr, " register value/mask calculation err %d.\n", ret);
		return ret;
	}

	print_register_state(reg_values, wp_bit_masks);

	return ret;
}

void print_help(int argc, char* argv[])
{
	fprintf(stderr, "Usage: %s [OPTIONS]\n\n"
		        "Required arguments:\n"
			"  -n, --name=name      Name of chip to \n"
			"                       calculate SR values for\n"
			"  -s, --start=addr     Start address of protection range\n"
			"  -l, --length=addr    Length of protection range\n\n"
			"Optional arguments:\n"
			"  -h, --help           Print help and exit\n\n",
			argv[0]);
}

int main(int argc, char* argv[])
{
	char *name = NULL;
	uint32_t wp_start = 0, wp_len = 0; /* default */
	bool wp_start_set = false, wp_len_set = false;

	static const char optstr[] = "hn:s:l:";
	static const struct option long_options[] = {
		{"help",		0, NULL, 'h'},
		{"name",		1, NULL, 'n'},
		{"start",		1, NULL, 's'},
		{"length",		1, NULL, 'l'},
		{NULL,			0, NULL, 0},
	};
	int opt, opt_idx = 0;
	while ((opt = getopt_long(argc, argv, optstr, long_options, &opt_idx)) != EOF) {
		switch (opt) {
			case 'n':
				name = optarg;
				break;
			case 's':
				wp_start = strtoul(optarg, NULL, 0);
				wp_start_set = true;
				break;
			case 'l':
				wp_len = strtoul(optarg, NULL, 0);
				wp_len_set = true;
				break;
			case 'h':
			default:
				print_help(argc, argv);
				return 0;
		}
	}

	if (!name) {
		fprintf(stderr, "Error: --name <chip name> must be provided\n");
		return 1;
	}

	if (!wp_start_set) {
		fprintf(stderr, "Error: --start <address> must be provided\n");
		return 1;
	}

	if (!wp_len_set) {
		fprintf(stderr, "Error: --length <len> must be provided\n");
		return 1;
	}

	printf(" > requested chip name: '%s' with start: 0x%x and len: 0x%x.\n", name, wp_start, wp_len);

	enum flashrom_wp_result ret = print_wp_regmasks(name, wp_start, wp_len);
	if (ret != FLASHROM_WP_OK) {
		fprintf(stderr, "Error: '%s'\n", get_wp_error_str(ret));
		return 1;
	}

	return 0;
}
