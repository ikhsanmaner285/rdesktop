/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Protocol services - TCP layer
   Copyright (C) Matthew Chapman <matthewc.unsw.edu.au> 1999-2008
   Copyright 2005-2011 Peter Astrand <astrand@cendio.se> for Cendio AB
   Copyright 2012-2019 Henrik Andersson <hean01@cendio.se> for Cendio AB
   Copyright 2017 Alexander Zakharov <uglym8@gmail.com>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _WIN32
#include <unistd.h>		/* select read write close */
#include <sys/socket.h>		/* socket connect setsockopt */
#include <sys/time.h>		/* timeval */
#include <sys/stat.h>
#include <netdb.h>		/* gethostbyname */
#include <netinet/in.h>		/* sockaddr_in */
#include <netinet/tcp.h>	/* TCP_NODELAY */
#include <arpa/inet.h>		/* inet_addr */
#include <errno.h>		/* errno */
#include <assert.h>
#endif

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <nettle/base64.h>

#include "rdesktop.h"
#include "ssl.h"
#include "asn.h"


#define CHECK(x) assert((x)>=0)

#ifdef _WIN32
#define socklen_t int
#define TCP_CLOSE(_sck) closesocket(_sck)
#define TCP_STRERROR "tcp error"
#define TCP_BLOCKS (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#define TCP_CLOSE(_sck) close(_sck)
#define TCP_STRERROR strerror(errno)
#define TCP_BLOCKS (errno == EWOULDBLOCK)
#endif

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long) -1)
#endif

#ifdef WITH_SCARD
#define STREAM_COUNT 8
#else
#define STREAM_COUNT 1
#endif

#ifdef IPv6
static struct addrinfo *g_server_address = NULL;
#else
struct sockaddr_in *g_server_address = NULL;
#endif

static char *g_last_server_name = NULL;
static RD_BOOL g_ssl_initialized = False;
static int g_sock;
static RD_BOOL g_run_ui = False;
static struct stream g_in;
static struct stream g_out[STREAM_COUNT];
int g_tcp_port_rdp = TCP_PORT_RDP;

extern RD_BOOL g_exit_mainloop;
extern RD_BOOL g_network_error;
extern RD_BOOL g_reconnect_loop;
extern char g_tls_version[];

static gnutls_session_t g_tls_session;

/*
 * Let's roll our own routines for GnuTLS pubkey store
 *
 * Store layout:
 *
 *  ~/.local/share/rdesktop/cert/
 *       |
 *       |-- a7b2373e
 *       |-- b1b78a40
 *
 *  Where filenames are a d2j hash of host
 *
 *  Content of file is two lines, first line a timestamp of when
 *  certificate expires and second line a base64 encoded public key
 *
 */

static void
cert_store_cache_filename(const char *db_name, const char *host, char *result, size_t size)
{
	uint32 hash;
	hash = utils_djb2_hash(host);
	snprintf(result, size, "%s/%x", db_name, hash);
}

static int
cert_tdb_store(const char* db_name, const char* host, const char* service,
	time_t expiration, const gnutls_datum_t* pubkey)
{
	UNUSED(service);
	FILE *out;
	char filename[4096];
	char b64[BASE64_ENCODE_RAW_LENGTH(4096)];

	base64_encode_raw(b64, pubkey->size, pubkey->data);

	/* store pubkey */
	cert_store_cache_filename(db_name, host, filename, sizeof(filename));
	logger(Core, Debug, "%s(), store pubkey in '%s'",__func__, filename);

	out = fopen(filename, "w+");
	fprintf(out, "%ld\n", expiration);
	fprintf(out, "%s\n", b64);
	fclose(out);

	return GNUTLS_E_SUCCESS;
}

