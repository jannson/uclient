/*
 * uclient - ustream based protocol client library
 *
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <unistd.h>
#include <stdio.h>
#include <dlfcn.h>
#include <getopt.h>
#include <fcntl.h>

#include <libubox/blobmsg.h>

#include "uclient.h"

#ifdef __APPLE__
#define LIB_EXT "dylib"
#else
#define LIB_EXT "so"
#endif

static struct ustream_ssl_ctx *ssl_ctx;
static const struct ustream_ssl_ops *ssl_ops;
static int quiet = false;
static bool verify = true;
static const char *output_file;
static int output_fd = -1;
static int error_ret;

static int open_output_file(const char *path, bool create)
{
	const char *str;
	char *filename;
	int len;
	int flags = O_WRONLY;

	if (create)
		flags |= O_CREAT;

	if (output_file) {
		if (!strcmp(output_file, "-"))
			return STDOUT_FILENO;

		return open(output_file, flags, 0644);
	}

	/* Don't automatically overwrite files if the name is derived from the URL */
	if (create)
		flags |= O_EXCL;

	len = strcspn(path, ";&");
	while (len > 0 && path[len - 1] == '/')
		len--;

	for (str = path + len - 1; str >= path; str--) {
		if (*str == '/')
			break;
	}

	str++;
	len -= str - path;

	if (len > 0) {
		filename = alloca(len + 1);
		strncpy(filename, str, len);
		filename[len] = 0;
	} else {
		filename = "index.html";
	}

	return open(filename, flags, 0644);
}

static void request_done(struct uclient *cl)
{
	if (output_fd >= 0) {
		close(output_fd);
		output_fd = -1;
	}
	uclient_disconnect(cl);
	uloop_end();
}

static void example_header_done(struct uclient *cl)
{
	static int retries;

	struct blob_attr *cur;
	int rem;

	if (retries < 10 && uclient_http_redirect(cl)) {
		if (!quiet)
			fprintf(stderr, "Redirected to %s on %s\n", cl->url->location, cl->url->host);

		retries++;
		return;
	}

	retries = 0;
	if (!quiet) {
		fprintf(stderr, "Headers (%d): \n", cl->status_code);
		blobmsg_for_each_attr(cur, cl->meta, rem) {
			fprintf(stderr, "%s=%s\n", blobmsg_name(cur), (char *) blobmsg_data(cur));
		}
	}

	switch (cl->status_code) {
	case 204:
	case 200:
		output_fd = open_output_file(cl->url->location, true);
		if (output_fd < 0) {
			if (!quiet)
				perror("Cannot open output file");
			error_ret = 3;
			request_done(cl);
		}
		break;

	default:
		request_done(cl);
		error_ret = 8;
		break;
	}
}

static void example_read_data(struct uclient *cl)
{
	char buf[256];
	int len;

	if (output_fd < 0)
		return;

	while (1) {
		len = uclient_read(cl, buf, sizeof(buf));
		if (!len)
			return;

		write(output_fd, buf, len);
	}
}

static void msg_connecting(struct uclient *cl)
{
	char addr[INET6_ADDRSTRLEN];
	int port;

	if (quiet)
		return;

	uclient_get_addr(addr, &port, &cl->remote_addr);
	fprintf(stderr, "Connecting to %s %s:%d\n", cl->url->host, addr, port);
}

static void init_request(struct uclient *cl)
{
	uclient_connect(cl);
	msg_connecting(cl);
	uclient_http_set_request_type(cl, "GET");
	uclient_request(cl);
}

static void example_eof(struct uclient *cl)
{
	request_done(cl);
}

static void handle_uclient_error(struct uclient *cl, int code)
{
	const char *type = "Unknown error";
	bool ignore = false;

	switch(code) {
	case UCLIENT_ERROR_CONNECT:
		type = "Connection failed";
		error_ret = 4;
		break;
	case UCLIENT_ERROR_SSL_INVALID_CERT:
		type = "Invalid SSL certificate";
		ignore = !verify;
		error_ret = 5;
		break;
	case UCLIENT_ERROR_SSL_CN_MISMATCH:
		type = "Server hostname does not match SSL certificate";
		ignore = !verify;
		error_ret = 5;
		break;
	default:
		error_ret = 1;
		break;
	}

	if (!quiet)
		fprintf(stderr, "Connection error: %s%s\n", type, ignore ? " (ignored)" : "");

	if (ignore)
		error_ret = 0;
	else
		request_done(cl);
}

static const struct uclient_cb cb = {
	.header_done = example_header_done,
	.data_read = example_read_data,
	.data_eof = example_eof,
	.error = handle_uclient_error,
};

static int usage(const char *progname)
{
	fprintf(stderr,
		"Usage: %s [options] <URL>\n"
		"Options:\n"
		"	-O <file>:                      Redirect output to file (use \"-\" for stdout)\n"
		"\n"
		"HTTPS options:\n"
		"	--ca-certificate=<cert>:        Load CA certificates from file <cert>\n"
		"	--no-check-certificate:         don't validate the server's certificate\n"
		"\n", progname);
	return 1;
}


static void init_ustream_ssl(void)
{
	void *dlh;

	dlh = dlopen("libustream-ssl." LIB_EXT, RTLD_LAZY | RTLD_LOCAL);
	if (!dlh)
		return;

	ssl_ops = dlsym(dlh, "ustream_ssl_ops");
	if (!ssl_ops)
		return;

	ssl_ctx = ssl_ops->context_new(false);
}

static int no_ssl(const char *progname)
{
	fprintf(stderr, "%s: SSL support not available, please install ustream-ssl\n", progname);
	return 1;
}

enum {
	L_NO_CHECK_CERTIFICATE,
	L_CA_CERTIFICATE,
};

static const struct option longopts[] = {
	[L_NO_CHECK_CERTIFICATE] = { "no-check-certificate", no_argument },
	[L_CA_CERTIFICATE] = { "ca-certificate", required_argument },
	{}
};

int main(int argc, char **argv)
{
	const char *progname = argv[0];
	struct uclient *cl;
	int ch;
	int longopt_idx = 0;

	init_ustream_ssl();

	while ((ch = getopt_long(argc, argv, "qO:", longopts, &longopt_idx)) != -1) {
		switch(ch) {
		case 0:
			switch (longopt_idx) {
			case L_NO_CHECK_CERTIFICATE:
				verify = false;
				break;
			case L_CA_CERTIFICATE:
				if (ssl_ctx)
					ssl_ops->context_add_ca_crt_file(ssl_ctx, optarg);
				break;
			default:
				return usage(progname);
			}
			break;
		case 'O':
			output_file = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		default:
			return usage(progname);
		}
	}

	argv += optind;
	argc -= optind;

	if (argc != 1)
		return usage(progname);

	if (!strncmp(argv[0], "https", 5) && !ssl_ctx)
		return no_ssl(progname);

	uloop_init();

	cl = uclient_new(argv[0], NULL, &cb);
	if (!cl) {
		fprintf(stderr, "Failed to allocate uclient context\n");
		return 1;
	}

	if (ssl_ctx)
		uclient_http_set_ssl_ctx(cl, ssl_ops, ssl_ctx, verify);

	init_request(cl);
	uloop_run();
	uloop_done();

	uclient_free(cl);

	if (ssl_ctx)
		ssl_ops->context_free(ssl_ctx);

	return error_ret;
}
