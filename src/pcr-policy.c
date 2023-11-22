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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>
#include <tss2_esys.h>
#include <tss2_sys.h>
#include <tss2_tctildr.h>
#include <tss2_rc.h>
#include <tss2_mu.h>

#include "util.h"
#include "runtime.h"
#include "pcr.h"
#include "digest.h"
#include "store.h"
#include "rsa.h"
#include "bufparser.h"
#include "tpm.h"
#include "config.h"
#include "tpm2key.h"
#include "sd-boot.h"

struct target_platform {
        const char *    name;
	unsigned int	unseal_flags;

        bool            (*write_sealed_secret)(const char *pathname,
					const TPML_PCR_SELECTION *pcr_sel,
					const TPM2B_PRIVATE *sealed_private,
					const TPM2B_PUBLIC *sealed_public);
        bool            (*write_signed_policy)(const char *input_path, const char *output_path,
					const char *policy_name,
					const tpm_pcr_bank_t *bank,
					const TPM2B_DIGEST *pcr_policy,
					const tpm_rsa_key_t *signing_key,
					const TPMT_SIGNATURE *signed_policy);
	bool		(*unseal_secret)(const char *input_path, const char *output_path,
					const tpm_pcr_selection_t *pcr_selection,
					const char *signed_policy_path,
					const stored_key_t *public_key_file);
};

static TPM2B_PUBLIC SRK_template = {
	.size = sizeof(TPMT_PUBLIC),
	.publicArea = {
		.type = TPM2_ALG_RSA,
		.nameAlg = TPM2_ALG_SHA256,
		/* For reasons not clear to me, grub2 derives the SRK using the NODA attribute,
		 * which means it is not subject to dictionary attack protections. */
		.objectAttributes = TPMA_OBJECT_RESTRICTED|TPMA_OBJECT_DECRYPT \
			|TPMA_OBJECT_FIXEDTPM|TPMA_OBJECT_FIXEDPARENT \
			|TPMA_OBJECT_SENSITIVEDATAORIGIN|TPMA_OBJECT_USERWITHAUTH \
			|TPMA_OBJECT_NODA,
		.parameters = {
			.rsaDetail = {
				.symmetric = {
					.algorithm = TPM2_ALG_AES,
					.keyBits = { .sym = 128 },
					.mode = { .sym = TPM2_ALG_CFB },
				},
				.scheme = { TPM2_ALG_NULL },
				.keyBits = 2048
			}
		}
	}
};

static const TPM2B_PUBLIC seal_public_template = {
            .size = sizeof(TPMT_PUBLIC),
            .publicArea = {
                .type = TPM2_ALG_KEYEDHASH,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT,
                .parameters = {
                        .keyedHashDetail = {
                                .scheme = { TPM2_ALG_NULL },
                        }
                },
                .unique = {
                        .keyedHash = {
                                .size = 32
                        }
                }
            }
        };

void
set_srk_rsa_bits (const unsigned int rsa_bits)
{
	SRK_template.publicArea.parameters.rsaDetail.keyBits = rsa_bits;
}

static inline const tpm_evdigest_t *
tpm_evdigest_from_TPM2B_DIGEST(const TPM2B_DIGEST *td, tpm_evdigest_t *result, const tpm_algo_info_t *algo_info)
{
	memset(result, 0, sizeof(*result));
	result->algo = algo_info;
	result->size = td->size;
	memcpy(result->data, td->buffer, td->size);

	return result;
}

static bool
write_digest(const char *path, const TPM2B_DIGEST *d)
{
	buffer_t *bp;
	TPM2_RC rc;
	bool ok = false;

	bp = buffer_alloc_write(2 * sizeof(*d));

	rc = Tss2_MU_TPM2B_DIGEST_Marshal(d, bp->data, bp->size, &bp->wpos);
	if (!tss_check_error(rc, "Tss2_MU_TPM2B_DIGEST_Marshal failed"))
		goto cleanup;

	ok = buffer_write_file(path, bp);

cleanup:
	buffer_free(bp);
	return ok;
}

static TPM2B_DIGEST *
read_digest(const char *path)
{
	TPM2B_DIGEST *d;
	buffer_t *bp;
	TPM2_RC rc;

	if (!(bp = buffer_read_file(path, 0)))
		return NULL;

	d = calloc(1, sizeof(*d));

	rc = Tss2_MU_TPM2B_DIGEST_Unmarshal(bp->data, bp->size, &bp->rpos, d);
	if (!tss_check_error(rc, "Tss2_MU_TPM2B_DIGEST_Unmarshal failed")) {
		free(d);
		d = NULL;
	}

	buffer_free(bp);
	return d;
}

static TPM2B_SENSITIVE_DATA *
read_secret(const char *path)
{
	TPM2B_SENSITIVE_DATA *sd;
	buffer_t *bp;

	if (!(bp = buffer_read_file(path, 0)))
		return false;

	sd = calloc(1, sizeof(*sd));

	if (buffer_available(bp) > sizeof(sd->buffer)) {
		error("secret data too large, maximum size is %u\n", sizeof(sd->buffer));
		free(sd);
		sd = NULL;
	} else {
		sd->size = buffer_available(bp);
		memcpy(sd->buffer, buffer_read_pointer(bp), sd->size);
	}

	buffer_free_secret(bp);
	return sd;
}

static bool
write_sealed_secret(const char *path, const TPM2B_PUBLIC *pub, const TPM2B_PRIVATE *priv)
{
	buffer_t *bp;
	TPM2_RC rc;
	bool ok = false;

	bp = buffer_alloc_write(sizeof(*pub) + sizeof(*priv));

	rc = Tss2_MU_TPM2B_PUBLIC_Marshal(pub, bp->data, bp->size, &bp->wpos);
	if (rc == TSS2_RC_SUCCESS)
		rc = Tss2_MU_TPM2B_PRIVATE_Marshal(priv, bp->data, bp->size, &bp->wpos);

	if (tss_check_error(rc, "Tss2_MU_TPM2B_MAX_BUFFER_Marshal failed"))
		ok = buffer_write_file(path, bp);

	buffer_free(bp);
	return ok;
}

static bool
read_sealed_secret(const char *path, TPM2B_PUBLIC **pub_ret, TPM2B_PRIVATE **priv_ret)
{
	TPM2B_PUBLIC *pub = NULL;
	TPM2B_PRIVATE *priv = NULL;
	buffer_t *bp;
	TPM2_RC rc;
	bool ok = false;

	if (!(bp = buffer_read_file(path, 0)))
		return false;

	pub = calloc(1, sizeof(*pub));
	priv = calloc(1, sizeof(*priv));

	rc = Tss2_MU_TPM2B_PUBLIC_Unmarshal(bp->data, bp->size, &bp->rpos, pub);
	if (rc == TSS2_RC_SUCCESS)
		rc = Tss2_MU_TPM2B_PRIVATE_Unmarshal(bp->data, bp->size, &bp->rpos, priv);

	if (tss_check_error(rc, "Tss2_MU_TPM2B_MAX_BUFFER_Unmarshal failed")) {
		*priv_ret = priv;
		*pub_ret = pub;
		ok = true;
	} else {
		error("%s does not seem to contain a valid pair of public/private sealed data\n", path);
		free(priv);
		free(pub);
	}

	buffer_free(bp);
	return ok;
}

static void
free_secret(TPM2B_SENSITIVE_DATA *sd)
{
	memset(sd, 0, sizeof(*sd));
	free(sd);
}

