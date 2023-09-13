/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of Google or the names of contributors or
 * licensors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * GOOGLE INC AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * GOOGLE OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF GOOGLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/ioctl.h>
#include <linux/types.h>

#include "flash.h"
#include "cros_ec_commands.h"
#include "cros_ec.h"
#include "programmer.h"

#define CROS_EC_COMMAND_RETRIES	50

static bool g_cros_ec_detected = false;
int cros_ec_fd;		/* File descriptor for kernel device */

/*
 * @version: Command version number (often 0)
 * @command: Command to send (EC_CMD_...)
 * @outsize: Outgoing length in bytes
 * @insize: Max number of bytes to accept from EC
 * @result: EC's response to the command (separate from communication failure)
 * @data: Where to put the incoming data from EC and outgoing data to EC
 */
struct cros_ec_command_v2 {
	uint32_t version;
	uint32_t command;
	uint32_t outsize;
	uint32_t insize;
	uint32_t result;
	uint8_t data[0];
};

#define CROS_EC_DEV_IOC_V2	0xEC
#define CROS_EC_DEV_IOCXCMD_V2	_IOWR(CROS_EC_DEV_IOC_V2, 0, \
				      struct cros_ec_command_v2)

#define CROS_EC_DEV_RETRY	3

/*
 * ec device interface v2
 * (used with upstream kernel as well as with Chrome OS v4.4 and later)
 */

static int command_wait_for_response_v2(void)
{
	uint8_t s_cmd_buf[sizeof(struct cros_ec_command_v2) +
			  sizeof(struct ec_response_get_comms_status)];
	struct cros_ec_command_v2 *s_cmd = (struct cros_ec_command_v2 *)s_cmd_buf;
	struct ec_response_get_comms_status *status = (struct ec_response_get_comms_status *)s_cmd->data;
	int ret;

	s_cmd->version = 0;
	s_cmd->command = EC_CMD_GET_COMMS_STATUS;
	s_cmd->outsize = 0;
	s_cmd->insize = sizeof(*status);

	/*
	 * FIXME: magic delay until we fix the underlying problem (probably in
	 * the kernel driver)
	 */
	usleep(10 * 1000);
	for (int i = 1; i <= CROS_EC_COMMAND_RETRIES; i++) {
		ret = ioctl(cros_ec_fd, CROS_EC_DEV_IOCXCMD_V2, s_cmd_buf);
		if (ret < 0) {
			msg_perr("%s(): CrOS EC command failed: %d, errno=%d\n",
				 __func__, ret, errno);
			ret = -EC_RES_ERROR;
			break;
		}
		if (s_cmd->result) {
			msg_perr("%s(): CrOS EC command failed: result=%d\n",
				 __func__, s_cmd->result);
			ret = -s_cmd->result;
			break;
		}

		if (!(status->flags & EC_COMMS_STATUS_PROCESSING)) {
			ret = -EC_RES_SUCCESS;
			break;
		}

		usleep(1000);
	}

	return ret;
}

static int __cros_ec_command_dev_v2(int command, int version,
			   const void *outdata, int outsize,
			   void *indata, int insize)
{
	const size_t size = sizeof(struct cros_ec_command_v2) + max(outsize, insize);

	assert(outsize == 0 || outdata != NULL);
	assert(insize == 0 || indata != NULL);

	struct cros_ec_command_v2 *s_cmd = malloc(size);
	if (s_cmd == NULL)
		return -EC_RES_ERROR;

	s_cmd->command = command;
	s_cmd->version = version;
	s_cmd->result = 0xff;
	s_cmd->outsize = outsize;
	s_cmd->insize = insize;
	memcpy(s_cmd->data, outdata, outsize);

	int ret = ioctl(cros_ec_fd, CROS_EC_DEV_IOCXCMD_V2, s_cmd, size);
	if (ret < 0 && errno == EAGAIN) {
		ret = command_wait_for_response_v2();
		s_cmd->result = 0;
	}
	if (ret < 0) {
		msg_perr("%s(): Command 0x%04x failed: %d, errno=%d\n",
			__func__, command, ret, errno);
		free(s_cmd);
		return -EC_RES_ERROR;
	}
	if (s_cmd->result) {
		msg_pdbg("%s(): Command 0x%04x returned result: %d\n",
			 __func__, command, s_cmd->result);
		ret = -s_cmd->result;
		free(s_cmd);
		return ret;
	}

	memcpy(indata, s_cmd->data, min(ret, insize));
	free(s_cmd);
	return min(ret, insize);
}