static int
cert_tdb_verify(const char* db_name, const char* host, const char* service,
	const gnutls_datum_t* pubkey)
{
	UNUSED(service);

	FILE *in;
	gnutls_datum_t store_pubkey;
	char buf[4096];
	uint8 dst[4096];
	char filename[4096];
	struct base64_decode_ctx ctx;
	size_t dst_size;

	cert_store_cache_filename(db_name, host, filename, sizeof(filename));

	logger(Core, Debug, "%s(), verify public key for %s",__func__, host);

	in = fopen(filename, "r");
	if (in == NULL)
	{
		logger(Core, Warning, "%s(), no cached public key found for host '%s'", __func__, host);
		unlink(filename);
		return GNUTLS_E_NO_CERTIFICATE_FOUND;
	}

	/* get expiration line */
	if (fgets(buf, sizeof(buf), in) == NULL)
	{
		logger(Core, Error, "%s(), invalid content of public key cache '%s'", __func__, filename);
		unlink(filename);
		return GNUTLS_E_NO_CERTIFICATE_FOUND;
	}

	/* get base64 line */
	if (fgets(buf, sizeof(buf), in) == NULL)
	{
		logger(Core, Error, "%s(), invalid content of public key cache '%s'", __func__, filename);
		unlink(filename);
		return GNUTLS_E_NO_CERTIFICATE_FOUND;
	}

	fclose(in);

	/* base64 decode stored key and compare */
	base64_decode_init(&ctx);
	dst_size = sizeof(dst);
	if (base64_decode_update(&ctx, &dst_size, dst, strlen(buf), buf) == 0)
	{
		logger(Core, Error, "%s(), failed to base64 decode public key from cache", __func__);
		unlink(filename);
		return GNUTLS_E_CERTIFICATE_KEY_MISMATCH;
	}
	base64_decode_final(&ctx);
	store_pubkey.size = dst_size;
	store_pubkey.data = dst;

	/* verify public key against cached key */
	if (pubkey->size != store_pubkey.size)
	{
		return GNUTLS_E_CERTIFICATE_KEY_MISMATCH;
	}

	if (memcmp(pubkey->data, store_pubkey.data, pubkey->size) != 0)
	{
		return GNUTLS_E_CERTIFICATE_KEY_MISMATCH;
	}

	/* Found mathcing public key in cache */
	return GNUTLS_E_SUCCESS;
}

static int
cert_tdb_store_commitment(const char* db_name, const char* host, const char* service,
	time_t expiration, gnutls_digest_algorithm_t algorithm, const gnutls_datum_t* hash)
{
	UNUSED(db_name);
	UNUSED(expiration);
	UNUSED(service);
	UNUSED(algorithm);
	UNUSED(hash);

	/* We don't use this, left as NOP */
	logger(Core, Warning, "Storing commitement for %s", host);

	return GNUTLS_E_SUCCESS;
}

static gnutls_tdb_t g_tdb;

static int
cert_store_init(gnutls_tdb_t *tdb)
{
	gnutls_tdb_init(tdb);
	gnutls_tdb_set_store_func(*tdb, cert_tdb_store);
	gnutls_tdb_set_verify_func(*tdb, cert_tdb_verify);
	gnutls_tdb_set_store_commitment_func(*tdb, cert_tdb_store_commitment);
	return 0;
}

/* wait till socket is ready to write or timeout */
static RD_BOOL
tcp_can_send(int sck, int millis)
{
	fd_set wfds;
	struct timeval time;
	int sel_count;

	time.tv_sec = millis / 1000;
	time.tv_usec = (millis * 1000) % 1000000;
	FD_ZERO(&wfds);
	FD_SET(sck, &wfds);
	sel_count = select(sck + 1, 0, &wfds, 0, &time);
	if (sel_count > 0)
	{
		return True;
	}
	return False;
}

/* Initialise TCP transport data packet */
STREAM
tcp_init(uint32 maxlen)
{
	static int cur_stream_id = 0;
	STREAM result = NULL;

#ifdef WITH_SCARD
	scard_lock(SCARD_LOCK_TCP);
#endif
	result = &g_out[cur_stream_id];
	s_realloc(result, maxlen);
	s_reset(result);
	cur_stream_id = (cur_stream_id + 1) % STREAM_COUNT;
#ifdef WITH_SCARD
	scard_unlock(SCARD_LOCK_TCP);
#endif
	return result;
}