static bool
write_signature(const char *path, const TPMT_SIGNATURE *s)
{
	buffer_t *bp;
	int rc;
	bool ok = false;

	bp = buffer_alloc_write(sizeof(*s) + 128);

	rc = Tss2_MU_TPMT_SIGNATURE_Marshal(s, bp->data, bp->size, &bp->wpos);
	if (!tss_check_error(rc, "Tss2_MU_TPMT_SIGNATURE_Marshal failed"))
		goto cleanup;

	ok = runtime_write_file(path, bp);

cleanup:
	buffer_free(bp);
	return ok;
}

static bool
read_signature(const char *path, TPMT_SIGNATURE **ret)
{
	TPMT_SIGNATURE *s;
	buffer_t *bp;
	int rc;
	bool ok = false;

	if (!(bp = buffer_read_file(path, 0)))
		return false;

	s = calloc(1, sizeof(*s));
	rc = Tss2_MU_TPMT_SIGNATURE_Unmarshal(bp->data, bp->size, &bp->rpos, s);
	if (tss_check_error(rc, "Tss2_MU_TPMT_SIGNATURE_Unmarshal failed")) {
		*ret = s;
		ok = true;
	} else {
		free(s);
	}

	buffer_free(bp);
	return ok;
}

bool
tss_write_public_key(const char *path, const TPM2B_PUBLIC *s)
{
	buffer_t *bp;
	int rc;
	bool ok = false;

	bp = buffer_alloc_write(sizeof(*s) + 128);

	rc = Tss2_MU_TPM2B_PUBLIC_Marshal(s, bp->data, bp->size, &bp->wpos);
	if (!tss_check_error(rc, "Tss2_MU_TPM2B_PUBLIC_Marshal failed"))
		goto cleanup;

	ok = runtime_write_file(path, bp);

cleanup:
	buffer_free(bp);
	return ok;
}

TPM2B_PUBLIC *
tss_read_public_key(const char *path)
{
	TPM2B_PUBLIC *pub_key, *result = NULL;
	buffer_t *bp;
	int rc;

	if (!(bp = buffer_read_file(path, 0)))
		return NULL;

	pub_key = calloc(1, sizeof(*pub_key));
	rc = Tss2_MU_TPM2B_PUBLIC_Unmarshal(bp->data, bp->size, &bp->rpos, pub_key);
	if (tss_check_error(rc, "Tss2_MU_TPM2B_PUBLIC_Unmarshal failed")) {
		result = pub_key;
	} else {
		error("%s does not seem to contain a valid public key\n", path);
		free(pub_key);
	}

	buffer_free(bp);
	return result;
}

static bool
esys_start_auth_session(ESYS_CONTEXT *esys_context, TPM2_SE session_type, ESYS_TR *session_handle_ret)
{
	static const TPMT_SYM_DEF symmetric = {
		.algorithm = TPM2_ALG_AES,
		.keyBits = { .aes = 128 },
		.mode = { .aes = TPM2_ALG_CFB }
	};
	TSS2_RC rc;

	rc = Esys_StartAuthSession(esys_context,
			ESYS_TR_NONE,	/* tpmKey */
			ESYS_TR_NONE,	/* bind */
			ESYS_TR_NONE,	/* shandle1 */
			ESYS_TR_NONE,	/* shandle2 */
			ESYS_TR_NONE,	/* shandle3 */
			NULL,		/* nonceCaller */
			session_type,	/* sessionType */
			&symmetric,
			TPM2_ALG_SHA256,
			session_handle_ret
			);

	return tss_check_error(rc, "Esys_StartAuthSession failed");
}

static void
esys_flush_context(ESYS_CONTEXT *esys_context, ESYS_TR *session_handle_p)
{
	TSS2_RC rc;

	if (*session_handle_p == ESYS_TR_NONE)
		return;

	rc = Esys_FlushContext(esys_context, *session_handle_p);
	(void) tss_check_error(rc, "Esys_FlushContext failed");
        *session_handle_p = ESYS_TR_NONE;
}

static bool
__pcr_selection_build(TPML_PCR_SELECTION *sel, unsigned int pcr_mask, const tpm_algo_info_t *algo_info)
{
	TPMS_PCR_SELECTION *bankSel;
	uint32_t i;

	memset(sel, 0, sizeof(*sel));

	/* 24 pcrs at most */
	pcr_mask &= 0xFFFFFF;
	if (pcr_mask == 0)
		return true;

	bankSel = &sel->pcrSelections[sel->count++];

	bankSel->hash = algo_info->tcg_id;
	bankSel->sizeofSelect = 3;
	for (i = 0; i < 3; ++i, pcr_mask >>= 8) {
		bankSel->pcrSelect[i] = pcr_mask & 0xFF;
	}
	return true;
}

static bool
pcr_bank_to_selection(TPML_PCR_SELECTION *sel, const tpm_pcr_bank_t *bank)
{
	return __pcr_selection_build(sel, bank->valid_mask, bank->algo_info);
}

static void
__pcr_selection_add(TPML_PCR_SELECTION *sel, unsigned int algo_id, unsigned int pcr_index)
{
	TPMS_PCR_SELECTION *bankSel;
	unsigned int i = pcr_index / 8;

	if (sel->count == 0) {
		bankSel = &sel->pcrSelections[sel->count++];
		bankSel->hash = algo_id;
		bankSel->sizeofSelect = 3;
	} else {
		bankSel = &sel->pcrSelections[0];
		assert(bankSel->hash == algo_id);
	}

	bankSel->pcrSelect[i] |= (1 << (pcr_index % 8));
}

static bool
__pcr_bank_hash(ESYS_CONTEXT *esys_context, const tpm_pcr_bank_t *bank, TPM2B_DIGEST **hash_ret, TPML_PCR_SELECTION *pcr_sel)
{
	TPM2B_AUTH null_auth = { .size = 0 };
	ESYS_TR sequence_handle = ESYS_TR_NONE;
	unsigned int i;
	TSS2_RC rc;

	memset(pcr_sel, 0, sizeof(*pcr_sel));

	debug("%s: going to hash PCRs from bank %s (TCG algo id %u)\n", __func__,
			bank->algo_info->openssl_name,
			bank->algo_info->tcg_id);

	rc = Esys_HashSequenceStart(esys_context, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			&null_auth,
			bank->algo_info->tcg_id,
			&sequence_handle);

	if (!tss_check_error(rc, "Esys_HashSequenceStart failed"))
		return false;

	for (i = 0; i < PCR_BANK_REGISTER_MAX; ++i) {
		const tpm_evdigest_t *d;
		TPM2B_MAX_BUFFER pcr_value;

		if (!pcr_bank_register_is_valid(bank, i))
			continue;
		d = &bank->pcr[i];

		assert(d->size <= sizeof(pcr_value.buffer));
		pcr_value.size = d->size;
		memcpy(pcr_value.buffer, d->data, d->size);

		rc = Esys_SequenceUpdate(esys_context, sequence_handle,
				ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
				&pcr_value);

		if (!tss_check_error(rc, "Esys_HashSequenceUpdate failed"))
			goto failed;

		__pcr_selection_add(pcr_sel, bank->algo_info->tcg_id, i);
	}

	rc = Esys_SequenceComplete(esys_context, sequence_handle,
			ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, NULL, esys_tr_rh_null,
			hash_ret, NULL);
	sequence_handle = ESYS_TR_NONE;

	if (!tss_check_error(rc, "Esys_HashSequenceUpdate failed"))
		return false;

	return true;

failed:
	if (sequence_handle != ESYS_TR_NONE)
		Esys_SequenceComplete(esys_context, sequence_handle,
			ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, NULL, esys_tr_rh_null,
			NULL, NULL);

	return false;
}