/*
 * cros_ec_command - Issue command to CROS_EC device with retry
 *
 * @command:	command code
 * @outdata:	data to send to EC
 * @outsize:	number of bytes in outbound payload
 * @indata:	(unallocated) buffer to store data received from EC
 * @insize:	number of bytes in inbound payload
 *
 * This uses the kernel Chrome OS EC driver to communicate with the EC.
 *
 * The outdata and indata buffers contain payload data (if any); command
 * and response codes as well as checksum data are handled transparently by
 * this function.
 *
 * Returns >=0 for success, or negative if other error.
 */
int cros_ec_command(int command, int version,
			   const void *outdata, int outsize,
			   void *indata, int insize)
{
	int ret = EC_RES_ERROR;
	int attempt;

	for (attempt = 0; attempt < CROS_EC_DEV_RETRY; attempt++) {
		ret = __cros_ec_command_dev_v2(command, version, outdata,
					       outsize, indata, insize);
		if (ret >= 0)
			return ret;
	}

	return ret;
}

static void cros_ec_set_max_size(struct opaque_master *op)
{
	struct ec_response_get_protocol_info info;

	msg_pdbg("%s: sending protoinfo command\n", __func__);
	int rc = cros_ec_command(EC_CMD_GET_PROTOCOL_INFO, 0, NULL, 0,
			      &info, sizeof(info));
	msg_pdbg("%s: rc:%d\n", __func__, rc);

	/*
	 * Use V3 large size only if v2 protocol is not supported.
	 * When v2 is supported, we may be using a kernel without v3 support,
	 * leading to sending larger commands the kernel can support.
	 */
	if (rc == sizeof(info) && ((info.protocol_versions & (1<<2)) == 0)) {

		op->max_data_write = info.max_request_packet_size -
			sizeof(struct ec_host_request);
		op->max_data_read = info.max_response_packet_size -
			sizeof(struct ec_host_response);
		/*
		 * Due to a bug in NPCX SPI code (chromium:725580),
		 * The EC may responds 163 when it meant 160; it should not
		 * have included header and footer.
		 */
		op->max_data_read &= ~3;
		msg_pdbg("%s: max_write:%d max_read:%d\n", __func__,
			 op->max_data_write, op->max_data_read);
	}
}

/* perform basic "hello" test to see if we can talk to the EC */
static int cros_ec_test(void)
{
	struct ec_params_hello request;
	struct ec_response_hello response;

	/* Say hello to EC. */
	request.in_data = 0xf0e0d0c0;  /* Expect EC will add on 0x01020304. */
	msg_pdbg("%s: sending HELLO request with 0x%08x\n",
	         __func__, request.in_data);
	int rc = cros_ec_command(EC_CMD_HELLO, 0, &request,
			     sizeof(request), &response, sizeof(response));
	msg_pdbg("%s: response: 0x%08x\n", __func__, response.out_data);

	if (rc < 0 || response.out_data != 0xf1e2d3c4) {
		msg_pdbg("response.out_data is not 0xf1e2d3c4.\n"
		         "rc=%d, request=0x%x response=0x%x\n",
		         rc, request.in_data, response.out_data);
		return 1;
	}

	return 0;
}

static struct opaque_master opaque_master_cros_ec_dev = {
	.max_data_read	= 128,
	.max_data_write	= 128,
	.probe		= cros_ec_probe_size,
	.read		= cros_ec_read,
	.write		= cros_ec_write,
	.erase		= cros_ec_block_erase,
	.wp_read_cfg    = cros_ec_wp_read_cfg,
	.wp_write_cfg   = cros_ec_wp_write_cfg,
	.wp_get_ranges  = cros_ec_wp_get_available_ranges,
	.data 		= NULL
};

static int cros_ec_dev_shutdown(void *data)
{
	close(cros_ec_fd);
	return 0;
}

/* ugly singleton to work around cros layering violations in action_descriptor.c */
bool programming_ec(void)
{
	/* Programmer is EC so toggle ec-alias path detection on. */
	return g_cros_ec_detected;
}

static int cros_ec_init(const struct programmer_cfg *cfg)
{
	const char *dev_path = "/dev/cros_ec";
	msg_pdbg("%s: probing for CROS_EC at %s\n", __func__, dev_path);
	cros_ec_fd = open(dev_path, O_RDWR);
	if (cros_ec_fd < 0)
		return cros_ec_fd;

	if (cros_ec_test())
		return 1;

	cros_ec_set_max_size(&opaque_master_cros_ec_dev);

	msg_pdbg("CROS_EC detected at %s\n", dev_path);
	register_opaque_master(&opaque_master_cros_ec_dev, NULL);
	register_shutdown(cros_ec_dev_shutdown, NULL);
	g_cros_ec_detected = true;

	return 0;
}

const struct programmer_entry programmer_cros_ec = {
	.name			= "ec",
	.type			= OTHER,
	.devs.note		= "Google EC.\n",
	.init			= cros_ec_init,
};
