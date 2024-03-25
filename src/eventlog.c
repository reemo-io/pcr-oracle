/*
 *   Copyright (C) 2022, 2023 SUSE LLC
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Written by Olaf Kirch <okir@suse.com>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <assert.h>

#include <tss2/tss2_tpm2_types.h>

#include "eventlog.h"
#include "bufparser.h"
#include "runtime.h"
#include "digest.h"
#include "util.h"
#include "uapi.h"
#include "sd-boot.h"

#define TPM_EVENT_LOG_MAX_ALGOS		64

struct tpm_event_log_reader {
	int			fd;
	unsigned int		tpm_version;
	unsigned int		event_count;

	struct tpm_event_log_tcg2_info {
		uint32_t		platform_class;
		uint8_t			spec_version_major;
		uint8_t			spec_version_minor;
		uint8_t			spec_errata;
		uint8_t			uintn_size;

		tpm_algo_info_t		algorithms[TPM_EVENT_LOG_MAX_ALGOS];
	} tcg2_info;

	struct {
		bool		valid_pcr0_locality;
		uint8_t		pcr0_locality;
	} tpm_startup;
};


static bool		__tpm_event_parse_tcg2_info(tpm_event_t *ev, struct tpm_event_log_tcg2_info *info);


static void
__read_exactly(int fd, void *vp, unsigned int len)
{
	int n;

	if ((n = read(fd, vp, len)) < 0)
		fatal("unable to read from event log: %m\n");
	if (n != len)
		fatal("short read from event log (premature EOF)\n");
}

static void
__read_u32le(int fd, uint32_t *vp)
{
	__read_exactly(fd, vp, sizeof(*vp));
	*vp = le32toh(*vp);
}

static void
__read_u16le(int fd, uint16_t *vp)
{
	__read_exactly(fd, vp, sizeof(*vp));
	*vp = le16toh(*vp);
}

static bool
__read_u32le_or_eof(int fd, uint32_t *vp)
{
	int n;

	if ((n = read(fd, vp, 4)) < 0)
		fatal("unable to read from event log: %m\n");
	if (n == 0)
		return false;

	if (n != 4)
		fatal("short read from event log (premature EOF)\n");
	*vp = le32toh(*vp);
	return true;
}

static const tpm_algo_info_t *
event_log_get_algo_info(tpm_event_log_reader_t *log, unsigned int algo_id)
{
	const tpm_algo_info_t *algo;

	if (!(algo = digest_by_tpm_alg(algo_id)))
		algo = __digest_by_tpm_alg(algo_id, log->tcg2_info.algorithms, TPM_EVENT_LOG_MAX_ALGOS);
	return algo;
}

tpm_event_log_reader_t *
event_log_open(const char *override_path)
{
	tpm_event_log_reader_t *log;

	log = calloc(1, sizeof(*log));
	log->tpm_version = 1;
	log->fd = runtime_open_eventlog(override_path);
	if (log->fd < 0) {
		event_log_close(log);
		return NULL;
	}

	return log;
}

void
event_log_close(tpm_event_log_reader_t *log)
{
	if (log->fd >= 0) {
		close(log->fd);
		log->fd = -1;
	}
	free(log);
}

static void
event_log_read_digest(tpm_event_log_reader_t *log, tpm_evdigest_t *dgst, int tpm_hash_algo_id)
{
	const tpm_algo_info_t *algo;

	if (!(algo = event_log_get_algo_info(log, tpm_hash_algo_id)))
		fatal("Unable to handle event log entry for unknown hash algorithm %u\n", tpm_hash_algo_id);

	__read_exactly(log->fd, dgst->data, algo->digest_size);

	dgst->algo = algo;
	dgst->size = algo->digest_size;
}

static void
event_log_resize_pcrs(tpm_event_t *ev, unsigned int count)
{
	if (count > 32)
		fatal("Bad number of PCRs in TPM event record (%u)\n", count);

	ev->pcr_values = calloc(count, sizeof(tpm_evdigest_t));
	if (ev->pcr_values == NULL)
		fatal("out of memory");
	ev->pcr_count = count;
}

static void
event_log_read_pcrs_tpm1(tpm_event_log_reader_t *log, tpm_event_t *ev)
{
	event_log_resize_pcrs(ev, 1);
	event_log_read_digest(log, &ev->pcr_values[0], TPM2_ALG_SHA1);
}

static void
event_log_read_pcrs_tpm2(tpm_event_log_reader_t *log, tpm_event_t *ev)
{
	uint32_t i, count;

	__read_u32le(log->fd, &count);
	event_log_resize_pcrs(ev, count);

	for (i = 0; i < count; ++i) {
		uint16_t algo_id;

		__read_u16le(log->fd, &algo_id);
		event_log_read_digest(log, &ev->pcr_values[i], algo_id);
	}
}

unsigned int
event_log_get_event_count(const tpm_event_log_reader_t *log)
{
	return log->event_count;
}

unsigned int
event_log_get_tpm_version(const tpm_event_log_reader_t *log)
{
	return log->tpm_version;
}

tpm_event_t *
event_log_read_next(tpm_event_log_reader_t *log)
{
	tpm_event_t *ev;
	uint32_t event_size;

again:
	ev = calloc(1, sizeof(*ev));

	if (!__read_u32le_or_eof(log->fd, &ev->pcr_index)) {
		free(ev);
		return NULL;
	}

	__read_u32le(log->fd, &ev->event_type);

	ev->file_offset = lseek(log->fd, 0, SEEK_CUR);

	if (log->tpm_version == 1) {
		event_log_read_pcrs_tpm1(log, ev);
	} else {
		event_log_read_pcrs_tpm2(log, ev);
	}

	__read_u32le(log->fd, &event_size);
	if (event_size > 1024*1024)
		fatal("Oversized TPM2 event log entry with %u bytes of data\n", event_size);

	ev->event_data = calloc(1, event_size);
	ev->event_size = event_size;
	__read_exactly(log->fd, ev->event_data, event_size);


	if (ev->event_type == TPM2_EVENT_NO_ACTION && ev->pcr_index == 0 && log->event_count == 0
	 && ev->event_size >= 16) {
		char *signature = (char *) ev->event_data;

		if (!strncmp(signature, "Spec ID Event03", 16)) {
			debug("Detected TPMv2 event log\n");

			if (!__tpm_event_parse_tcg2_info(ev, &log->tcg2_info))
				fatal("Unable to parse TCG2 magic event header");

			log->tpm_version = log->tcg2_info.spec_version_major;
			free(ev);
			goto again;
		} else
		if (!memcmp(signature, "StartupLocality", 16) && ev->event_size == 17) {
			log->tpm_startup.valid_pcr0_locality = true;
			log->tpm_startup.pcr0_locality = ((unsigned char *) signature)[16];
			free(ev);
			goto again;
		}
	}

	ev->event_index = log->event_count++;
	return ev;
}

bool
event_log_get_locality(tpm_event_log_reader_t *log, unsigned int pcr_index, uint8_t *loc_p)
{
	if (pcr_index != 0)
		return false;
	if (!log->tpm_startup.valid_pcr0_locality)
		return false;

	*loc_p = log->tpm_startup.pcr0_locality;
	return true;
}

/*
 * TCGv2 defines a "magic event" record that conveys some additional information
 * on where the log was created, the hash sizes for the algorithms etc.
 */
