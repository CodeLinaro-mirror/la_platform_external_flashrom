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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>

#include "flash.h"
#include "flashchips.h"
#include "programmer.h"
#include "libflashrom.h"
#include "writeprotect.h"


static const struct flashchip *find_chip_by_name(const char *name)
{
	for (const struct flashchip *chip = flashchips; chip && chip->name; chip++)
		if (!strcasecmp(chip->name, name))
			return chip;
	return NULL;
}

/*
 * Fixup mechanism for JEDEC ID collisions.
 * When offline tools or automated test scripts (e.g. GscUtils.kt) invoke ap_wpsr with only
 * --jedec_id=0xc86019, multiple flash models (GD25LQ255E, GD25LQ256H, etc.) share the
 * exact same JEDEC ID despite requiring different status register layouts.
 *
 * This fixup mechanism supports resolution tiers for JEDEC ID collisions:
 * 1. Direct environment variable: `AP_WPSR_PARTNAME=<partname>`.
 * 2. Runtime sysfs probing: automatically reading `/sys/class/mtd/mtd0/device/spi-nor/partname`
 *    (or file path override via `AP_WPSR_PARTNAME_FILE`).
 */
struct ap_wpsr_fixup {
	uint8_t manufacture_id;
	uint16_t model_id;
	/*
	 * Hook called when this JEDEC ID (`manufacture_id`/`model_id`) is encountered.
	 * Returns the specific chip name (e.g., "GD25LQ256H" or "GD25LQ255E") if resolved
	 * via environment override or sysfs partname probing, or NULL if unresolved.
	 * If `probed_buf` is provided, it returns the actual probed/override string for debugging.
	 */
	const char *(*resolve_chip_name)(uint8_t manufacture_id, uint16_t model_id, char *probed_buf, size_t buf_len);
	/*
	 * Default fallback chip name to select when multiple non-duplicate entries
	 * share this JEDEC ID and runtime probing returns NULL (e.g. during offline host calculations).
	 */
	const char *default_fallback_name;
};

static char *trim_ws(char *str)
{
	if (!str)
		return NULL;
	while (isspace((unsigned char)*str))
		str++;
	size_t len = strlen(str);
	while (len > 0 && isspace((unsigned char)str[len - 1]))
		str[--len] = '\0';
	return str;
}

/*
 * Map Linux kernel spi-nor partnames (e.g., "gd25lq256h", "gd25lq255e") to flashrom chip names.
 * Explicit ad-hoc mapping is required because kernel partnames do not map 1-to-1 with flashrom
 * chip names, which may combine variants using slashes (e.g., "GD25LB256F/GD25LR256F").
 * Note: For partnames without explicit WP register mask definitions in flashrom (e.g. "gd25lb256f"),
 * return NULL so that the caller falls back to default_fallback_name ("GD25LQ255E").
 */
static const char *map_gd25lq256_partname(const char *partname)
{
	if (!partname)
		return NULL;

	if (!strcasecmp(partname, "gd25lq256h"))
		return "GD25LQ256H";
	if (!strcasecmp(partname, "gd25lq255e"))
		return "GD25LQ255E";

	return NULL;
}

static const char *fixup_resolve_gd25lq256_collision(uint8_t manufacture_id, uint16_t model_id, char *probed_buf, size_t buf_len)
{
	char local_buf[256];
	char *buf = (probed_buf && buf_len > 0) ? probed_buf : local_buf;
	size_t len = (probed_buf && buf_len > 0) ? buf_len : sizeof(local_buf);

	buf[0] = '\0';

	/* 1. Direct environment variable string override (e.g. AP_WPSR_PARTNAME="GD25LQ256H") */
	const char *env_name = getenv("AP_WPSR_PARTNAME");
	if (env_name && env_name[0] != '\0') {
		snprintf(buf, len, "%s", env_name);
		char *trimmed = trim_ws(buf);
		return map_gd25lq256_partname(trimmed);
	}

	/* 2. Probing sysfs files or file path overrides */
	const char *env_file = getenv("AP_WPSR_PARTNAME_FILE");
	const char *sysfs_paths[] = {
		env_file,
		"/sys/class/mtd/mtd0/device/spi-nor/partname",
	};

	for (size_t i = 0; i < ARRAY_SIZE(sysfs_paths); i++) {
		if (!sysfs_paths[i])
			continue;
		FILE *fp = fopen(sysfs_paths[i], "r");
		if (!fp)
			continue;
		char *res = fgets(buf, len, fp);
		fclose(fp);
		if (res) {
			char *trimmed = trim_ws(buf);
			/*
			 * Stop immediately once a partname is read from an override file or sysfs.
			 * Falling through to real sysfs could silently override test intents.
			 */
			return map_gd25lq256_partname(trimmed);
		}
	}
	return NULL;
}

