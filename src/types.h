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

#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct buffer		buffer_t;
typedef struct tpm_evdigest	tpm_evdigest_t;
typedef struct tpm_algo_info	tpm_algo_info_t;
typedef struct digest_ctx	digest_ctx_t;
typedef struct win_cert		win_cert_t;
typedef struct cert_table	cert_table_t;
typedef struct parsed_cert	parsed_cert_t;
typedef struct pecoff_image_info pecoff_image_info_t;
typedef struct testcase		testcase_t;
typedef struct stored_key	stored_key_t;
typedef struct target_platform	target_platform_t;

#endif /* TYPES_H */