static bool
__tpm_event_parse_tcg2_info(tpm_event_t *ev, struct tpm_event_log_tcg2_info *info)
{
	buffer_t buf;
	uint32_t i, algo_info_count;

	buffer_init_read(&buf, ev->event_data, ev->event_size);

	/* skip over magic signature string */
	buffer_skip(&buf, 16);

	if (!buffer_get_u32le(&buf, &info->platform_class)
	 || !buffer_get_u8(&buf, &info->spec_version_minor)
	 || !buffer_get_u8(&buf, &info->spec_version_major)
	 || !buffer_get_u8(&buf, &info->spec_errata)
	 || !buffer_get_u8(&buf, &info->uintn_size)
	 || !buffer_get_u32le(&buf, &algo_info_count)
	   )
		return false;

	for (i = 0; i < algo_info_count; ++i) {
		uint16_t algo_id, algo_size;
		const tpm_algo_info_t *wk;

		if (!buffer_get_u16le(&buf, &algo_id)
		 || !buffer_get_u16le(&buf, &algo_size))
			return false;

		if (algo_id > TPM2_ALG_LAST)
			continue;

		if ((wk = digest_by_tpm_alg(algo_id)) == NULL) {
			char fake_name[32];

			snprintf(fake_name, sizeof(fake_name), "TPM2_ALG_%u", algo_id);
			info->algorithms[algo_id].digest_size = algo_size;
			info->algorithms[algo_id].openssl_name = strdup(fake_name);
		} else if (wk->digest_size != algo_size) {
			fprintf(stderr, "Conflicting digest sizes for %s: %u versus %u\n",
					wk->openssl_name, wk->digest_size, algo_size);
		} else
			/* NOP */ ;
	}

	return true;
}