static const struct ap_wpsr_fixup ap_wpsr_fixups[] = {
	{
		.manufacture_id = GIGADEVICE_ID, /* 0xc8 */
		.model_id = GIGADEVICE_GD25LQ255E, /* 0x6019 */
		.resolve_chip_name = fixup_resolve_gd25lq256_collision,
		.default_fallback_name = "GD25LQ255E",
	},
	{ 0, 0, NULL, NULL }
};

static const struct flashchip *find_chip_by_jedec_id(unsigned long long jedec_id)
{
	const struct flashchip *found_chip = NULL;
	int match_count = 0;
	uint8_t manufacture_id;
	uint16_t model_id;

	/*
	 * JEDEC ID is read as a sequence of bytes. It may be preceded by
	 * continuation codes (0x7F). This function parses a 64-bit integer
	 * representation of the ID to find the components.
	 */
	uint8_t jedec_bytes[8];
	for (int i = 0; i < ARRAY_SIZE(jedec_bytes); i++)
		jedec_bytes[i] = (jedec_id >> (56 - i * 8)) & 0xff;

	/*
	 * Find the first non-zero byte to locate the start of the ID, as the
	 * jedec_id from the command line might be shorter than 8 bytes.
	 */
	int first_byte_idx = 0;
	while (first_byte_idx < ARRAY_SIZE(jedec_bytes) && jedec_bytes[first_byte_idx] == 0)
		first_byte_idx++;

	if (first_byte_idx == ARRAY_SIZE(jedec_bytes))
		return NULL;  /* ID is all zeros. */

	/* Skip continuation codes (0x7F) to find the manufacturer ID. */
	int id_start_idx = first_byte_idx;
	while (id_start_idx < ARRAY_SIZE(jedec_bytes) && jedec_bytes[id_start_idx] == 0x7F)
		id_start_idx++;

	/* We need at least 3 bytes for a valid ID (1 for manufacturer, 2 for model). */
	if (ARRAY_SIZE(jedec_bytes) - id_start_idx < 3)
		return NULL;

	/* The first non-0x7F byte is the manufacturer ID. */
	manufacture_id = jedec_bytes[id_start_idx];
	/* The next two bytes are the model ID. */
	model_id = (jedec_bytes[id_start_idx + 1] << 8) | jedec_bytes[id_start_idx + 2];

	/* Check for JEDEC ID collision fixups and runtime resolutions before standard db scan */
	for (const struct ap_wpsr_fixup *f = ap_wpsr_fixups; f && (f->manufacture_id || f->model_id); f++) {
		if (f->manufacture_id == manufacture_id && f->model_id == model_id) {
			char probed_name[256] = {0};
			if (f->resolve_chip_name) {
				const char *resolved = f->resolve_chip_name(manufacture_id, model_id, probed_name, sizeof(probed_name));
				if (resolved) {
					const struct flashchip *c = find_chip_by_name(resolved);
					if (c) {
						printf(" > [fixup] Resolved JEDEC ID 0x%llx (probed partname: '%s') to chip: '%s'\n",
						       jedec_id, probed_name[0] ? probed_name : "unknown", resolved);
						return c;
					}
				}
			}
			if (f->default_fallback_name) {
				const struct flashchip *c = find_chip_by_name(f->default_fallback_name);
				if (c) {
					if (probed_name[0]) {
						printf(" > [fixup] Probed partname '%s' (via env/sysfs) did not match target for JEDEC ID 0x%llx. Using default fallback: '%s'\n",
						       probed_name, jedec_id, f->default_fallback_name);
					} else {
						printf(" > [fixup] Multiple chips share JEDEC ID 0x%llx (no partname detected). Using default fallback: '%s'\n",
						       jedec_id, f->default_fallback_name);
					}
					return c;
				}
			}
			break;
		}
	}

	/*
	 * The Extended Device ID bytes that may follow the model ID are currently
	 * ignored for the purpose of finding a chip match.
	 */
	for (const struct flashchip *chip = flashchips; chip && chip->name; chip++) {
		if (chip->manufacture_id == manufacture_id && chip->model_id == model_id) {
			if (is_chipname_duplicate(chip))
				continue;
			found_chip = chip;
			match_count++;
		}
	}

	if (match_count > 1) {
		fprintf(stderr, "Error: Multiple non-duplicate chips found for JEDEC ID 0x%llx\n",
			jedec_id);
		return NULL;
	}

	return found_chip;
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
	if (feature_bits & FEATURE_4BA_ENTER)
	       printf(" > Can enter/exit 4BA mode with instructions 0xb7/0xe9 w/o WREN\n");
	if (feature_bits & FEATURE_4BA_ENTER_WREN)
		printf(" > Can enter/exit 4BA mode with instructions 0xb7/0xe9 after WREN\n");
	if (feature_bits & FEATURE_4BA_ENTER_EAR7)
		printf(" > Can enter/exit 4BA mode by setting bit7 of the ext addr reg\n");
	if (feature_bits & FEATURE_4BA_EAR_C5C8)
		printf(" > Regular 3-byte operations can be used by writing the most "
		       "significant address byte into an extended address register "
		       "(using 0xc5/0xc8 instructions).\n");
	if (feature_bits & FEATURE_4BA_EAR_1716)
		printf(" > Like FEATURE_4BA_EAR_C5C8 but with 0x17/0x16 instructions.\n");
	if (feature_bits & FEATURE_4BA_READ)
		printf(" > Native 4BA read instruction (0x13) is supported.\n");
	if (feature_bits & FEATURE_4BA_FAST_READ)
		printf(" > Native 4BA fast read instruction (0x0c) is supported.\n");
	if (feature_bits & FEATURE_4BA_WRITE)
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

enum flashrom_wp_result print_wp_regmasks(const struct flashchip *chip, uint32_t wp_start, uint32_t wp_len)
{
	struct registered_master r_mst = {0};
	struct flashctx flash = { .mst = &r_mst };

	flash.chip = (struct flashchip *)chip;

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
			"  One of the following must be specified:\n"
			"    -n, --name=name      Name of chip to calculate SR values for\n"
			"    -j, --jedec_id=id    JEDEC ID of chip to calculate SR values for\n"
			"  -s, --start=addr     Start address of protection range\n"
			"  -l, --length=addr    Length of protection range\n\n"
			"Optional arguments:\n"
			"  -h, --help           Print help and exit\n\n",
			argv[0]);
}