/* Send TCP transport data packet */
void
tcp_send(STREAM s)
{
	int length = s->end - s->data;
	int sent, total = 0;

	if (g_network_error == True)
		return;

#ifdef WITH_SCARD
	scard_lock(SCARD_LOCK_TCP);
#endif

	while (total < length)
	{
		if (g_ssl_initialized) {
			sent = gnutls_record_send(g_tls_session, s->data + total, length - total);
			if (sent <= 0) {
				if (gnutls_error_is_fatal(sent)) {
#ifdef WITH_SCARD
					scard_unlock(SCARD_LOCK_TCP);
#endif
					logger(Core, Error, "tcp_send(), gnutls_record_send() failed with %d: %s\n", sent, gnutls_strerror(sent));
					g_network_error = True;
					return;
				} else {
					tcp_can_send(g_sock, 100);
					sent = 0;
				}
			}
		}
		else
		{
			sent = send(g_sock, s->data + total, length - total, 0);
			if (sent <= 0)
			{
				if (sent == -1 && TCP_BLOCKS)
				{
					tcp_can_send(g_sock, 100);
					sent = 0;
				}
				else
				{
#ifdef WITH_SCARD
					scard_unlock(SCARD_LOCK_TCP);
#endif
					logger(Core, Error, "tcp_send(), send() failed: %s",
					       TCP_STRERROR);
					g_network_error = True;
					return;
				}
			}
		}
		total += sent;
	}
#ifdef WITH_SCARD
	scard_unlock(SCARD_LOCK_TCP);
#endif
}

/* Receive a message on the TCP layer */
STREAM
tcp_recv(STREAM s, uint32 length)
{
	uint32 new_length, end_offset, p_offset;
	int rcvd = 0;

	if (g_network_error == True)
		return NULL;

	if (s == NULL)
	{
		/* read into "new" stream */
		if (length > g_in.size)
		{
			g_in.data = (uint8 *) xrealloc(g_in.data, length);
			g_in.size = length;
		}
		g_in.end = g_in.p = g_in.data;
		s = &g_in;
	}
	else
	{
		/* append to existing stream */
		new_length = (s->end - s->data) + length;
		if (new_length > s->size)
		{
			p_offset = s->p - s->data;
			end_offset = s->end - s->data;
			s->data = (uint8 *) xrealloc(s->data, new_length);
			s->size = new_length;
			s->p = s->data + p_offset;
			s->end = s->data + end_offset;
		}
	}

	while (length > 0)
	{

		if ((!g_ssl_initialized || (gnutls_record_check_pending(g_tls_session) <= 0)) && g_run_ui)
		{
			ui_select(g_sock);

			/* break out of recv, if request of exiting
			   main loop has been done */
			if (g_exit_mainloop == True)
				return NULL;
		}

		if (g_ssl_initialized) {
			rcvd = gnutls_record_recv(g_tls_session, s->end, length);

			if (rcvd < 0) {
				if (gnutls_error_is_fatal(rcvd)) {
					logger(Core, Error, "tcp_recv(), gnutls_record_recv() failed with %d: %s\n", rcvd, gnutls_strerror(rcvd));
					g_network_error = True;
					return NULL;
				} else {
					rcvd = 0;
				}
			}

		}
		else
		{
			rcvd = recv(g_sock, s->end, length, 0);
			if (rcvd < 0)
			{
				if (rcvd == -1 && TCP_BLOCKS)
				{
					rcvd = 0;
				}
				else
				{
					logger(Core, Error, "tcp_recv(), recv() failed: %s",
							TCP_STRERROR);
					g_network_error = True;
					return NULL;
				}
			}
			else if (rcvd == 0)
			{
				logger(Core, Error, "rcp_recv(), connection closed by peer");
				return NULL;
			}
		}

		s->end += rcvd;
		length -= rcvd;
	}

	return s;
}