const char *
tpm_event_type_to_string(unsigned int event_type)
{
	static char buffer[16];

	switch (event_type) {
	case TPM2_EVENT_PREBOOT_CERT:
		return "EVENT_PREBOOT_CERT";
	case TPM2_EVENT_POST_CODE:
		return "EVENT_POST_CODE";
	case TPM2_EVENT_UNUSED:
		return "EVENT_UNUSED";
	case TPM2_EVENT_NO_ACTION:
		return "EVENT_NO_ACTION";
	case TPM2_EVENT_SEPARATOR:
		return "EVENT_SEPARATOR";
	case TPM2_EVENT_ACTION:
		return "EVENT_ACTION";
	case TPM2_EVENT_EVENT_TAG:
		return "EVENT_EVENT_TAG";
	case TPM2_EVENT_S_CRTM_CONTENTS:
		return "EVENT_S_CRTM_CONTENTS";
	case TPM2_EVENT_S_CRTM_VERSION:
		return "EVENT_S_CRTM_VERSION";
	case TPM2_EVENT_CPU_MICROCODE:
		return "EVENT_CPU_MICROCODE";
	case TPM2_EVENT_PLATFORM_CONFIG_FLAGS:
		return "EVENT_PLATFORM_CONFIG_FLAGS";
	case TPM2_EVENT_TABLE_OF_DEVICES:
		return "EVENT_TABLE_OF_DEVICES";
	case TPM2_EVENT_COMPACT_HASH:
		return "EVENT_COMPACT_HASH";
	case TPM2_EVENT_IPL:
		return "EVENT_IPL";
	case TPM2_EVENT_IPL_PARTITION_DATA:
		return "EVENT_IPL_PARTITION_DATA";
	case TPM2_EVENT_NONHOST_CODE:
		return "EVENT_NONHOST_CODE";
	case TPM2_EVENT_NONHOST_CONFIG:
		return "EVENT_NONHOST_CONFIG";
	case TPM2_EVENT_NONHOST_INFO:
		return "EVENT_NONHOST_INFO";
	case TPM2_EVENT_OMIT_BOOT_DEVICE_EVENTS:
		return "EVENT_OMIT_BOOT_DEVICE_EVENTS";
	case TPM2_EFI_EVENT_BASE:
		return "EFI_EVENT_BASE";
	case TPM2_EFI_VARIABLE_DRIVER_CONFIG:
		return "EFI_VARIABLE_DRIVER_CONFIG";
	case TPM2_EFI_VARIABLE_BOOT:
		return "EFI_VARIABLE_BOOT";
	case TPM2_EFI_BOOT_SERVICES_APPLICATION:
		return "EFI_BOOT_SERVICES_APPLICATION";
	case TPM2_EFI_BOOT_SERVICES_DRIVER:
		return "EFI_BOOT_SERVICES_DRIVER";
	case TPM2_EFI_RUNTIME_SERVICES_DRIVER:
		return "EFI_RUNTIME_SERVICES_DRIVER";
	case TPM2_EFI_GPT_EVENT:
		return "EFI_GPT_EVENT";
	case TPM2_EFI_ACTION:
		return "EFI_ACTION";
	case TPM2_EFI_PLATFORM_FIRMWARE_BLOB:
		return "EFI_PLATFORM_FIRMWARE_BLOB";
	case TPM2_EFI_HANDOFF_TABLES:
		return "EFI_HANDOFF_TABLES";
	case TPM2_EFI_PLATFORM_FIRMWARE_BLOB2:
		return "EFI_PLATFORM_FIRMWARE_BLOB2";
	case TPM2_EFI_HANDOFF_TABLES2:
		return "EFI_HANDOFF_TABLES2";
	case TPM2_EFI_VARIABLE_BOOT2:
		return "EFI_VARIABLE_BOOT2";
	case TPM2_EFI_HCRTM_EVENT:
		return "EFI_HCRTM_EVENT";
	case TPM2_EFI_VARIABLE_AUTHORITY:
		return "EFI_VARIABLE_AUTHORITY";
	case TPM2_EFI_SPDM_FIRMWARE_BLOB:
		return "EFI_SPDM_FIRMWARE_BLOB";
	case TPM2_EFI_SPDM_FIRMWARE_CONFIG:
		return "EFI_SPDM_FIRMWARE_CONFIG";
	}

	snprintf(buffer, sizeof(buffer), "0x%x", event_type);
	return buffer;
}

const tpm_evdigest_t *
tpm_event_get_digest(const tpm_event_t *ev, const tpm_algo_info_t *algo_info)
{
	unsigned int i;

	for (i = 0; i < ev->pcr_count; ++i) {
		const tpm_evdigest_t *md = &ev->pcr_values[i];

		if (md->algo == algo_info)
			return md;
	}

	return NULL;
}

void
tpm_event_print(tpm_event_t *ev)
{
	__tpm_event_print(ev, (void (*)(const char *, ...)) printf);
}