int main(int argc, char* argv[])
{
	char *name = NULL;
	unsigned long long jedec_id = 0;
	uint32_t wp_start = 0, wp_len = 0; /* default */
	bool wp_start_set = false, wp_len_set = false;

	static const char optstr[] = "hn:s:l:j:";
	static const struct option long_options[] = {
		{"help",		0, NULL, 'h'},
		{"name",		1, NULL, 'n'},
		{"jedec_id",		1, NULL, 'j'},
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
			case 'j':
				jedec_id = strtoull(optarg, NULL, 0);
				if (jedec_id == 0) {
					fprintf(stderr, "Error: invalid JEDEC ID '0x0'.\n");
					return 1;
				}
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

	if (!!name == !!jedec_id) {
		fprintf(stderr, "Error: Exactly one of --name or --jedec_id must be provided\n");
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

	const struct flashchip *chip = NULL;
	if (name) {
		printf(" > requested chip name: '%s' with start: 0x%x and len: 0x%x.\n", name, wp_start, wp_len);
		chip = find_chip_by_name(name);
		if (!chip) {
			fprintf(stderr, " no match found for '%s' in chip db.\n", name);
			return 1;
		}
		printf(" > found match '%s' in chip db. (Manufacture: 0x%02x, Model: 0x%04x)\n\n",
		       chip->name, chip->manufacture_id, chip->model_id);
	} else {
		printf(" > requested jedec id: 0x%llx with start: 0x%x and len: 0x%x.\n", jedec_id, wp_start, wp_len);
		chip = find_chip_by_jedec_id(jedec_id);
		if (!chip) {
			fprintf(stderr, " no match found for jedec id 0x%llx in chip db.\n", jedec_id);
			return 1;
		}
		printf(" > found match '%s' in chip db. (Manufacture: 0x%02x, Model: 0x%04x)\n\n",
		       chip->name, chip->manufacture_id, chip->model_id);
	}

	enum flashrom_wp_result ret = print_wp_regmasks(chip, wp_start, wp_len);
	if (ret != FLASHROM_WP_OK) {
		fprintf(stderr, "Error: '%s'\n", get_wp_error_str(ret));
		return 1;
	}

	return 0;
}