int check_cert(gnutls_session_t session)
{
	int rv;
	char *home;
	char certcache_dir[PATH_MAX];

	struct stat sb;

	int type;
	time_t exp_time;
	gnutls_x509_crt_t cert;
	gnutls_datum_t cinfo;
	const gnutls_datum_t *cert_list;
	unsigned int cert_list_size = 0;

	size_t size;
	char dn[256];
	char *name;

	home = getenv("HOME");

	if (home == NULL)
		return False;

	snprintf(certcache_dir, sizeof(certcache_dir) - 1, "%s/%s", home, ".local/share/rdesktop/certs/");

	if ((rv = stat(certcache_dir, &sb)) == -1) {

		if (errno == ENOENT) {
			if (rd_certcache_mkdir() == False) {
				goto bail;
			}
		}
	} else {
		if ((sb.st_mode & S_IFMT) != S_IFDIR) {
			logger(Core, Error, "%s: %s exists but it's not a directory", __func__, certcache_dir);
			goto bail;
		}
	}

	type = gnutls_certificate_type_get(session);

	if (type == GNUTLS_CRT_X509) {

		cert_list = gnutls_certificate_get_peers(session, &cert_list_size);

		if (cert_list_size > 0) {
			gnutls_x509_crt_init(&cert);
			gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER);

			size = sizeof(dn);
			gnutls_x509_crt_get_dn(cert, dn, &size);

			if (size > 0) {
				name = strstr(dn, "CN=");

				if (!name) {
					logger(Core, Error, "%s: Failed to find CN in Distinguished Name part of certificate", __func__);
					goto bail;
				}

				name += 3;

				if (!strlen(name)) {
					logger(Core, Error, "%s: DN length is 0", __func__);
					goto bail;
				}

			} else {
				logger(Core, Error, "%s: Failed to get DN from certificate", __func__);
				goto bail;
			}

			/*
			 * uglym8: we can't rely on hostname being consistent here as we can connect
			 * to tunneled host (e.g. via ssh) so we're going to use DN as a hostname
			 *
			 */
			rv = gnutls_verify_stored_pubkey(certcache_dir, g_tdb, name, "rdesktop", type, &cert_list[0], 0);

			if (rv == GNUTLS_E_NO_CERTIFICATE_FOUND) {
				logger(Core, Debug, "%s: %s: No previous stored certificate for the host '%s'. Storing it into the cache", __func__, name);

				exp_time = gnutls_x509_crt_get_expiration_time(cert);
				rv = gnutls_store_pubkey(certcache_dir, g_tdb, name, "rdesktop", type, &cert_list[0], exp_time, 0);

				if (rv != GNUTLS_E_SUCCESS) {
					logger(Core, Error, "%s: Failed to store certificate. error = 0x%x (%s)", __func__, rv, gnutls_strerror(rv));
					goto bail;
				}

			} else if (rv == GNUTLS_E_CERTIFICATE_KEY_MISMATCH) {
				const char *response;
				char message[2048];

				snprintf(message, sizeof(message),
					"Host '%s' is known but has another key associated with it, \n"
					, name);

				rv = gnutls_x509_crt_print(cert, GNUTLS_CRT_PRINT_ONELINE, &cinfo);
				if (rv == 0)
				{
					char *p;
					strcat(message, "review the following certificate info:\n\n");

					/* replace ',' with '\n' for simpler format */
					p = (char *)cinfo.data;
					while(*p != '\0')
					{
						if (*p == ',') *p = '\n';
						p++;
					}
					strcat(message, " ");
					strncat(message, (char *)cinfo.data, cinfo.size);
					gnutls_free(cinfo.data);
				}
				else
				{
					logger(Core, Error, "%s: Failed to print the certificate. error = 0x%x (%s)", __func__, rv, gnutls_strerror(rv));

					strcat(message,
						"rdesktop failed to parse the certificate and there for " \
						"we can not display certificate information for you to "  \
						" inspect the change.\n\n");
				}

				strcat(message, "\n\nDo you trust this certificate (yes/no)? ");

				response = util_dialog_choice(message, "no", "yes", NULL);
				if (strcmp(response, "no") == 0 || response == NULL)
					goto bail;

				//logger(Core, Debug, "%s: %s: Replacing certificate for the host '%s'.", __func__, name);
				logger(Core, Debug, "%s: %s: Adding a new certificate for the host '%s'.", __func__, name);

				exp_time = gnutls_x509_crt_get_expiration_time(cert);
				rv = gnutls_store_pubkey(certcache_dir, g_tdb, name, "rdesktop", type, &cert_list[0], exp_time, 0);

				if (rv != GNUTLS_E_SUCCESS) {
					logger(Core, Error, "%s: Failed to store certificate. error = 0x%x (%s)", __func__, rv, gnutls_strerror(rv));
					goto bail;
				}

			} else if (rv < 0) {
				fprintf(stderr, "%s: gnutls_verify_stored_pubkey: %s\n", __func__, gnutls_strerror(rv));
				logger(Core, Error, "%s: Verification for host '%s' certificate failed. Error = 0x%x (%s)", __func__, name, rv, gnutls_strerror(rv));
				goto bail;
			} else {
				logger(Core, Debug, "%s: %s: Host %s is known and the key is OK.", __func__, name);
			}
		}
	}

	return 0;

bail:
	return 1;
}