void
__tpm_event_print(tpm_event_t *ev, tpm_event_bit_printer *print_fn)
{
	unsigned int i;

	print_fn("%05lx: event type=%s pcr=%d digests=%d data=%u bytes\n",
			ev->file_offset,
			tpm_event_type_to_string(ev->event_type),
			ev->pcr_index, ev->pcr_count, ev->event_size);

	if (ev->__parsed)
		tpm_parsed_event_print(ev->__parsed, print_fn);

	for (i = 0; i < ev->pcr_count; ++i) {
		const tpm_evdigest_t *d = &ev->pcr_values[i];

		print_fn("  %-10s %s\n", d->algo->openssl_name, digest_print_value(d));
	}

	print_fn("  Data:\n");
	hexdump(ev->event_data, ev->event_size, print_fn, 8);
}

static const tpm_evdigest_t *
__tpm_event_rehash_efi_variable(const char *var_name, tpm_event_log_rehash_ctx_t *ctx)
{
	const tpm_evdigest_t *md;
	buffer_t *data;

	data = runtime_read_efi_variable(var_name);
	if (data == NULL) {
		error("Unable to read EFI variable %s\n", var_name);
		return NULL;
	}

	md = digest_buffer(ctx->algo, data);
	buffer_free(data);
	return md;
}

static tpm_parsed_event_t *
tpm_parsed_event_new(unsigned int event_type)
{
	tpm_parsed_event_t *parsed;

	parsed = calloc(1, sizeof(*parsed));
	parsed->event_type = event_type;
	return parsed;
}

static void
tpm_parsed_event_free(tpm_parsed_event_t *parsed)
{
	if (parsed->destroy)
		parsed->destroy(parsed);
	memset(parsed, 0, sizeof(*parsed));
	free(parsed);
}

const char *
tpm_parsed_event_describe(tpm_parsed_event_t *parsed)
{
	if (!parsed)
		return NULL;

	if (!parsed->describe)
		return tpm_event_type_to_string(parsed->event_type);

	return parsed->describe(parsed);
}

void
tpm_parsed_event_print(tpm_parsed_event_t *parsed, tpm_event_bit_printer *print_fn)
{
	if (!parsed)
		return;
	if (parsed->print)
		parsed->print(parsed, print_fn);
	else if (parsed->describe)
		print_fn("  %s\n", parsed->describe(parsed));
}

buffer_t *
tpm_parsed_event_rebuild(tpm_parsed_event_t *parsed, const void *raw_data, unsigned int raw_data_len)
{
	if (parsed && parsed->rebuild)
		return parsed->rebuild(parsed, raw_data, raw_data_len);

	return NULL;
}

const tpm_evdigest_t *
tpm_parsed_event_rehash(const tpm_event_t *ev, const tpm_parsed_event_t *parsed, tpm_event_log_rehash_ctx_t *ctx)
{
	if (!parsed)
		return NULL;

	if (parsed->rehash)
		return parsed->rehash(ev, parsed, ctx);

	return NULL;
}

const char *
tpm_event_decode_uuid(const unsigned char *data)
{
	static char uuid[64];
	uint32_t w0;
	uint16_t hw0, hw1;

	w0 = le32toh(((uint32_t *) data)[0]);
	hw0 = le32toh(((uint16_t *) data)[2]);
	hw1 = le32toh(((uint16_t *) data)[3]);
	snprintf(uuid, sizeof(uuid), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			w0, hw0, hw1,
			data[8], data[9],
			data[10], data[11], data[12],
			data[13], data[14], data[15]
			);
	return uuid;
}

/*
 * For files residing on the EFI partition, grub usually formats these as
 * (hdX,gptY)/EFI/BOOT/some.file
 * Once it has determined the final root device, the device part will be
 * omitted (eg for kernel and initrd).
 */
static bool
__grub_file_parse(grub_file_t *grub_file, const char *value)
{
	if (value[0] == '/') {
		grub_file->device = NULL;
		grub_file->path = strdup(value);
	} else if (value[0] == '(') {
		char *copy = strdup(value);
		char *path;

		if ((path = strchr(copy, ')')) == NULL) {
			free(copy);
			return false;
		}

		*path++ = '\0';

		grub_file->device = strdup(copy + 1);
		grub_file->path = strdup(path);
		free(copy);
	} else {
		return false;
	}

	return true;
}

static const char *
__grub_file_join(grub_file_t grub_file)
{
	static char path[PATH_MAX];

	if (grub_file.device == NULL)
		snprintf(path, sizeof(path), "%s", grub_file.path);
	else
		snprintf(path, sizeof(path), "(%s)%s", grub_file.device, grub_file.path);

	return path;
}

static void
__grub_file_destroy(grub_file_t *grub_file)
{
	drop_string(&grub_file->device);
	drop_string(&grub_file->path);
}

/*
 * Handle IPL events, which grub2 and sd-boot uses to hide its stuff in
 */
