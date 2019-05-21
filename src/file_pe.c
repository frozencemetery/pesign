// SPDX-License-Identifier: GPLv2
/*
 * file_pe.c - decls for our PE file type helpers.
 * Copyright Peter Jones <pjones@redhat.com>
 * Copyright Red Hat, Inc.
 */
#include "fix_coverity.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pesign.h"
#include "pesign_standalone.h"

static void
open_input(pesign_context *ctx)
{
	if (!ctx->infile) {
		fprintf(stderr, "pesign: No input file specified.\n");
		exit(1);
	}

	struct stat statbuf;
	ctx->infd = open(ctx->infile, O_RDONLY|O_CLOEXEC);
	stat(ctx->infile, &statbuf);
	ctx->outmode = statbuf.st_mode;

	if (ctx->infd < 0) {
		fprintf(stderr, "pesign: Error opening input: %m\n");
		exit(1);
	}

	Pe_Cmd cmd = ctx->infd == STDIN_FILENO ? PE_C_READ : PE_C_READ_MMAP;
	ctx->inpe = pe_begin(ctx->infd, cmd, NULL);
	if (!ctx->inpe) {
		fprintf(stderr, "pesign: could not load input file: %s\n",
			pe_errmsg(pe_errno()));
		exit(1);
	}

	int rc = parse_signatures(&ctx->cms_ctx->signatures,
				  &ctx->cms_ctx->num_signatures, ctx->inpe);
	if (rc < 0) {
		fprintf(stderr, "pesign: could not parse signature list in "
			"EFI binary\n");
		exit(1);
	}
}

static void
close_input(pesign_context *ctx)
{
	pe_end(ctx->inpe);
	ctx->inpe = NULL;

	close(ctx->infd);
	ctx->infd = -1;
}

static void
close_output(pesign_context *ctx)
{
	Pe_Cmd cmd = ctx->outfd == STDOUT_FILENO ? PE_C_RDWR : PE_C_RDWR_MMAP;

	finalize_signatures(ctx->cms_ctx->signatures,
				ctx->cms_ctx->num_signatures, ctx->outpe);
	pe_update(ctx->outpe, cmd);
	pe_end(ctx->outpe);
	ctx->outpe = NULL;

	close(ctx->outfd);
	ctx->outfd = -1;
}

static void
open_output(pesign_context *ctx)
{
	if (!ctx->outfile) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (access(ctx->outfile, F_OK) == 0 && ctx->force == 0) {
		fprintf(stderr, "pesign: \"%s\" exists and --force was "
				"not given.\n", ctx->outfile);
		exit(1);
	}

	ctx->outfd = open(ctx->outfile, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,
			ctx->outmode);
	if (ctx->outfd < 0) {
		fprintf(stderr, "pesign: Error opening output: %m\n");
		exit(1);
	}

	size_t size;
	char *addr;

	addr = pe_rawfile(ctx->inpe, &size);

	ftruncate(ctx->outfd, size);
	lseek(ctx->outfd, 0, SEEK_SET);
	write(ctx->outfd, addr, size);

	Pe_Cmd cmd = ctx->outfd == STDOUT_FILENO ? PE_C_RDWR : PE_C_RDWR_MMAP;
	ctx->outpe = pe_begin(ctx->outfd, cmd, NULL);
	conderrx(!ctx->outpe, 1, "could not load output file \"%s\": %s",
                 ctx->outfile, pe_errmsg(pe_errno()));

	pe_clearcert(ctx->outpe);
}

#define define_input_file(fname, name, descr)                           \
        static void                                                     \
        CAT3(open_, fname, _input)(pesign_context *ctx)                 \
        {                                                               \
                conderrx(!ctx->name, 1,                                 \
                         "No input file specified for %s",              \
                         descr);                                        \
                ctx->CAT(name, fd) =                                    \
                        open(ctx->name, O_RDONLY|O_CLOEXEC);            \
                conderr(ctx->CAT(name, fd) < 0, 1,                      \
                        "Error opening %s file \"%s\" for input",       \
                        descr, ctx->name);                              \
        }                                                               \
        static void                                                     \
        CAT3(close_, fname, _input)(pesign_context *ctx)                \
        {                                                               \
                close(ctx->CAT(name, fd));                              \
                ctx->CAT(name, fd) = -1;                                \
        }