/* Establish a SSL/TLS 1.0 connection */
RD_BOOL
tcp_tls_connect(void)
{
	int err;

	int type;
	int status;
	gnutls_datum_t out;
	gnutls_certificate_credentials_t xcred;

	/* Initialize TLS session */
	if (!g_ssl_initialized)
	{
		gnutls_global_init();
		CHECK(gnutls_init(&g_tls_session, GNUTLS_CLIENT));
		CHECK(cert_store_init(&g_tdb));
		g_ssl_initialized = True;
	}

	/* It is recommended to use the default priorities */
	//CHECK(gnutls_set_default_priority(g_tls_session));
	// Use compatible priority to overcome key validation error
	// THIS IS TEMPORARY
	CHECK(gnutls_priority_set_direct(g_tls_session, "NORMAL:%COMPAT", NULL));
	CHECK(gnutls_certificate_allocate_credentials(&xcred));
	CHECK(gnutls_credentials_set(g_tls_session, GNUTLS_CRD_CERTIFICATE, xcred));

#if GNUTLS_VERSION_NUMBER >= 0x030109
	gnutls_transport_set_int(g_tls_session, g_sock);
#else
	gnutls_transport_set_ptr(g_tls_session, (gnutls_transport_ptr_t)g_sock);
#endif

#if GNUTLS_VERSION_NUMBER >= 0x030100
	gnutls_handshake_set_timeout(g_tls_session, GNUTLS_DEFAULT_HANDSHAKE_TIMEOUT);
#endif

	/* Perform the TLS handshake */
	do {
		err = gnutls_handshake(g_tls_session);
	} while (err < 0 && gnutls_error_is_fatal(err) == 0);

	if (err < 0) {

#if GNUTLS_VERSION_NUMBER >= 0x030406
		if (err == GNUTLS_E_CERTIFICATE_VERIFICATION_ERROR) {
			/* check certificate verification status */
			type = gnutls_certificate_type_get(g_tls_session);
			status = gnutls_session_get_verify_cert_status(g_tls_session);
			CHECK(gnutls_certificate_verification_status_print(status, type, &out, 0));
			gnutls_free(out.data);
		}
#endif

		goto fail;

	} else {
#if GNUTLS_VERSION_NUMBER >= 0x03010a
		char *desc;
		desc = gnutls_session_get_desc(g_tls_session);
		logger(Core, Verbose, "TLS  Session info: %s\n", desc);
		gnutls_free(desc);
#endif
	}

	//if (check_cert(g_tls_session) != 0) goto fail;
	if (check_cert(g_tls_session) != 0) {
		fprintf(stdout, "%s: Failed to check certificate. Bailing out\n", __func__);
		exit (1);
	}

	return True;

fail:

	if (g_ssl_initialized) {
		gnutls_deinit(g_tls_session);
		// Not needed since 3.3.0
		gnutls_global_deinit();

		g_ssl_initialized = False;
	}

	return False;
}

/* Get public key from server of TLS 1.x connection */
RD_BOOL
tcp_tls_get_server_pubkey(STREAM s)
{
	int ret;
	unsigned int list_size;
	const gnutls_datum_t *cert_list;
	gnutls_x509_crt_t cert;

	unsigned int algo, bits;
	gnutls_datum_t m, e;

	int pk_size;
	uint8_t pk_data[1024];

	s->data = s->p = NULL;
	s->size = 0;

	cert_list = gnutls_certificate_get_peers(g_tls_session, &list_size);

	if (!cert_list) {
		logger(Core, Error, "%s:%s:%d Failed to get peer's certs' list\n", __FILE__, __func__, __LINE__);
		goto out;
	}

	if ((ret = gnutls_x509_crt_init(&cert)) != GNUTLS_E_SUCCESS) {
		logger(Core, Error, "%s:%s:%d Failed to init certificate structure. GnuTLS error: %s\n",
				__FILE__, __func__, __LINE__, gnutls_strerror(ret));
		goto out;
	}

	if ((ret = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER)) != GNUTLS_E_SUCCESS) {
		logger(Core, Error, "%s:%s:%d Failed to import DER certificate. GnuTLS error:%s\n",
				__FILE__, __func__, __LINE__, gnutls_strerror(ret));
		goto out;
	}

	algo = gnutls_x509_crt_get_pk_algorithm(cert, &bits);

	if (algo == GNUTLS_PK_RSA) {
		if ((ret = gnutls_x509_crt_get_pk_rsa_raw(cert, &m, &e)) !=  GNUTLS_E_SUCCESS) {
			logger(Core, Error, "%s:%s:%d Failed to get RSA public key parameters from certificate. GnuTLS error:%s\n",
					__FILE__, __func__, __LINE__, gnutls_strerror(ret));
			goto out;
		}
	} else {
			logger(Core, Error, "%s:%s:%d Peer's certificate public key algorithm is not RSA. GnuTLS error:%s\n",
					__FILE__, __func__, __LINE__, gnutls_strerror(algo));
			goto out;
	}

	pk_size = sizeof(pk_data);

	/*
	 * This key will be used further in cssp_connect() for server's key comparison.
	 *
	 * Note that we need to encode this RSA public key into PKCS#1 DER
	 * ATM there's no way to encode/export RSA public key to PKCS#1 using GnuTLS,
	 * gnutls_pubkey_export() encodes into PKCS#8. So besides fixing GnuTLS
	 * we can use libtasn1 for encoding.
	 */

	if ((ret = write_pkcs1_der_pubkey(&m, &e, pk_data, &pk_size)) != 0) {
			logger(Core, Error, "%s:%s:%d Failed to encode RSA public key to PKCS#1 DER\n",
					__FILE__, __func__, __LINE__);
			goto out;
	}

	s->size = pk_size;
	s->data = s->p = xmalloc(s->size);
	memcpy((void *)s->data, (void *)pk_data, pk_size);
	s->p = s->data;
	s->end = s->p + s->size;