static void
__tpm_event_grub_file_destroy(tpm_parsed_event_t *parsed)
{
	__grub_file_destroy(&parsed->grub_file);
}

const char *
__tpm_event_grub_file_describe(const tpm_parsed_event_t *parsed)
{
	static char buffer[1024];

	snprintf(buffer, sizeof(buffer), "grub2 file load from %s", __grub_file_join(parsed->grub_file));
	return buffer;
}


static const tpm_evdigest_t *
__tpm_event_grub_file_rehash(const tpm_event_t *ev, const tpm_parsed_event_t *parsed, tpm_event_log_rehash_ctx_t *ctx)
{
	const grub_file_event *evspec = &parsed->grub_file;
	const tpm_evdigest_t *md = NULL;

	debug("  re-hashing %s\n", __tpm_event_grub_file_describe(parsed));
	if (evspec->device == NULL || !strcmp(evspec->device, "crypto0")) {
		debug("  assuming the file resides on system partition\n");
		md = runtime_digest_rootfs_file(ctx->algo, evspec->path);
	} else {
		if (sdb_is_boot_entry(evspec->path) && ctx->boot_entry_path) {
			debug("  getting different boot entry file from EFI boot partition: %s\n",
			      ctx->boot_entry_path);
			md = runtime_digest_rootfs_file(ctx->algo, ctx->boot_entry_path);
		} else
		if (sdb_is_kernel(evspec->path) && ctx->boot_entry) {
			debug("  getting different kernel from EFI boot partition: %s\n",
			      ctx->boot_entry->image_path);
			md = runtime_digest_efi_file(ctx->algo, ctx->boot_entry->image_path);
		} else
		if (sdb_is_initrd(evspec->path) && ctx->boot_entry) {
			debug("  getting different initrd from EFI boot partition: %s\n",
			      ctx->boot_entry->initrd_path);
			md = runtime_digest_efi_file(ctx->algo, ctx->boot_entry->initrd_path);
		} else {
			debug("  assuming the file resides on EFI boot partition\n");
			md = runtime_digest_efi_file(ctx->algo, evspec->path);
		}
	}

	return md;
}

static bool
__tpm_event_grub_file_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed, const char *value)
{
	if (!__grub_file_parse(&parsed->grub_file, value))
		return false;

	parsed->event_subtype = GRUB_EVENT_FILE;
	parsed->destroy = __tpm_event_grub_file_destroy;
	parsed->rehash = __tpm_event_grub_file_rehash;
	parsed->describe = __tpm_event_grub_file_describe;

	return true;
}

static void
__tpm_event_grub_command_destroy(tpm_parsed_event_t *parsed)
{
	int argc;

	drop_string(&parsed->grub_command.string);
	for (argc = 0; argc < GRUB_COMMAND_ARGV_MAX; argc++)
		drop_string(&parsed->grub_command.argv[argc]);
}

static const char *
__tpm_event_grub_command_describe(const tpm_parsed_event_t *parsed)
{
	static char buffer[128];
	static char *topic = NULL;

	switch (parsed->event_subtype) {
	case GRUB_EVENT_COMMAND:
		topic = "grub2 command";
		break;
	case GRUB_EVENT_COMMAND_LINUX:
		topic = "grub2 linux command";
		break;
	case GRUB_EVENT_COMMAND_INITRD:
		topic = "grub2 initrd command";
		break;
	case GRUB_EVENT_KERNEL_CMDLINE:
		topic = "grub2 kernel cmdline";
		break;
	}

	snprintf(buffer, sizeof(buffer), "%s \"%s\"", topic, parsed->grub_command.string);

	return buffer;
}