static bool
esys_policy_pcr(ESYS_CONTEXT *esys_context, TPML_PCR_SELECTION *pcrSel, TPM2B_DIGEST *pcrDigest, TPM2B_DIGEST **result)
{
	ESYS_TR session_handle = ESYS_TR_NONE;
	TPM2_RC rc;
	bool ok = false;

	if (!esys_start_auth_session(esys_context, TPM2_SE_TRIAL, &session_handle))
		return false;

	rc = Esys_PolicyPCR(esys_context, session_handle, ESYS_TR_NONE,
			ESYS_TR_NONE, ESYS_TR_NONE, pcrDigest, pcrSel);
	if (!tss_check_error(rc, "Esys_PolicyPCR failed"))
		goto cleanup;

	rc = Esys_PolicyGetDigest(esys_context, session_handle, ESYS_TR_NONE,
			ESYS_TR_NONE, ESYS_TR_NONE, result);
	if (!tss_check_error(rc, "Esys_PolicyGetDigest failed"))
		goto cleanup;

	ok = true;

cleanup:
	esys_flush_context(esys_context, &session_handle);
	return ok;
}

static TPM2B_DIGEST *
__pcr_policy_make(ESYS_CONTEXT *esys_context, const tpm_pcr_bank_t *bank)
{
	TPML_PCR_SELECTION pcrSel;
	TPM2B_DIGEST *pcrDigest = NULL;
	TPM2B_DIGEST *result = NULL;

	if (!pcr_bank_to_selection(&pcrSel, bank))
		return NULL;

	if (!__pcr_bank_hash(esys_context, bank, &pcrDigest, &pcrSel)) {
		debug("__pcr_bank_hash failed\n");
		return NULL;
	}

	if (!esys_policy_pcr(esys_context, &pcrSel, pcrDigest, &result))
		assert(result == NULL);

	if (pcrDigest)
		free(pcrDigest);
	return result;
}

static bool
esys_create_authorized_policy(ESYS_CONTEXT *esys_context,
			TPM2B_DIGEST *pcrPolicy, const TPM2B_PUBLIC *pubKey,
			TPM2B_DIGEST **authorizedPolicy)
{
	ESYS_TR session_handle;
	TPM2B_NONCE policy_qualifier = { .size = 0 };
	ESYS_TR pub_key_handle;
	TPM2B_NAME *public_key_name = NULL;
	TPM2_RC rc;
	bool okay = false;

	rc = Esys_LoadExternal(esys_context,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, NULL,
			pubKey, esys_tr_rh_owner,
			&pub_key_handle);
	if (!tss_check_error(rc, "Esys_LoadExternal failed"))
		goto out;

	rc = Esys_TR_GetName(esys_context, pub_key_handle, &public_key_name);
	if (!tss_check_error(rc, "Esys_TR_GetName failed"))
		goto out;

	/* Create a trial session */
	if (!esys_start_auth_session(esys_context, TPM2_SE_TRIAL, &session_handle))
		goto out;

	TPMT_TK_VERIFIED check_ticket = { .tag = TPM2_ST_VERIFIED, .hierarchy = TPM2_RH_OWNER, .digest = { 0 } };

	rc = Esys_PolicyAuthorize(esys_context,
			session_handle,
			ESYS_TR_NONE,
			ESYS_TR_NONE,
			ESYS_TR_NONE,
			pcrPolicy,
			&policy_qualifier,
			public_key_name,
			&check_ticket);
	if (!tss_check_error(rc, "Esys_PolicyAuthorize failed"))
		goto out;

	/* Now get the digest and return it */
	rc = Esys_PolicyGetDigest(esys_context,
			session_handle,
			ESYS_TR_NONE,
			ESYS_TR_NONE,
			ESYS_TR_NONE,
			authorizedPolicy);
	if (!tss_check_error(rc, "Esys_PolicyGetDigest failed"))
		goto out;

	okay = true;

out:
	if (public_key_name)
		free(public_key_name);
	esys_flush_context(esys_context, &session_handle);
	esys_flush_context(esys_context, &pub_key_handle);

	return okay;
}

static bool
esys_create_primary(ESYS_CONTEXT *esys_context, ESYS_TR *handle_ret)
{
	TPM2B_SENSITIVE_CREATE in_sensitive = { .size = 0 };
	TPML_PCR_SELECTION creation_pcr = { .count = 0 };
	double t0;
	TPM2_RC rc;

	t0 = timing_begin();
	rc = Esys_CreatePrimary(esys_context, ESYS_TR_RH_OWNER,
			ESYS_TR_PASSWORD,
			ESYS_TR_NONE, ESYS_TR_NONE, &in_sensitive, &SRK_template,
			NULL, &creation_pcr, handle_ret,
			NULL, NULL,
			NULL, NULL);

	if (!tss_check_error(rc, "Esys_CreatePrimary failed"))
		return false;

	debug("took %.3f sec to create SRK\n", timing_since(t0));
	return true;
}

static bool
esys_create(ESYS_CONTEXT *esys_context,
		ESYS_TR srk_handle, TPM2B_DIGEST *authorized_policy, TPM2B_SENSITIVE_DATA *secret,
		TPM2B_PRIVATE **out_private, TPM2B_PUBLIC **out_public)
{
	TPM2B_SENSITIVE_CREATE in_sensitive;
	TPM2B_PUBLIC in_public;
	TPM2_RC rc;

	memset(&in_sensitive, 0, sizeof(in_sensitive));
	in_sensitive.size = sizeof(in_sensitive);
	in_sensitive.sensitive.data = *secret;

	in_public = seal_public_template;
	in_public.publicArea.authPolicy = *authorized_policy;

	TPML_PCR_SELECTION creation_pcr = { .count = 0 };
	rc = Esys_Create(esys_context, srk_handle,
			ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
			&in_sensitive, &in_public,
			NULL, &creation_pcr, out_private, out_public, NULL, NULL, NULL);

	if (!tss_check_error(rc, "Esys_Create failed"))
		return false;

	return true;
}

static bool
esys_seal_secret(const target_platform_t *platform, ESYS_CONTEXT *esys_context,
		 TPM2B_DIGEST *policy, const TPML_PCR_SELECTION *pcr_sel,
		 const char *input_path, const char *output_path)
{
	TPM2B_SENSITIVE_DATA *secret = NULL;
	TPM2B_PRIVATE *sealed_private = NULL;
	TPM2B_PUBLIC *sealed_public = NULL;
	ESYS_TR srk_handle = ESYS_TR_NONE;
	bool ok = false;

	if (!(secret = read_secret(input_path)))
		goto cleanup;

	/* On my machine, the TPM needs 20 seconds to derive the SRK in CreatePrimary */
	infomsg("Sealing secret - this may take a moment\n");
	if (!esys_create_primary(esys_context, &srk_handle))
		goto cleanup;

	if (!esys_create(esys_context, srk_handle, policy, secret, &sealed_private, &sealed_public))
		goto cleanup;

	ok = platform->write_sealed_secret(output_path, pcr_sel, sealed_private, sealed_public);
	if (ok)
		infomsg("Sealed secret written to %s\n", output_path?: "(standard output)");

cleanup:
	if (sealed_private)
		free(sealed_private);
	if (sealed_public)
		free(sealed_public);
	if (secret)
		free_secret(secret);

	esys_flush_context(esys_context, &srk_handle);
	return ok;
}