out:
	if ((e.size != 0) && (e.data)) {
		free(e.data);
	}

	if ((m.size != 0) && (m.data)) {
		free(m.data);
	}

	return (s->size != 0);
}

/* Helper function to determine if rdesktop should resolve hostnames again or not */
static RD_BOOL
tcp_connect_resolve_hostname(const char *server)
{
	return (g_server_address == NULL ||
		g_last_server_name == NULL || strcmp(g_last_server_name, server) != 0);
}

/* Establish a connection on the TCP layer

   This function tries to avoid resolving any server address twice. The
   official Windows 2008 documentation states that the windows farm name
   should be a round-robin DNS entry containing all the terminal servers
   in the farm. When connected to the farm address, if we look up the
   address again when reconnecting (for any reason) we risk reconnecting
   to a different server in the farm.
*/

RD_BOOL
tcp_connect(char *server)
{
	socklen_t option_len;
	uint32 option_value;
	int i;
	char buf[NI_MAXHOST];

#ifdef IPv6

	int n;
	struct addrinfo hints, *res, *addr;
	struct sockaddr *oldaddr;
	char tcp_port_rdp_s[10];

	if (tcp_connect_resolve_hostname(server))
	{
		snprintf(tcp_port_rdp_s, 10, "%d", g_tcp_port_rdp);

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		if ((n = getaddrinfo(server, tcp_port_rdp_s, &hints, &res)))
		{
			logger(Core, Error, "tcp_connect(), getaddrinfo() failed: %s",
			       gai_strerror(n));
			return False;
		}
	}
	else
	{
		res = g_server_address;
	}

	g_sock = -1;

	for (addr = res; addr != NULL; addr = addr->ai_next)
	{
		g_sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
		if (g_sock < 0)
		{
			logger(Core, Debug, "tcp_connect(), socket() failed: %s", TCP_STRERROR);
			continue;
		}

		n = getnameinfo(addr->ai_addr, addr->ai_addrlen, buf, sizeof(buf), NULL, 0,
				NI_NUMERICHOST);
		if (n != 0)
		{
			logger(Core, Error, "tcp_connect(), getnameinfo() failed: %s",
			       gai_strerror(n));
			return False;
		}

		logger(Core, Debug, "tcp_connect(), trying %s (%s)", server, buf);

		if (connect(g_sock, addr->ai_addr, addr->ai_addrlen) == 0)
			break;

		TCP_CLOSE(g_sock);
		g_sock = -1;
	}

	if (g_sock == -1)
	{
		logger(Core, Error, "tcp_connect(), unable to connect to %s", server);
		return False;
	}

	/* Save server address for later use, if we haven't already. */

	if (g_server_address == NULL)
	{
		g_server_address = xmalloc(sizeof(struct addrinfo));
		g_server_address->ai_addr = xmalloc(sizeof(struct sockaddr_storage));
	}

	if (g_server_address != addr)
	{
		/* don't overwrite ptr to allocated sockaddr */
		oldaddr = g_server_address->ai_addr;
		memcpy(g_server_address, addr, sizeof(struct addrinfo));
		g_server_address->ai_addr = oldaddr;

		memcpy(g_server_address->ai_addr, addr->ai_addr, addr->ai_addrlen);

		g_server_address->ai_canonname = NULL;
		g_server_address->ai_next = NULL;

		freeaddrinfo(res);
	}

#else /* no IPv6 support */
	struct hostent *nslookup = NULL;

	if (tcp_connect_resolve_hostname(server))
	{
		if (g_server_address != NULL)
			xfree(g_server_address);
		g_server_address = xmalloc(sizeof(struct sockaddr_in));
		g_server_address->sin_family = AF_INET;
		g_server_address->sin_port = htons((uint16) g_tcp_port_rdp);

		if ((nslookup = gethostbyname(server)) != NULL)
		{
			memcpy(&g_server_address->sin_addr, nslookup->h_addr,
			       sizeof(g_server_address->sin_addr));
		}
		else if ((g_server_address->sin_addr.s_addr = inet_addr(server)) == INADDR_NONE)
		{
			logger(Core, Error, "tcp_connect(), unable to resolve host '%s'", server);
			return False;
		}
	}

	if ((g_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		logger(Core, Error, "tcp_connect(), socket() failed: %s", TCP_STRERROR);
		return False;
	}

	logger(Core, Debug, "tcp_connect(), trying %s (%s)",
	       server, inet_ntop(g_server_address->sin_family,
				 &g_server_address->sin_addr, buf, sizeof(buf)));

	if (connect(g_sock, (struct sockaddr *) g_server_address, sizeof(struct sockaddr)) < 0)
	{
		if (!g_reconnect_loop)
			logger(Core, Error, "tcp_connect(), connect() failed: %s", TCP_STRERROR);

		TCP_CLOSE(g_sock);
		g_sock = -1;
		return False;
	}

#endif /* IPv6 */

	option_value = 1;
	option_len = sizeof(option_value);
	setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (void *) &option_value, option_len);
	/* receive buffer must be a least 16 K */
	if (getsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (void *) &option_value, &option_len) == 0)
	{
		if (option_value < (1024 * 16))
		{
			option_value = 1024 * 16;
			option_len = sizeof(option_value);
			setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (void *) &option_value,
				   option_len);
		}
	}

	g_in.size = 4096;
	g_in.data = (uint8 *) xmalloc(g_in.size);

	for (i = 0; i < STREAM_COUNT; i++)
	{
		g_out[i].size = 4096;
		g_out[i].data = (uint8 *) xmalloc(g_out[i].size);
	}

	/* After successful connect: update the last server name */
	if (g_last_server_name)
		xfree(g_last_server_name);
	g_last_server_name = strdup(server);
	return True;
}