static const tpm_evdigest_t *
__tpm_event_grub_command_rehash(const tpm_event_t *ev, const tpm_parsed_event_t *parsed, tpm_event_log_rehash_ctx_t *ctx)
{
	char *str = NULL;
	size_t sz = 0;
	const tpm_evdigest_t *digest = NULL;
	grub_file_t file;

	switch (parsed->event_subtype) {
	case GRUB_EVENT_COMMAND:
		str = strdup(parsed->grub_command.string);
		break;
	case GRUB_EVENT_COMMAND_LINUX:
		if (ctx->boot_entry && parsed->grub_command.file.path) {
			file = (grub_file_t) {
				.device = parsed->grub_command.file.device,
				.path = ctx->boot_entry->image_path,
			};
			sz = snprintf(NULL, 0, "linux %s %s", __grub_file_join(file), ctx->boot_entry->options);
			str = malloc(sz + 1);
			snprintf(str, sz + 1, "linux %s %s", __grub_file_join(file), ctx->boot_entry->options);
			debug("Hashed linux command: %s\n", str);
		} else
			str = strdup(parsed->grub_command.string);
		break;
	case GRUB_EVENT_COMMAND_INITRD:
		if (ctx->boot_entry && parsed->grub_command.file.path) {
			file = (grub_file_t) {
				.device = parsed->grub_command.file.device,
				.path = ctx->boot_entry->initrd_path,
			};
			sz = snprintf(NULL, 0, "initrd %s", __grub_file_join(file));
			str = malloc(sz + 1);
			snprintf(str, sz + 1, "initrd %s", __grub_file_join(file));
			debug("Hashed initrd command: %s\n", str);
		} else
			str = strdup(parsed->grub_command.string);
		break;
	case GRUB_EVENT_KERNEL_CMDLINE:
		if (ctx->boot_entry && parsed->grub_command.file.path) {
			file = (grub_file_t) {
				.device = parsed->grub_command.file.device,
				.path = ctx->boot_entry->image_path,
			};
			sz = snprintf(NULL, 0, "%s %s", __grub_file_join(file), ctx->boot_entry->options);
			str = malloc(sz + 1);
			snprintf(str, sz + 1, "%s %s", __grub_file_join(file), ctx->boot_entry->options);
			debug("Hashed kernel cmdline: %s\n", str);
		} else
			str = strdup(parsed->grub_command.string);
		break;
	}

	if (str) {
		digest = digest_compute(ctx->algo, str, strlen(str));
		free(str);
	}

	return digest;
}

/*
 * This event holds stuff like
 *  grub_cmd: ....
 *  kernel_cmdline: ...
 */
static bool
__tpm_event_grub_command_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed, const char *value)
{
	unsigned int wordlen;
	char *copy, *keyword, *arg, *s, cc;
	int argc;

	/* clear argv */
	memset(&parsed->grub_command, 0, sizeof(parsed->grub_command));

	for (wordlen = 0; (cc = value[wordlen]) && (isalpha(cc) || cc == '_'); ++wordlen)
		;

	if (value[wordlen] != ':' || value[wordlen + 1] != ' ')
		return false;

	copy = strdup(value);
	copy[wordlen++] = '\0';
	copy[wordlen++] = '\0';

	keyword = copy;
	arg = copy + wordlen;

	if (!strcmp(keyword, "grub_cmd") && !strncmp(arg, "linux", strlen("linux"))) {
		for (wordlen = 0; (cc = arg[wordlen]) && (cc != ' '); ++wordlen)
			;
		if (arg[wordlen] == ' ' && !__grub_file_parse(&parsed->grub_command.file, arg + wordlen + 1))
			goto failed;
		parsed->event_subtype = GRUB_EVENT_COMMAND_LINUX;
	} else
	if (!strcmp(keyword, "grub_cmd") && !strncmp(arg, "initrd", strlen("initrd"))) {
		for (wordlen = 0; (cc = arg[wordlen]) && (cc != ' '); ++wordlen)
			;
		if (arg[wordlen] == ' ' && !__grub_file_parse(&parsed->grub_command.file, arg + wordlen + 1))
			goto failed;
		parsed->event_subtype = GRUB_EVENT_COMMAND_INITRD;
	} else
	if (!strcmp(keyword, "grub_cmd")) {
		parsed->event_subtype = GRUB_EVENT_COMMAND;
	} else
	if (!strcmp(keyword, "kernel_cmdline")) {
		if (!__grub_file_parse(&parsed->grub_command.file, arg))
			goto failed;
		parsed->event_subtype = GRUB_EVENT_KERNEL_CMDLINE;
	} else
		goto failed;

	parsed->grub_command.string = strdup(arg);
	for (argc = 0, s = strtok(arg, " \t"); s && argc < GRUB_COMMAND_ARGV_MAX - 1; s = strtok(NULL, " \t")) {
		parsed->grub_command.argv[argc++] = strdup(s);
		parsed->grub_command.argv[argc] = NULL;
	}

	parsed->destroy = __tpm_event_grub_command_destroy;
	parsed->rehash = __tpm_event_grub_command_rehash;
	parsed->describe = __tpm_event_grub_command_describe;

	free(copy);
	return true;

failed:
	free(copy);
	return false;
}

static void
__tpm_event_shim_destroy(tpm_parsed_event_t *parsed)
{
	drop_string(&parsed->shim_event.string);
}

static const char *
__tpm_event_shim_describe(const tpm_parsed_event_t *parsed)
{
	static char buffer[64];

	snprintf(buffer, sizeof(buffer), "shim loader %s event", parsed->shim_event.string);
	return buffer;
}

static const tpm_evdigest_t *
__tpm_event_shim_rehash(const tpm_event_t *ev, const tpm_parsed_event_t *parsed, tpm_event_log_rehash_ctx_t *ctx)
{
	if (parsed->event_subtype == SHIM_EVENT_VARIABLE)
		return __tpm_event_rehash_efi_variable(parsed->shim_event.efi_variable, ctx);
	return NULL;
}