#define define_output_file(fname, name, descr)                          \
        static void                                                     \
        CAT3(open_, fname, _output)(pesign_context *ctx)                \
        {                                                               \
                conderrx(!ctx->name, 1,                                 \
                         "No output file specified for %s.",            \
                         descr);                                        \
                                                                        \
                if (access(ctx->name, F_OK) == 0 && ctx->force == 0)    \
                        errx(1,                                         \
                             "\"%s\" exists and --force was not given.",\
                             ctx->name);                                \
                                                                        \
                ctx->CAT(name, fd) =                                    \
                        open(ctx->name,                                 \
                             O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,          \
                             ctx->outmode);                             \
                conderr(ctx->CAT(name, fd) < 0, 1,                      \
                        "Error opening %s file \"%s\" for output",      \
                        descr, ctx->name);                              \
        }                                                               \
        static void                                                     \
        CAT3(close_, fname, _output)(pesign_context *ctx)               \
        {                                                               \
                close(ctx->CAT(name,fd));                               \
                ctx->CAT(name,fd) = -1;                                 \
        }

define_input_file(rawsig, rawsig, "raw signature");
define_input_file(sattr, insattrs, "signed attributes");
define_output_file(sattr, outsattrs, "signed attributes");
define_input_file(sig, insig, "signature");
define_output_file(sig, outsig, "signature");
define_output_file(pubkey, outkey, "pubkey");
define_output_file(cert, outcert, "certificate");

static void
check_inputs(pesign_context *ctx)
{
	if (!ctx->infile) {
		fprintf(stderr, "pesign: No input file specified.\n");
		exit(1);
	}

	if (!ctx->outfile) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (!strcmp(ctx->infile, ctx->outfile)) {
		fprintf(stderr, "pesign: in-place file editing "
				"is not yet supported\n");
		exit(1);
	}
}

static void
print_digest(pesign_context *pctx)
{
	if (!pctx)
		return;

	cms_context *ctx = pctx->cms_ctx;
	if (!ctx)
		return;

	printf("%s ", pctx->infile);
	int j = ctx->selected_digest;
	for (unsigned int i = 0; i < ctx->digests[j].pe_digest->len; i++)
		printf("%02x",
			(unsigned char)ctx->digests[j].pe_digest->data[i]);
	printf("\n");
}