static bool
esys_unseal_pcr_policy(ESYS_CONTEXT *esys_context,
		const tpm_pcr_bank_t *bank,
		const TPM2B_PUBLIC *sealed_public, const TPM2B_PRIVATE *sealed_private,
		TPM2B_SENSITIVE_DATA **sensitive_ret)
{
	TPML_PCR_SELECTION pcrs;
	ESYS_TR session_handle = ESYS_TR_NONE;
	ESYS_TR primary_handle = ESYS_TR_NONE;
	ESYS_TR sealed_object_handle = ESYS_TR_NONE;
	TPM2_RC rc;
	bool okay = false;

	pcr_bank_to_selection(&pcrs, bank);
	if (!esys_create_primary(esys_context, &primary_handle))
		goto cleanup;

	rc = Esys_Load(esys_context, primary_handle,
		ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
		sealed_private, sealed_public,
                &sealed_object_handle);
	if (!tss_check_error(rc, "Esys_Load failed"))
		goto cleanup;

	/* Create a policy session */
	if (!esys_start_auth_session(esys_context, TPM2_SE_POLICY, &session_handle))
		goto cleanup;

	TPM2B_DIGEST empty_digest = { .size = 0 };
	rc = Esys_PolicyPCR(esys_context, session_handle,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			&empty_digest, &pcrs);
	if (!tss_check_error(rc, "Esys_PolicyPCR failed"))
		goto cleanup;

	rc = Esys_Unseal(esys_context, sealed_object_handle,
                session_handle, ESYS_TR_NONE, ESYS_TR_NONE,
                (TPM2B_SENSITIVE_DATA **) sensitive_ret);
	if (!tss_check_error(rc, "Esys_Unseal failed"))
		goto cleanup;

	infomsg("Successfully unsealed... something.\n");
	okay = true;

cleanup:
	esys_flush_context(esys_context, &session_handle);
	esys_flush_context(esys_context, &primary_handle);
	esys_flush_context(esys_context, &sealed_object_handle);
	return okay;
}

static bool
esys_unseal_authorized(ESYS_CONTEXT *esys_context,
		const tpm_pcr_bank_t *bank,
		const TPMT_SIGNATURE *policy_signature,
		const TPM2B_PUBLIC *pub_key,
		const TPM2B_PUBLIC *sealed_public, const TPM2B_PRIVATE *sealed_private,
		TPM2B_SENSITIVE_DATA **sensitive_ret)
{
	TPML_PCR_SELECTION pcrs;
	ESYS_TR pub_key_handle = ESYS_TR_NONE;
	ESYS_TR session_handle = ESYS_TR_NONE;
	ESYS_TR primary_handle = ESYS_TR_NONE;
	ESYS_TR sealed_object_handle = ESYS_TR_NONE;
	TPM2B_NAME *public_key_name = NULL;
	TPM2B_DIGEST *pcr_policy = NULL;
	TPM2B_DIGEST *pcr_policy_hash = NULL;
	TPMT_TK_VERIFIED *verification_ticket = NULL;
	TPM2_RC rc;
	bool okay = false;

	if (policy_signature->sigAlg != TPM2_ALG_RSASSA)
		warning("%s: bad sigAlg %x\n", __func__, policy_signature->sigAlg);
	if (policy_signature->signature.rsassa.hash != TPM2_ALG_SHA256)
		warning("%s: bad hash %x\n", __func__, policy_signature->signature.rsassa.hash);

	pcr_bank_to_selection(&pcrs, bank);

	rc = Esys_LoadExternal(esys_context,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, NULL,
			pub_key, esys_tr_rh_owner,
			&pub_key_handle);
	if (!tss_check_error(rc, "Esys_LoadExternal failed"))
		goto cleanup;

	rc = Esys_TR_GetName(esys_context, pub_key_handle, &public_key_name);
	if (!tss_check_error(rc, "Esys_TR_GetName failed"))
		goto cleanup;

	if (!esys_create_primary(esys_context, &primary_handle))
		goto cleanup;

	rc = Esys_Load(esys_context, primary_handle,
		ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
		sealed_private, sealed_public,
                &sealed_object_handle);
	if (!tss_check_error(rc, "Esys_Load failed"))
		goto cleanup;


	/* Create a policy session */
	if (!esys_start_auth_session(esys_context, TPM2_SE_POLICY, &session_handle))
		goto cleanup;

	TPM2B_DIGEST empty_digest = { .size = 0 };
	rc = Esys_PolicyPCR(esys_context, session_handle,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			&empty_digest, &pcrs);
	if (!tss_check_error(rc, "Esys_PolicyPCR failed"))
		goto cleanup;

	rc = Esys_PolicyGetDigest(esys_context, session_handle, ESYS_TR_NONE,
			ESYS_TR_NONE, ESYS_TR_NONE, &pcr_policy);
	if (!tss_check_error(rc, "Esys_PolicyGetDigest failed"))
		goto cleanup;

	rc = Esys_Hash(esys_context,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			(const TPM2B_MAX_BUFFER *) pcr_policy,
			TPM2_ALG_SHA256, esys_tr_rh_null,
			&pcr_policy_hash, NULL);
	if (!tss_check_error(rc, "Esys_Hash failed"))
		goto cleanup;

	rc = Esys_VerifySignature(esys_context,
			pub_key_handle,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			pcr_policy_hash, policy_signature,
			&verification_ticket);
	if (!tss_check_error(rc, "Esys_VerifySignature failed"))
		goto cleanup;

	TPM2B_NONCE policyRef = { .size = 0 };
	rc = Esys_PolicyAuthorize(esys_context, session_handle,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			pcr_policy, &policyRef,
			public_key_name, verification_ticket);
	if (!tss_check_error(rc, "Esys_PolicyAuthorize failed"))
		goto cleanup;

	rc = Esys_Unseal(esys_context, sealed_object_handle,
                session_handle, ESYS_TR_NONE, ESYS_TR_NONE,
                (TPM2B_SENSITIVE_DATA **) sensitive_ret);
	if (!tss_check_error(rc, "Esys_Unseal failed"))
		goto cleanup;

	infomsg("Successfully unsealed... something.\n");
	okay = true;

cleanup:
	if (public_key_name)
		free(public_key_name);
	if (pcr_policy_hash)
		free(pcr_policy_hash);
	esys_flush_context(esys_context, &pub_key_handle);
	esys_flush_context(esys_context, &session_handle);
	esys_flush_context(esys_context, &primary_handle);
	esys_flush_context(esys_context, &sealed_object_handle);
	return okay;
}

static bool
__pcr_policy_sign(const tpm_rsa_key_t *rsa_key, const TPM2B_DIGEST *authorized_policy, TPMT_SIGNATURE **signed_policy)
{
	TPMT_SIGNATURE *result;
	TPM2B_PUBLIC_KEY_RSA *sigbuf;

	*signed_policy = NULL;
	result = calloc(1, sizeof(*result));

	result->sigAlg = TPM2_ALG_RSASSA;
	result->signature.rsassa.hash = TPM2_ALG_SHA256;

	sigbuf = &result->signature.rsassa.sig;

	sigbuf->size = tpm_rsa_sign(rsa_key,
			authorized_policy->buffer, authorized_policy->size,
			sigbuf->buffer, sizeof(sigbuf->buffer));
	if (sigbuf->size <= 0) {
		error("Unable to sign authorized policy\n");
		free(result);
		return false;
	}

	*signed_policy = result;

	return true;
}