/*
 * This event holds stuff like
 *  grub_cmd: ....
 *  kernel_cmdline: ...
 */
static bool
__tpm_event_shim_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed, const char *value)
{
	struct shim_event *evspec = &parsed->shim_event;
	const char *shim_rt_var;

	shim_rt_var = shim_variable_get_full_rtname(value);
	if (shim_rt_var != NULL) {
		parsed->event_subtype = SHIM_EVENT_VARIABLE;
		assign_string(&evspec->efi_variable, shim_rt_var);
	} else {
		error("Unknown shim IPL event %s\n", value);
		return NULL;
	}

	evspec->string = strdup(value);

	parsed->destroy = __tpm_event_shim_destroy;
	parsed->rehash = __tpm_event_shim_rehash;
	parsed->describe = __tpm_event_shim_describe;

	return true;
}

static void
__tpm_event_systemd_destroy(tpm_parsed_event_t *parsed)
{
	drop_string(&parsed->systemd_event.string);
}

static const char *
__tpm_event_systemd_describe(const tpm_parsed_event_t *parsed)
{
	static char buffer[1024];
	char data[768];
	unsigned int len;

	/* It is in UTF16, and also include two '\0' at the end */
	len = parsed->systemd_event.len >> 1;
	if (len > sizeof(data))
		len = sizeof(data);
	__convert_from_utf16le(parsed->systemd_event.string, parsed->systemd_event.len, data, len);
	data[len] = '\0';

	snprintf(buffer, sizeof(buffer), "systemd boot event %s", data);
	return buffer;
}

static const tpm_evdigest_t *
__tpm_event_systemd_rehash(const tpm_event_t *ev, const tpm_parsed_event_t *parsed, tpm_event_log_rehash_ctx_t *ctx)
{
	const uapi_boot_entry_t *boot_entry = ctx->boot_entry;
	char cmdline[2048];
	char cmdline_utf16[4096];
	unsigned int len;

	/* If no --next-kernel option was given, do not rehash anything */
	if (boot_entry == NULL)
		return tpm_event_get_digest(ev, ctx->algo);

	if (!boot_entry->image_path) {
		error("Unable to identify the next kernel\n");
		return NULL;
	}

	debug("Next boot entry expected from: %s %s\n", boot_entry->title, boot_entry->version? : "");
	snprintf(cmdline, sizeof(cmdline), "initrd=%s %s",
			path_unix2dos(boot_entry->initrd_path),
			boot_entry->options? : "");
	debug("Measuring Kernel command line: %s\n", cmdline);

	len = (strlen(cmdline) + 1) << 1;
	assert(len <= sizeof(cmdline_utf16));
	__convert_to_utf16le(cmdline, strlen(cmdline) + 1, cmdline_utf16, len);

	return digest_compute(ctx->algo, cmdline_utf16, len);
}

/*
 * This event holds stuff like
 *  initrd = ....
 */
static bool
__tpm_event_systemd_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed, const char *value, unsigned int len)
{
	struct systemd_event *evspec = &parsed->systemd_event;

	evspec->len = len;
	evspec->string = malloc(len);
	memcpy(evspec->string, value, len);

	parsed->event_subtype = SYSTEMD_EVENT_VARIABLE;
	parsed->destroy = __tpm_event_systemd_destroy;
	parsed->rehash = __tpm_event_systemd_rehash;
	parsed->describe = __tpm_event_systemd_describe;

	return true;
}


static void
__tpm_event_tag_destroy(tpm_parsed_event_t *parsed)
{
}

static const char *
__tpm_event_tag_options_describe(const tpm_parsed_event_t *parsed)
{
	return "Kernel command line (measured by the kernel)";
}

static const tpm_evdigest_t *
__tpm_event_tag_options_rehash(const tpm_event_t *ev, const tpm_parsed_event_t *parsed, tpm_event_log_rehash_ctx_t *ctx)
{
	return __tpm_event_systemd_rehash(ev, parsed, ctx);
}

static const char *
__tpm_event_tag_initrd_describe(const tpm_parsed_event_t *parsed)
{
	return "initrd (measured by the kernel)";
}

static const tpm_evdigest_t *
__tpm_event_tag_initrd_rehash(const tpm_event_t *ev, const tpm_parsed_event_t *parsed, tpm_event_log_rehash_ctx_t *ctx)
{
	const uapi_boot_entry_t *boot_entry = ctx->boot_entry;

	/* If no --next-kernel option was given, do not rehash anything */
	if (boot_entry == NULL)
		return tpm_event_get_digest(ev, ctx->algo);

	if (!boot_entry->initrd_path) {
		/* Can this happen eg when going from a split kernel to a unified kernel? */
		error("Unable to identify the next initrd\n");
		return NULL;
	}

	debug("Next boot entry expected from: %s %s\n", boot_entry->title, boot_entry->version? : "");
	debug("Measuring initrd: %s\n", boot_entry->initrd_path);
	return runtime_digest_efi_file(ctx->algo, boot_entry->initrd_path);
}