/* Disconnect on the TCP layer */
void
tcp_disconnect(void)
{
	int i;

	if (g_ssl_initialized) {
		(void)gnutls_bye(g_tls_session, GNUTLS_SHUT_WR);
		gnutls_deinit(g_tls_session);
		// Not needed since 3.3.0
		gnutls_global_deinit();

		g_ssl_initialized = False;
	}

	TCP_CLOSE(g_sock);
	g_sock = -1;

	g_in.size = 0;
	xfree(g_in.data);
	g_in.data = NULL;

	for (i = 0; i < STREAM_COUNT; i++)
	{
		g_out[i].size = 0;
		xfree(g_out[i].data);
		g_out[i].data = NULL;
	}
}

char *
tcp_get_address()
{
	static char ipaddr[32];
	struct sockaddr_in sockaddr;
	socklen_t len = sizeof(sockaddr);
	if (getsockname(g_sock, (struct sockaddr *) &sockaddr, &len) == 0)
	{
		uint8 *ip = (uint8 *) & sockaddr.sin_addr;
		sprintf(ipaddr, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
	}
	else
		strcpy(ipaddr, "127.0.0.1");
	return ipaddr;
}

RD_BOOL
tcp_is_connected()
{
	struct sockaddr_in sockaddr;
	socklen_t len = sizeof(sockaddr);
	if (getpeername(g_sock, (struct sockaddr *) &sockaddr, &len))
		return True;
	return False;
}

/* reset the state of the tcp layer */
/* Support for Session Directory */
void
tcp_reset_state(void)
{
	int i;

	/* Clear the incoming stream */
	s_reset(&g_in);

	/* Clear the outgoing stream(s) */
	for (i = 0; i < STREAM_COUNT; i++)
	{
		s_reset(&g_out[i]);
	}
}

void
tcp_run_ui(RD_BOOL run)
{
	g_run_ui = run;
}