static bool
__pcr_policy_create_authorized(ESYS_CONTEXT *esys_context, const tpm_pcr_selection_t *pcr_selection,
				const stored_key_t *private_key_file,
				TPM2B_DIGEST **ret_digest_p)
{
	tpm_pcr_bank_t zero_bank;
	TPM2B_DIGEST *pcr_policy = NULL;
	TPM2B_PUBLIC *pub_key = NULL;
	bool okay = false;

	if (!(pub_key = stored_key_read_native_public(private_key_file)))
		goto out;

	/* Create a PCR policy using all-zeros for the selection of PCRs we're
	 * interested in. */
	pcr_bank_initialize(&zero_bank, pcr_selection->pcr_mask, pcr_selection->algo_info);
	pcr_bank_init_from_zero(&zero_bank);
	if (!(pcr_policy = __pcr_policy_make(esys_context, &zero_bank)))
		goto out;

	okay = esys_create_authorized_policy(esys_context, pcr_policy, pub_key, ret_digest_p);

out:
	if (pcr_policy)
		free(pcr_policy);
	if (pub_key)
		free(pub_key);

	return okay;
}

/*
 * This implements the ESYS backend for pcr_bank_init_from_current
 * The previous implementation used FAPI and that's just messy.
 */
bool
pcr_read_into_bank(tpm_pcr_bank_t *bank)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TPML_PCR_SELECTION pcr_selection;
	TPML_DIGEST *pcr_values = NULL;
	unsigned int pcr_chunk_offset;
	unsigned int index, k;
	bool okay = false;
	TPM2_RC rc;

	/* TPML_DIGEST will hold only up to 8 digests, which means
	 * if we're interested in more PCRs, we need to do them in
	 * chunks of 8 or less.
	 */
	for (pcr_chunk_offset = 0; pcr_chunk_offset < PCR_BANK_REGISTER_MAX; pcr_chunk_offset += 8) {
		unsigned int pcr_mask;

		pcr_mask = bank->pcr_mask & (0xFFU << pcr_chunk_offset);
		if (pcr_mask == 0)
			continue;

		/* drop the values from previous iteration */
		if (pcr_values) {
			free(pcr_values);
			pcr_values = NULL;
		}

		/* We cannot use pcr_bank_to_selection here, because that function
		 * only selects digests for those PCRs that are valid. We haven't read
		 * any PCR values yet, so we need to consult the mask of PCRs
		 * that we *want* to have */
		if (!__pcr_selection_build(&pcr_selection, pcr_mask, bank->algo_info))
			return false;

		debug2("Trying to read PCR chunk starting with PCR %u\n", pcr_chunk_offset);
		rc = Esys_PCR_Read(esys_context,
				ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
				&pcr_selection, NULL, NULL, &pcr_values);
		if (!tss_check_error(rc, "Esys_PCR_Read failed"))
			goto cleanup;

		for (index = 0, k = 0; index < PCR_BANK_REGISTER_MAX; ++index) {
			TPM2B_DIGEST *d;

			if (!(pcr_mask & (1 << index)))
				continue;

			d = &pcr_values->digests[k++];
			if (d->size == 0)
				continue;

			if (d->size != bank->algo_info->digest_size) {
				error("Esys_PCR_Read returns a %s digest with size %u (expected %u)\n",
						bank->algo_info->openssl_name,
						d->size,
						bank->algo_info->digest_size);
				debug("PCR %u value %u size 0x%x\n", index, k, d->size);
				goto cleanup;
			}

			tpm_evdigest_t *pcr = &bank->pcr[index];

			digest_set(pcr, bank->algo_info, d->size, d->buffer);
			if (digest_is_invalid(pcr)) {
				debug2("ignoring PCR %u; %s\n", index, digest_print(pcr));
			} else {
				pcr_bank_mark_valid(bank, index);
			}
		}
	}

	okay = true;

cleanup:
	if (pcr_values)
		free(pcr_values);

	return okay;
}

/*
 * Store the public portion of an RSA key in a format compatible
 * with TSS2. This should make it easier to implement the loading
 * in a boot loader (where you do NOT want to handle PEM/DER/ASN.1)
 */
bool
pcr_store_public_key(const stored_key_t *private_key_file, const stored_key_t *public_key_file)
{
	tpm_rsa_key_t *pub_key;
	bool okay = false;

	/* Read the public key from the private key file */
	if ((pub_key = stored_key_read_rsa_public(private_key_file)) != NULL) {
		okay = stored_key_write_rsa_public(public_key_file, pub_key);
		tpm_rsa_key_free(pub_key);
	}

	return okay;
}

bool
pcr_seal_secret(const target_platform_t *platform, const tpm_pcr_bank_t *bank,
		const char *input_path, const char *output_path)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TPM2B_DIGEST *pcr_policy = NULL;
	TPML_PCR_SELECTION pcr_sel;
	bool ok = false;

	if (!(pcr_policy = __pcr_policy_make(esys_context, bank)))
		return false;

	if (!pcr_bank_to_selection(&pcr_sel, bank))
		return false;

	ok = esys_seal_secret(platform, esys_context, pcr_policy, &pcr_sel,
			      input_path, output_path);

	free(pcr_policy);
	return ok;
}

static bool
pcr_unseal_secret_pcr(const tpm_pcr_selection_t *pcr_selection, const char *input_path, const char *output_path)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	tpm_pcr_bank_t pcr_current_bank;
	TPM2B_PRIVATE *sealed_private = NULL;
	TPM2B_PUBLIC *sealed_public = NULL;
	TPM2B_SENSITIVE_DATA *unsealed = NULL;
	bool okay = false;

	if (!read_sealed_secret(input_path, &sealed_public, &sealed_private))
		goto cleanup;

	pcr_bank_initialize(&pcr_current_bank, pcr_selection->pcr_mask, pcr_selection->algo_info);
	pcr_bank_init_from_current(&pcr_current_bank);

	/* Now we've got all the ingredients we need. Go for it. */
	okay = esys_unseal_pcr_policy(esys_context,
			&pcr_current_bank,
			sealed_public, sealed_private, &unsealed);

	if (unsealed) {
		buffer_t *bp = buffer_alloc_write(unsealed->size);

		buffer_put(bp, unsealed->buffer, unsealed->size);
		buffer_write_file(output_path, bp);
		buffer_free(bp);
	}

cleanup:
	if (unsealed)
		free_secret(unsealed);
	return okay;
}


bool
pcr_authorized_policy_create(const tpm_pcr_selection_t *pcr_selection, const stored_key_t *private_key_file, const char *output_path)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TPM2B_DIGEST *authorized_policy = NULL;
	bool ok;

	ok = __pcr_policy_create_authorized(esys_context, pcr_selection, private_key_file, &authorized_policy);
	if (ok && write_digest(output_path, authorized_policy))
		infomsg("Authorized policy written to %s\n", output_path?: "(standard output)");

	if (authorized_policy)
		free(authorized_policy);

	return ok;
}

bool
pcr_authorized_policy_seal_secret(const target_platform_t *platform, const char *authpolicy_path,
				  const char *input_path, const char *output_path)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TPM2B_DIGEST *authorized_policy = NULL;
	bool ok = false;

	if (!(authorized_policy = read_digest(authpolicy_path)))
		return false;

	ok = esys_seal_secret(platform, esys_context, authorized_policy, NULL,
			      input_path, output_path);
	free(authorized_policy);
	return ok;
}

bool
old_pcr_authorized_policy_seal_secret(const char *authpolicy_path, const char *input_path, const char *output_path)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TPM2B_DIGEST *authorized_policy = NULL;
	TPM2B_SENSITIVE_DATA *secret = NULL;
	TPM2B_PRIVATE *sealed_private = NULL;
	TPM2B_PUBLIC *sealed_public = NULL;
	ESYS_TR srk_handle = ESYS_TR_NONE;
	bool ok = false;

	if (!(secret = read_secret(input_path)))
		goto cleanup;

	if (!(authorized_policy = read_digest(authpolicy_path)))
		goto cleanup;

	/* On my machine, the TPM needs 20 seconds to derive the SRK in CreatePrimary */
	infomsg("Sealing secret - this may take a moment\n");

	if (!esys_create_primary(esys_context, &srk_handle))
		goto cleanup;

	if (!esys_create(esys_context, srk_handle, authorized_policy, secret, &sealed_private, &sealed_public))
		goto cleanup;

	ok = write_sealed_secret(output_path, sealed_public, sealed_private);

	if (ok)
		infomsg("Sealed secret written to %s\n", output_path?: "(standard output)");