void
pe_handle_action(pesign_context *ctxp, int action, int padding)
{
	ssize_t sigspace = 0;
	int rc;

	switch (action) {
		/* in this case we have the actual binary signature and the
		 * signing cert, but not the pkcs7ish certificate that goes
		 * with it.
		 */
		case IMPORT_RAW_SIGNATURE|IMPORT_SATTRS:
			check_inputs(ctxp);
			rc = find_certificate(ctxp->cms_ctx, 0);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_rawsig_input(ctxp);
			open_sattr_input(ctxp);
			import_raw_signature(ctxp);
			close_sattr_input(ctxp);
			close_rawsig_input(ctxp);

			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->outpe, 1);
			sigspace = calculate_signature_space(ctxp->cms_ctx,
								ctxp->outpe);
			allocate_signature_space(ctxp->outpe, sigspace);
			generate_signature(ctxp->cms_ctx);
			insert_signature(ctxp->cms_ctx, ctxp->signum);
			close_output(ctxp);
			break;
		case EXPORT_SATTRS:
			open_input(ctxp);
			open_sattr_output(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->inpe, 1);
			generate_sattr_blob(ctxp);
			close_sattr_output(ctxp);
			close_input(ctxp);
			break;
		/* add a signature from a file */
		case IMPORT_SIGNATURE:
			check_inputs(ctxp);
			if (ctxp->signum > ctxp->cms_ctx->num_signatures + 1) {
				fprintf(stderr, "Invalid signature number.\n");
				exit(1);
			}
			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			open_sig_input(ctxp);
			parse_signature(ctxp);
			sigspace = get_sigspace_extend_amount(ctxp->cms_ctx,
					ctxp->outpe, &ctxp->cms_ctx->newsig);
			allocate_signature_space(ctxp->outpe, sigspace);
			check_signature_space(ctxp);
			insert_signature(ctxp->cms_ctx, ctxp->signum);
			close_sig_input(ctxp);
			close_output(ctxp);
			break;
		case EXPORT_PUBKEY:
			rc = find_certificate(ctxp->cms_ctx, 1);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_pubkey_output(ctxp);
			export_pubkey(ctxp);
			break;
		case EXPORT_CERT:
			rc = find_certificate(ctxp->cms_ctx, 0);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_cert_output(ctxp);
			export_cert(ctxp);
			break;
		/* find a signature in the binary and save it to a file */
		case EXPORT_SIGNATURE:
			open_input(ctxp);
			open_sig_output(ctxp);
			if (ctxp->signum > ctxp->cms_ctx->num_signatures) {
				fprintf(stderr, "Invalid signature number.\n");
				exit(1);
			}
			if (ctxp->signum < 0)
				ctxp->signum = 0;
			if (ctxp->signum >= ctxp->cms_ctx->num_signatures) {
				fprintf(stderr, "No valid signature #%d.\n",
					ctxp->signum);
				exit(1);
			}
			memcpy(&ctxp->cms_ctx->newsig,
				ctxp->cms_ctx->signatures[ctxp->signum],
				sizeof (ctxp->cms_ctx->newsig));
			export_signature(ctxp->cms_ctx, ctxp->outsigfd, ctxp->ascii);
			close_input(ctxp);
			close_sig_output(ctxp);
			memset(&ctxp->cms_ctx->newsig, '\0',
				sizeof (ctxp->cms_ctx->newsig));
			break;
		/* remove a signature from the binary */
		case REMOVE_SIGNATURE:
			check_inputs(ctxp);
			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			if (ctxp->signum < 0 ||
					ctxp->signum >=
					ctxp->cms_ctx->num_signatures) {
				fprintf(stderr, "Invalid signature number %d.  "
					"Must be between 0 and %d.\n",
					ctxp->signum,
					ctxp->cms_ctx->num_signatures - 1);
				exit(1);
			}
			remove_signature(ctxp);
			close_output(ctxp);
			break;
		/* list signatures in the binary */
		case LIST_SIGNATURES:
			open_input(ctxp);
			list_signatures(ctxp);
			break;
		case GENERATE_DIGEST|PRINT_DIGEST|OMIT_VENDOR_CERT:
			open_input(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->inpe, padding);
			print_digest(ctxp);
			break;
		case GENERATE_DIGEST|PRINT_DIGEST:
			open_input(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->inpe, padding);
			print_digest(ctxp);
			break;
		/* generate a signature and save it in a separate file */
		case EXPORT_SIGNATURE|GENERATE_SIGNATURE:
			rc = find_certificate(ctxp->cms_ctx, 1);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_input(ctxp);
			open_sig_output(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->inpe, 1);
			generate_signature(ctxp->cms_ctx);
			export_signature(ctxp->cms_ctx, ctxp->outsigfd, ctxp->ascii);
			break;
		/* generate a signature and embed it in the binary */
		case IMPORT_SIGNATURE|GENERATE_SIGNATURE:
			check_inputs(ctxp);
			rc = find_certificate(ctxp->cms_ctx, 1);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			if (ctxp->signum > ctxp->cms_ctx->num_signatures + 1) {
				fprintf(stderr, "Invalid signature number.\n");
				exit(1);
			}
			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->outpe, 1);
			sigspace = calculate_signature_space(ctxp->cms_ctx,
							     ctxp->outpe);
			allocate_signature_space(ctxp->outpe, sigspace);
			generate_digest(ctxp->cms_ctx, ctxp->outpe, 1);
			generate_signature(ctxp->cms_ctx);
			insert_signature(ctxp->cms_ctx, ctxp->signum);
			close_output(ctxp);
			break;
		default:
			fprintf(stderr, "Incompatible flags (0x%08x): ", action);
			for (int i = 1; i < FLAG_LIST_END; i <<= 1) {
				if (action & i)
					print_flag_name(stderr, i);
			}
			fprintf(stderr, "\n");
			exit(1);
	}
}