/*
 * Generated by the kernel (PCR#9), to measure the cmdline and initrd
 */
static bool
__tpm_event_parse_tag(tpm_event_t *ev, tpm_parsed_event_t *parsed, buffer_t *bp)
{
	struct tag_event *evspec = &parsed->tag_event;

	if (!buffer_get_u32le(bp, &evspec->event_id))
		return false;

	if (!buffer_get_u32le(bp, &(evspec->event_data_len)))
		return false;

	if (evspec->event_data_len > sizeof(evspec->event_data))
		return false;

	if (!buffer_get(bp, evspec->event_data, evspec->event_data_len))
		return false;

	parsed->destroy = __tpm_event_tag_destroy;
	if (evspec->event_id == LOAD_OPTIONS_EVENT_TAG_ID) {
		parsed->rehash = __tpm_event_tag_options_rehash;
		parsed->describe = __tpm_event_tag_options_describe;
	} else
	if (evspec->event_id == INITRD_EVENT_TAG_ID) {
		parsed->rehash = __tpm_event_tag_initrd_rehash;
		parsed->describe = __tpm_event_tag_initrd_describe;
	} else
		return false;

	return true;
}

static bool
__tpm_event_parse_ipl(tpm_event_t *ev, tpm_parsed_event_t *parsed, buffer_t *bp)
{
	const char *value = (const char *) ev->event_data;
	unsigned int len = ev->event_size;

	/* An empty IPL is okay - some firmwares generated these, it seems. At least
	 * my old thinkpad's firmware does this (but that machine has a TPMv1 chip). */
	if (len == 0 || *value == '\0') {
		ev->rehash_strategy = EVENT_STRATEGY_COPY;
		return true;
	}

	/* ATM, grub2 and shim seem to record the string including its trailing NUL byte */
	if (value[len - 1] != '\0')
		return false;

	if (ev->pcr_index == 8)
		return __tpm_event_grub_command_event_parse(ev, parsed, value);

	if (ev->pcr_index == 9)
		return __tpm_event_grub_file_event_parse(ev, parsed, value);

	if (ev->pcr_index == 12)
		return __tpm_event_systemd_event_parse(ev, parsed, value, len);

	if (ev->pcr_index == 14)
		return __tpm_event_shim_event_parse(ev, parsed, value);

	return false;
}

static bool
__tpm_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed, tpm_event_log_scan_ctx_t *ctx)
{
	buffer_t buf;

	buffer_init_read(&buf, ev->event_data, ev->event_size);

	switch (ev->event_type) {
	case TPM2_EVENT_EVENT_TAG:
		return __tpm_event_parse_tag(ev, parsed, &buf);

	case TPM2_EVENT_IPL:
		return __tpm_event_parse_ipl(ev, parsed, &buf);

	case TPM2_EFI_VARIABLE_AUTHORITY:
	case TPM2_EFI_VARIABLE_BOOT:
	case TPM2_EFI_VARIABLE_DRIVER_CONFIG:
		return __tpm_event_parse_efi_variable(ev, parsed, &buf);

	case TPM2_EFI_BOOT_SERVICES_APPLICATION:
	case TPM2_EFI_BOOT_SERVICES_DRIVER:
		return __tpm_event_parse_efi_bsa(ev, parsed, &buf, ctx);

	case TPM2_EFI_GPT_EVENT:
		return __tpm_event_parse_efi_gpt(ev, parsed, &buf);
	}

	return false;
}

tpm_parsed_event_t *
tpm_event_parse(tpm_event_t *ev, tpm_event_log_scan_ctx_t *ctx)
{
	if (!ev->__parsed) {
		tpm_parsed_event_t *parsed;

		parsed = tpm_parsed_event_new(ev->event_type);
		if (__tpm_event_parse(ev, parsed, ctx))
			ev->__parsed = parsed;
		else
			tpm_parsed_event_free(parsed);
	}

	return ev->__parsed;
}

void
tpm_event_log_rehash_ctx_init(tpm_event_log_rehash_ctx_t *ctx, const tpm_algo_info_t *algo)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->algo = algo;
}

void
tpm_event_log_rehash_ctx_destroy(tpm_event_log_rehash_ctx_t *ctx)
{
}

void
tpm_event_log_scan_ctx_init(tpm_event_log_scan_ctx_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}

void
tpm_event_log_scan_ctx_destroy(tpm_event_log_scan_ctx_t *ctx)
{
	drop_string(&ctx->efi_partition);
}