cleanup:
	if (sealed_private)
		free(sealed_private);
	if (sealed_public)
		free(sealed_public);
	if (authorized_policy)
		free(authorized_policy);
	if (secret)
		free_secret(secret);

	esys_flush_context(esys_context, &srk_handle);
	return ok;
}


/*
 * The "signing" part of using authorized policies consists of hashing together the set of
 * expected PCR values, and signing the resulting digest.
 */
bool
pcr_policy_sign(const target_platform_t *platform, const tpm_pcr_bank_t *bank,
		const stored_key_t *private_key_file,
		const char *input_path, const char *output_path, const char *policy_name)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TPM2B_DIGEST *pcr_policy = NULL;
	tpm_rsa_key_t *rsa_key = NULL;
	TPM2B_PUBLIC *pub_key = NULL;
	TPMT_SIGNATURE *signed_policy = NULL;
	bool okay = false;

	if (platform->write_signed_policy == NULL) {
		error("Platform %s does not support signing policies yet\n", platform->name);
		goto out;
	}

	if (!(rsa_key = stored_key_read_rsa_private(private_key_file)))
		goto out;

	if (!(pcr_policy = __pcr_policy_make(esys_context, bank)))
		goto out;

	if (!__pcr_policy_sign(rsa_key, pcr_policy, &signed_policy))
		goto out;

	okay = platform->write_signed_policy(input_path, output_path,
			policy_name, bank, pcr_policy,
			rsa_key, signed_policy);
	if (okay)
		infomsg("Signed PCR policy written to %s\n", output_path?: "(standard output)");

out:
	if (pcr_policy)
		free(pcr_policy);
	if (signed_policy)
		free(signed_policy);
	if (pub_key)
		free(pub_key);
	if (rsa_key)
		tpm_rsa_key_free(rsa_key);

	return okay;
}

/*
 * This is not really needed here - the code that does the actual unsealing
 * should probably live in the boot loader.
 * The code is here mostly for educational/testing purposes.
 */
static bool
pcr_authorized_policy_unseal_secret(const tpm_pcr_selection_t *pcr_selection,
				const char *signed_policy_path,
				const stored_key_t *public_key_file,
                                const char *input_path, const char *output_path)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	tpm_pcr_bank_t pcr_current_bank;
	TPMT_SIGNATURE *policy_signature = NULL;
	tpm_rsa_key_t *rsa_key = NULL;
	TPM2B_PUBLIC *pub_key = NULL;
	TPM2B_PRIVATE *sealed_private = NULL;
	TPM2B_PUBLIC *sealed_public = NULL;
	TPM2B_SENSITIVE_DATA *unsealed = NULL;
	bool okay = false;

	if (!(pub_key = stored_key_read_native_public(public_key_file)))
		goto cleanup;

	if (!read_sealed_secret(input_path, &sealed_public, &sealed_private))
		goto cleanup;

	if (!read_signature(signed_policy_path, &policy_signature))
		goto cleanup;

	/* On my machine, the TPM needs 20 seconds to derive the SRK in CreatePrimary */
	infomsg("Unsealing secret - this may take a moment\n");

	pcr_bank_initialize(&pcr_current_bank, pcr_selection->pcr_mask, pcr_selection->algo_info);
	pcr_bank_init_from_current(&pcr_current_bank);

	/* Now we've got all the ingredients we need. Go for it. */
	okay = esys_unseal_authorized(esys_context,
			&pcr_current_bank, policy_signature, pub_key,
			sealed_public, sealed_private, &unsealed);

	if (unsealed) {
		buffer_t *bp = buffer_alloc_write(unsealed->size);

		buffer_put(bp, unsealed->buffer, unsealed->size);
		buffer_write_file(output_path, bp);
		buffer_free(bp);
	}

cleanup:
	if (unsealed)
		free_secret(unsealed);
	if (policy_signature)
		free(policy_signature);
	if (pub_key)
		free(pub_key);
	if (rsa_key)
		tpm_rsa_key_free(rsa_key);

	return okay;
}

static inline TPMI_ALG_HASH
__TPMT_SIGNATURE_get_hash_alg (TPMT_SIGNATURE *sig)
{
  switch (sig->sigAlg)
    {
    case TPM2_ALG_RSASSA:
      return sig->signature.rsassa.hash;
    case TPM2_ALG_RSAPSS:
      return sig->signature.rsapss.hash;
    case TPM2_ALG_ECDSA:
      return sig->signature.ecdsa.hash;
    case TPM2_ALG_ECDAA:
      return sig->signature.ecdaa.hash;
    case TPM2_ALG_SM2:
      return sig->signature.sm2.hash;
    case TPM2_ALG_ECSCHNORR:
      return sig->signature.ecschnorr.hash;
    case TPM2_ALG_HMAC:
      return sig->signature.hmac.hashAlg;
    default:
      break;
    }

  return TPM2_ALG_NULL;
}

static bool
__pcr_policy_tpm2_policyauthorize(ESYS_CONTEXT *esys_context, ESYS_TR session_handle, buffer_t *bp)
{
	TPM2B_PUBLIC pub_key = { 0 };
	TPM2B_DIGEST policy_ref = { 0 };
	TPMT_SIGNATURE policy_signature = { 0 };
	TPMI_ALG_HASH sig_hash_alg;
	TPM2B_DIGEST *pcr_policy = NULL;
	TPM2B_DIGEST *pcr_policy_hash = NULL;
	TPMT_TK_VERIFIED *verification_ticket = NULL;
	ESYS_TR pub_key_handle = ESYS_TR_NONE;
	TPM2B_NAME *public_key_name = NULL;
	TPM2_RC rc;
	bool okay = false;

	rc = Tss2_MU_TPM2B_PUBLIC_Unmarshal(bp->data, bp->size, &bp->rpos, &pub_key);
	if (rc != TSS2_RC_SUCCESS)
		return false;

	rc = Tss2_MU_TPM2B_DIGEST_Unmarshal(bp->data, bp->size, &bp->rpos, &policy_ref);
	if (rc != TSS2_RC_SUCCESS)
		return false;

	rc = Tss2_MU_TPMT_SIGNATURE_Unmarshal(bp->data, bp->size, &bp->rpos, &policy_signature);
	if (rc != TSS2_RC_SUCCESS)
		return false;

	sig_hash_alg = __TPMT_SIGNATURE_get_hash_alg(&policy_signature);

	rc = Esys_PolicyGetDigest(esys_context, session_handle, ESYS_TR_NONE,
			ESYS_TR_NONE, ESYS_TR_NONE, &pcr_policy);
	if (!tss_check_error(rc, "Esys_PolicyGetDigest failed"))
		goto cleanup;

	rc = Esys_Hash(esys_context,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			(const TPM2B_MAX_BUFFER *) pcr_policy,
			sig_hash_alg, esys_tr_rh_null,
			&pcr_policy_hash, NULL);
	if (!tss_check_error(rc, "Esys_Hash failed"))
		goto cleanup;

	rc = Esys_LoadExternal(esys_context,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, NULL,
			&pub_key, esys_tr_rh_owner,
			&pub_key_handle);
	if (!tss_check_error(rc, "Esys_LoadExternal failed"))
		goto cleanup;

	rc = Esys_TR_GetName(esys_context, pub_key_handle, &public_key_name);
	if (!tss_check_error(rc, "Esys_TR_GetName failed"))
		goto cleanup;

	rc = Esys_VerifySignature(esys_context,
			pub_key_handle,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			pcr_policy_hash, &policy_signature,
			&verification_ticket);
	if (!tss_check_error(rc, "Esys_VerifySignature failed"))
		goto cleanup;

	rc = Esys_PolicyAuthorize(esys_context, session_handle,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			pcr_policy, &policy_ref,
			public_key_name, verification_ticket);
	if (!tss_check_error(rc, "Esys_PolicyAuthorize failed"))
		goto cleanup;

	okay = true;
cleanup:
	if (pcr_policy)
		free(pcr_policy);
	if (pcr_policy_hash)
		free(pcr_policy_hash);
	if (public_key_name)
		free(public_key_name);
	esys_flush_context(esys_context, &pub_key_handle);

	return okay;
}

static bool
__pcr_policy_tpm2_policypcr(ESYS_CONTEXT *esys_context, ESYS_TR session_handle, buffer_t *bp)
{
	TPM2B_DIGEST digest = { 0 };
	TPML_PCR_SELECTION pcrs = { 0 };
	TPM2_RC rc;

	rc = Tss2_MU_TPM2B_DIGEST_Unmarshal(bp->data, bp->size, &bp->rpos, &digest);
	if (rc != TSS2_RC_SUCCESS)
		return false;

	rc = Tss2_MU_TPML_PCR_SELECTION_Unmarshal(bp->data, bp->size, &bp->rpos, &pcrs);
	if (rc != TSS2_RC_SUCCESS)
		return false;

	rc = Esys_PolicyPCR(esys_context, session_handle,
			ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
			&digest, &pcrs);
	if (!tss_check_error(rc, "Esys_PolicyPCR failed"))
		return false;

	return true;
}

static bool
__pcr_policy_unseal_policy_seq(ESYS_CONTEXT *esys_context,
			ESYS_TR sealed_object_handle,
			STACK_OF(TSSOPTPOLICY) *policy_seq,
			TPM2B_SENSITIVE_DATA **sensitive_ret)
{
	ESYS_TR session_handle = ESYS_TR_NONE;
	TSSOPTPOLICY *policy;
	int i, num_commands;
	TPM2_RC rc;
	bool okay = false;

	/* Create a policy session */
	if (!esys_start_auth_session(esys_context, TPM2_SE_POLICY, &session_handle))
		goto cleanup;

	num_commands = sk_TSSOPTPOLICY_num(policy_seq);
	for (i = 0; i < num_commands; i++) {
		int code;
		buffer_t buf;

		policy = sk_TSSOPTPOLICY_value(policy_seq, i);
		code = ASN1_INTEGER_get(policy->CommandCode);
		buffer_init_read(&buf, policy->CommandPolicy->data, policy->CommandPolicy->length);
		switch (code) {
		case TPM2_CC_PolicyPCR:
			if (!__pcr_policy_tpm2_policypcr(esys_context, session_handle, &buf))
				goto cleanup;
			break;
		case TPM2_CC_PolicyAuthorize:
			if (!__pcr_policy_tpm2_policyauthorize(esys_context, session_handle, &buf))
				goto cleanup;
			break;
		default:
			error("Unsupported TPM command: %d\n", code);
			goto cleanup;
		}
	}

	rc = Esys_Unseal(esys_context, sealed_object_handle,
                session_handle, ESYS_TR_NONE, ESYS_TR_NONE,
                sensitive_ret);
	if (!tss_check_error(rc, "Esys_Unseal failed"))
		goto cleanup;

	infomsg("Successfully unsealed... something.\n");

	okay = true;
cleanup:
	esys_flush_context(esys_context, &session_handle);

	return okay;
}

/* Unseal the key in TPM 2.0 Key File format */
static bool
tpm2key_unseal_secret(const char *input_path, const char *output_path,
				const tpm_pcr_selection_t *pcr_selection,
				const char *signed_policy_path,
				const stored_key_t *public_key_file)
{
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TSSPRIVKEY *tpm2key = NULL;
	buffer_t buf;
	TPM2B_PUBLIC pub = { 0 };
	TPM2B_PRIVATE priv = { 0 };
	ESYS_TR primary_handle = ESYS_TR_NONE;
	ESYS_TR sealed_object_handle = ESYS_TR_NONE;
	TPM2B_SENSITIVE_DATA *unsealed = NULL;
	TPM2_RC rc;
	bool okay = false;

	if (!tpm2key_read_file(input_path, &tpm2key))
		return false;

	buffer_init_read(&buf, tpm2key->pubkey->data, tpm2key->pubkey->length);
	rc = Tss2_MU_TPM2B_PUBLIC_Unmarshal(buf.data, buf.size, &buf.rpos, &pub);
	if (rc != TSS2_RC_SUCCESS)
		goto cleanup;

	buffer_init_read(&buf, tpm2key->privkey->data, tpm2key->privkey->length);
	rc = Tss2_MU_TPM2B_PRIVATE_Unmarshal(buf.data, buf.size, &buf.rpos, &priv);
	if (rc != TSS2_RC_SUCCESS)
		goto cleanup;

	if (!esys_create_primary(esys_context, &primary_handle))
		goto cleanup;

	rc = Esys_Load(esys_context, primary_handle,
		ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
		&priv, &pub, &sealed_object_handle);
	if (!tss_check_error(rc, "Esys_Load failed"))
		goto cleanup;

	if (tpm2key->authPolicy) {
		TSSAUTHPOLICY *authpolicy;
		STACK_OF(TSSOPTPOLICY) *policy_seq;
		int i, num_policies;

		num_policies = sk_TSSAUTHPOLICY_num(tpm2key->authPolicy);
		for (i = 0; i < num_policies; i++) {
			authpolicy = sk_TSSAUTHPOLICY_value(tpm2key->authPolicy, i);
			policy_seq = authpolicy->policy;
			okay = __pcr_policy_unseal_policy_seq(esys_context,
					sealed_object_handle, policy_seq, &unsealed);
			if (okay)
				break;
		}
	} else if (tpm2key->policy) {
		okay = __pcr_policy_unseal_policy_seq(esys_context, sealed_object_handle,
				 tpm2key->policy, &unsealed);
	}

	if (unsealed) {
		buffer_t *bp = buffer_alloc_write(unsealed->size);

		buffer_put(bp, unsealed->buffer, unsealed->size);
		buffer_write_file(output_path, bp);
		buffer_free(bp);
	}

cleanup:
	if (tpm2key)
		TSSPRIVKEY_free(tpm2key);
	if (unsealed)
		free_secret(unsealed);

	esys_flush_context(esys_context, &primary_handle);
	esys_flush_context(esys_context, &sealed_object_handle);

	return okay;
}

bool
pcr_unseal_secret_new(const target_platform_t *platform,
				const tpm_pcr_selection_t *pcr_selection,
				const char *signed_policy_path,
				const stored_key_t *public_key_file,
				const char *input_path, const char *output_path)
{
	if (!platform->unseal_secret) {
		error("target platform %s does not support unsealing yet\n", platform->name);
		return false;
	}

	return platform->unseal_secret(input_path, output_path, pcr_selection, signed_policy_path, public_key_file);
}

bool
pcr_policy_sign_systemd(const tpm_pcr_bank_t *bank,
			const stored_key_t *private_key_file,
			const char *output_path)
{
	bool ok = false;
	FILE *fp = NULL;
	tpm_rsa_key_t *rsa_key = NULL;
        const tpm_evdigest_t *digest;
	ESYS_CONTEXT *esys_context = tss_esys_context();
	TPM2B_DIGEST *pcr_policy = NULL;
	TPMT_SIGNATURE *signed_policy = NULL;

	if (!(fp = fopen(output_path, "w"))) {
		error("Cannot open systemd JSON file %s: %m\n", output_path);
		goto out;
	}

	if (!(rsa_key = stored_key_read_rsa_private(private_key_file)))
		goto out;
	digest = tpm_rsa_key_public_digest(rsa_key);

	if (!(pcr_policy = __pcr_policy_make(esys_context, bank)))
		goto out;

	if (!__pcr_policy_sign(rsa_key, pcr_policy, &signed_policy))
		goto out;

	fprintf(fp, "{\n");
	fprintf(fp, "\t\"%s\": [\n", bank->algo_name);
	fprintf(fp, "\t\t\{\n");
	fprintf(fp, "\t\t\t\"pcrs\": [\n");
	fprintf(fp, "\t\t\t\t%s\n", print_pcr_mask(bank->pcr_mask));
	fprintf(fp, "\t\t\t],\n");
	fprintf(fp, "\t\t\t\"pkfp\": \"%s\",\n", print_hex_string(digest->data, digest->size));
	fprintf(fp, "\t\t\t\"pol\": \"%s\",\n", print_hex_string(pcr_policy->buffer, pcr_policy->size));
	fprintf(fp, "\t\t\t\"sig\": \"%s\"\n", print_base64_value(signed_policy->signature.rsassa.sig.buffer, signed_policy->signature.rsassa.sig.size));
	fprintf(fp, "\t\t}\n");
	fprintf(fp, "\t]\n");
	fprintf(fp, "}\n");

	ok = true;

out:
	if (rsa_key)
		tpm_rsa_key_free(rsa_key);

	fclose(fp);
	return ok;
}

/*
 * Depending on the target platform, sealed data, authorized policies etc are
 * written to different types of files.
 */
static bool
oldgrub_write_sealed_secret(const char *pathname,
					const TPML_PCR_SELECTION *pcr_sel,
					const TPM2B_PRIVATE *sealed_private,
					const TPM2B_PUBLIC *sealed_public)
{
	/* Just marshal public and private portions and concat them into a single file. */
	return write_sealed_secret(pathname, sealed_public, sealed_private);
}

static bool
oldgrub_write_signed_policy(const char *input_path, const char *output_path,
					const char *policy_name,
					const tpm_pcr_bank_t *bank,
					const TPM2B_DIGEST *pcr_policy,
					const tpm_rsa_key_t *signing_key,
					const TPMT_SIGNATURE *signed_policy)
{
	/* Just write the signature, that's all */
	return write_signature(output_path, signed_policy);
}

static bool
oldgrub_unseal_secret(const char *input_path, const char *output_path,
				const tpm_pcr_selection_t *pcr_selection,
				const char *signed_policy_path,
				const stored_key_t *public_key_file)
{
	if (signed_policy_path == NULL)
		return pcr_unseal_secret_pcr(pcr_selection, input_path, output_path);

	return pcr_authorized_policy_unseal_secret(pcr_selection,
				signed_policy_path, public_key_file,
				input_path, output_path);
}

/*
 * This uses the TPM2.0 Key format defined in
 * https://www.hansenpartnership.com/draft-bottomley-tpm2-keys.html
 */
static bool
tpm2key_write_sealed_secret(const char *pathname,
					const TPML_PCR_SELECTION *pcr_sel,
					const TPM2B_PRIVATE *sealed_private,
					const TPM2B_PUBLIC *sealed_public)
{
	TSSPRIVKEY *tpm2key = NULL;
	bool ok = false;

	if (!tpm2key_basekey(&tpm2key, TPM2_RH_OWNER, sealed_public, sealed_private))
		goto cleanup;

	if (pcr_sel && !tpm2key_add_policy_policypcr(tpm2key, pcr_sel))
		goto cleanup;

	ok = tpm2key_write_file(pathname, tpm2key);

cleanup:
	if (tpm2key)
		TSSPRIVKEY_free(tpm2key);
	return ok;
}

static bool
tpm2key_write_signed_policy(const char *input_path, const char *output_path,
					const char *policy_name,
					const tpm_pcr_bank_t *bank,
					const TPM2B_DIGEST *pcr_policy,
					const tpm_rsa_key_t *signing_key,
					const TPMT_SIGNATURE *signed_policy)
{
	TSSPRIVKEY *tpm2key = NULL;
	TPM2B_PUBLIC *pub_key = NULL;
	TPML_PCR_SELECTION pcr_sel;
	bool okay = false;

	if (!policy_name)
		policy_name = "default";

	/* Allow an in-place update */
	if (input_path == NULL)
		input_path = output_path;

	if (!tpm2key_read_file(input_path, &tpm2key))
		goto out;

	if (!(pub_key = tpm_rsa_key_to_tss2(signing_key)))
		goto out;

	if (!pcr_bank_to_selection(&pcr_sel, bank))
		goto out;

	/* Prepend the signed policy */
	if (!tpm2key_add_authpolicy_policyauthorize(tpm2key, policy_name, &pcr_sel, pub_key, signed_policy, false))
		goto out;

	okay = tpm2key_write_file(output_path, tpm2key);

out:
	if (pub_key)
		free(pub_key);
	if (tpm2key)
		TSSPRIVKEY_free(tpm2key);

	return okay;
}

static bool
systemd_write_signed_policy(const char *input_path, const char *output_path,
					const char *policy_name,
					const tpm_pcr_bank_t *bank,
					const TPM2B_DIGEST *pcr_policy,
					const tpm_rsa_key_t *signing_key,
					const TPMT_SIGNATURE *signed_policy)
{
	if (input_path && strcmp(input_path, output_path)) {
		error("systemd policy will only do in-place updates of the json file\n");
		return false;
	}

#ifdef notyet
	sdb_policy_file_add_entry(output_path,
			policy_name,
			bank->algo_name,
			bank->pcr_mask, ...);
#endif
	error("%s not yet implemented\n", __func__);
	return false;
}

static target_platform_t	target_platforms[] = {
	{
		.name			= "oldgrub",
		.unseal_flags		= PLATFORM_NEED_INPUT_FILE
					| PLATFORM_NEED_OUTPUT_FILE
					| PLATFORM_NEED_PCR_SELECTION,
		.write_sealed_secret	= oldgrub_write_sealed_secret,
		.write_signed_policy	= oldgrub_write_signed_policy,
		.unseal_secret		= oldgrub_unseal_secret,
	},
	{
		.name			= "tpm2.0",
		.unseal_flags		= PLATFORM_NEED_INPUT_FILE | PLATFORM_NEED_OUTPUT_FILE,
		.write_sealed_secret	= tpm2key_write_sealed_secret,
		.write_signed_policy	= tpm2key_write_signed_policy,
		.unseal_secret		= tpm2key_unseal_secret,
	},
	{
		.name			= "systemd",
		.unseal_flags		= PLATFORM_NEED_INPUT_FILE | PLATFORM_NEED_OUTPUT_FILE,
		.write_sealed_secret	= tpm2key_write_sealed_secret,
		.write_signed_policy	= systemd_write_signed_policy,
	},
	{ NULL }
};

const target_platform_t *
pcr_get_target_platform(const char *name)
{
	target_platform_t *tp;

	for (tp = target_platforms; tp->name; ++tp) {
		if (!strcmp(tp->name, name))
			return tp;
	}
	return NULL;
}

unsigned int
target_platform_unseal_flags(const target_platform_t *platform)
{
	return platform->unseal_flags;
}
