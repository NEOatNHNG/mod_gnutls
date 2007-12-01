/**
 *  Copyright 2004-2005 Paul Querna
 *  Copyright 2007 Nikos Mavrogiannopoulos
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "mod_gnutls.h"
#include "http_vhost.h"
#include "ap_mpm.h"

#if !USING_2_1_RECENT
extern server_rec *ap_server_conf;
#endif

#if APR_HAS_THREADS
GCRY_THREAD_OPTION_PTHREAD_IMPL;
#endif

#if MOD_GNUTLS_DEBUG
static apr_file_t *debug_log_fp;
#endif

static int mpm_is_threaded;

static int mgs_cert_verify(request_rec * r, mgs_handle_t * ctxt);
/* use side==0 for server and side==1 for client */
static void mgs_add_common_cert_vars(request_rec * r, gnutls_x509_crt cert,
				     int side,
				     int export_certificates_enabled);

static apr_status_t mgs_cleanup_pre_config(void *data)
{
    gnutls_global_deinit();
    return APR_SUCCESS;
}

#if MOD_GNUTLS_DEBUG
static void gnutls_debug_log_all(int level, const char *str)
{
    apr_file_printf(debug_log_fp, "<%d> %s\n", level, str);
}
#endif

int
mgs_hook_pre_config(apr_pool_t * pconf,
		    apr_pool_t * plog, apr_pool_t * ptemp)
{

#if APR_HAS_THREADS
    ap_mpm_query(AP_MPMQ_IS_THREADED, &mpm_is_threaded);
    if (mpm_is_threaded) {
	gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
    }
#else
    mpm_is_threaded = 0;
#endif

    gnutls_global_init();

    apr_pool_cleanup_register(pconf, NULL, mgs_cleanup_pre_config,
			      apr_pool_cleanup_null);

#if MOD_GNUTLS_DEBUG
    apr_file_open(&debug_log_fp, "/tmp/gnutls_debug",
		  APR_APPEND | APR_WRITE | APR_CREATE, APR_OS_DEFAULT,
		  pconf);

    gnutls_global_set_log_level(9);
    gnutls_global_set_log_function(gnutls_debug_log_all);
#endif

    return OK;
}


static gnutls_datum
load_params(const char *file, server_rec * s, apr_pool_t * pool)
{
    gnutls_datum ret = { NULL, 0 };
    apr_file_t *fp;
    apr_finfo_t finfo;
    apr_status_t rv;
    apr_size_t br = 0;

    rv = apr_file_open(&fp, file, APR_READ | APR_BINARY, APR_OS_DEFAULT,
		       pool);
    if (rv != APR_SUCCESS) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP, rv, s,
		     "GnuTLS failed to load params file at: %s. Will use internal params.",
		     file);
	return ret;
    }

    rv = apr_file_info_get(&finfo, APR_FINFO_SIZE, fp);

    if (rv != APR_SUCCESS) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP, rv, s,
		     "GnuTLS failed to stat params file at: %s", file);
	return ret;
    }

    ret.data = apr_palloc(pool, finfo.size + 1);
    rv = apr_file_read_full(fp, ret.data, finfo.size, &br);

    if (rv != APR_SUCCESS) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP, rv, s,
		     "GnuTLS failed to read params file at: %s", file);
	return ret;
    }
    apr_file_close(fp);
    ret.data[br] = '\0';
    ret.size = br;

    return ret;
}

/* We don't support openpgp certificates, yet */
const static int cert_type_prio[2] = { GNUTLS_CRT_X509, 0 };

static int mgs_select_virtual_server_cb(gnutls_session_t session)
{
    mgs_handle_t *ctxt;
    mgs_srvconf_rec *tsc;
    int ret;

    ctxt = gnutls_transport_get_ptr(session);

    /* find the virtual server */
    tsc = mgs_find_sni_server(session);

    if (tsc != NULL)
	ctxt->sc = tsc;

    gnutls_certificate_server_set_request(session,
					  ctxt->sc->client_verify_mode);

    /* set the new server credentials 
     */

    gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE,
			   ctxt->sc->certs);

    gnutls_credentials_set(session, GNUTLS_CRD_ANON, ctxt->sc->anon_creds);

    if (ctxt->sc->srp_tpasswd_conf_file != NULL
	&& ctxt->sc->srp_tpasswd_file != NULL) {
	gnutls_credentials_set(session, GNUTLS_CRD_SRP,
			       ctxt->sc->srp_creds);
    }

    /* update the priorities - to avoid negotiating a ciphersuite that is not
     * enabled on this virtual server. Note that here we ignore the version
     * negotiation.
     */
    ret = gnutls_priority_set(session, ctxt->sc->priorities);
    gnutls_certificate_type_set_priority(session, cert_type_prio);


    /* actually it shouldn't fail since we have checked at startup */
    if (ret < 0)
	return ret;

    /* allow separate caches per virtual host. Actually allowing the same is a
     * bad idea, since they might have different security requirements.
     */
    mgs_cache_session_init(ctxt);

    return 0;
}

static int cert_retrieve_fn(gnutls_session_t session, gnutls_retr_st * ret)
{
    mgs_handle_t *ctxt;

    ctxt = gnutls_transport_get_ptr(session);

    ret->type = GNUTLS_CRT_X509;
    ret->ncerts = 1;
    ret->deinit_all = 0;

    ret->cert.x509 = &ctxt->sc->cert_x509;
    ret->key.x509 = ctxt->sc->privkey_x509;
    return 0;
}

const char static_dh_params[] = "-----BEGIN DH PARAMETERS-----\n"
    "MIIBBwKCAQCsa9tBMkqam/Fm3l4TiVgvr3K2ZRmH7gf8MZKUPbVgUKNzKcu0oJnt\n"
    "gZPgdXdnoT3VIxKrSwMxDc1/SKnaBP1Q6Ag5ae23Z7DPYJUXmhY6s2YaBfvV+qro\n"
    "KRipli8Lk7hV+XmT7Jde6qgNdArb9P90c1nQQdXDPqcdKB5EaxR3O8qXtDoj+4AW\n"
    "dr0gekNsZIHx0rkHhxdGGludMuaI+HdIVEUjtSSw1X1ep3onddLs+gMs+9v1L7N4\n"
    "YWAnkATleuavh05zA85TKZzMBBx7wwjYKlaY86jQw4JxrjX46dv7tpS1yAPYn3rk\n"
    "Nd4jbVJfVHWbZeNy/NaO8g+nER+eSv9zAgEC\n"
    "-----END DH PARAMETERS-----\n";

/* Read the common name or the alternative name of the certificate.
 * We only support a single name per certificate.
 *
 * Returns negative on error.
 */
static int read_crt_cn(apr_pool_t * p, gnutls_x509_crt cert,
		       char **cert_cn)
{
    int rv = 0, i;
    size_t data_len;


    *cert_cn = NULL;

    rv = gnutls_x509_crt_get_dn_by_oid(cert,
				       GNUTLS_OID_X520_COMMON_NAME,
				       0, 0, NULL, &data_len);

    if (rv >= 0 && data_len > 1) {
	*cert_cn = apr_palloc(p, data_len);
	rv = gnutls_x509_crt_get_dn_by_oid(cert,
					   GNUTLS_OID_X520_COMMON_NAME, 0,
					   0, *cert_cn, &data_len);
    } else {			/* No CN return subject alternative name */

	/* read subject alternative name */
	for (i = 0; !(rv < 0); i++) {
	    rv = gnutls_x509_crt_get_subject_alt_name(cert, i,
		    NULL, &data_len, NULL);
		    
	    if (rv == GNUTLS_SAN_DNSNAME) {
		*cert_cn = apr_palloc(p, data_len);
		rv = gnutls_x509_crt_get_subject_alt_name(cert, i,
		    *cert_cn, &data_len, NULL);
                break;

	    }
	}
    }
    
    return rv;

}

int
mgs_hook_post_config(apr_pool_t * p, apr_pool_t * plog,
		     apr_pool_t * ptemp, server_rec * base_server)
{
    int rv;
    server_rec *s;
    gnutls_dh_params_t dh_params;
    gnutls_rsa_params_t rsa_params;
    mgs_srvconf_rec *sc;
    mgs_srvconf_rec *sc_base;
    void *data = NULL;
    int first_run = 0;
    const char *userdata_key = "mgs_init";

    apr_pool_userdata_get(&data, userdata_key, base_server->process->pool);
    if (data == NULL) {
	first_run = 1;
	apr_pool_userdata_set((const void *) 1, userdata_key,
			      apr_pool_cleanup_null,
			      base_server->process->pool);
    }


    {
	gnutls_datum pdata;
	apr_pool_t *tpool;
	s = base_server;
	sc_base =
	    (mgs_srvconf_rec *) ap_get_module_config(s->module_config,
						     &gnutls_module);

	apr_pool_create(&tpool, p);

	gnutls_dh_params_init(&dh_params);

	pdata = load_params(sc_base->dh_params_file, s, tpool);

	if (pdata.size != 0) {
	    rv = gnutls_dh_params_import_pkcs3(dh_params, &pdata,
					       GNUTLS_X509_FMT_PEM);
	    if (rv != 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Unable to load DH Params: (%d) %s",
			     rv, gnutls_strerror(rv));
		exit(rv);
	    }
	} else {
	    /* If the file does not exist use internal parameters
	     */
	    pdata.data = (void *) static_dh_params;
	    pdata.size = sizeof(static_dh_params);
	    rv = gnutls_dh_params_import_pkcs3(dh_params, &pdata,
					       GNUTLS_X509_FMT_PEM);

	    if (rv < 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Unable to load internal DH Params."
			     " Shutting down.");
		exit(-1);
	    }
	}
	apr_pool_clear(tpool);

	rsa_params = NULL;

	pdata = load_params(sc_base->rsa_params_file, s, tpool);

	if (pdata.size != 0) {
	    gnutls_rsa_params_init(&rsa_params);
	    rv = gnutls_rsa_params_import_pkcs1(rsa_params, &pdata,
						GNUTLS_X509_FMT_PEM);
	    if (rv != 0) {
		ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, s,
			     "GnuTLS: Unable to load RSA Params: (%d) %s",
			     rv, gnutls_strerror(rv));
		exit(rv);
	    }
	}
	/* not an error but RSA-EXPORT ciphersuites are not available 
	 */

	apr_pool_destroy(tpool);
	rv = mgs_cache_post_config(p, s, sc_base);
	if (rv != 0) {
	    ap_log_error(APLOG_MARK, APLOG_STARTUP, rv, s,
			 "GnuTLS: Post Config for GnuTLSCache Failed."
			 " Shutting Down.");
	    exit(-1);
	}

	for (s = base_server; s; s = s->next) {
	    sc = (mgs_srvconf_rec *) ap_get_module_config(s->module_config,
							  &gnutls_module);
	    sc->cache_type = sc_base->cache_type;
	    sc->cache_config = sc_base->cache_config;

	    if (rsa_params != NULL)
		gnutls_certificate_set_rsa_export_params(sc->certs,
							 rsa_params);
	    gnutls_certificate_set_dh_params(sc->certs, dh_params);

	    gnutls_anon_set_server_dh_params(sc->anon_creds, dh_params);

	    gnutls_certificate_server_set_retrieve_function(sc->certs,
							    cert_retrieve_fn);

	    if (sc->srp_tpasswd_conf_file != NULL
		&& sc->srp_tpasswd_file != NULL) {
		gnutls_srp_set_server_credentials_file(sc->srp_creds,
						       sc->
						       srp_tpasswd_file,
						       sc->
						       srp_tpasswd_conf_file);
	    }

	    if (sc->cert_x509 == NULL
		&& sc->enabled == GNUTLS_ENABLED_TRUE) {
		ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
			     "[GnuTLS] - Host '%s:%d' is missing a "
			     "Certificate File!", s->server_hostname,
			     s->port);
		exit(-1);
	    }

	    if (sc->privkey_x509 == NULL
		&& sc->enabled == GNUTLS_ENABLED_TRUE) {
		ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s,
			     "[GnuTLS] - Host '%s:%d' is missing a "
			     "Private Key File!",
			     s->server_hostname, s->port);
		exit(-1);
	    }

	    if (sc->enabled == GNUTLS_ENABLED_TRUE) {
		rv = read_crt_cn(p, sc->cert_x509, &sc->cert_cn);
		if (rv < 0) {
		    sc->enabled = GNUTLS_ENABLED_FALSE;
		    sc->cert_cn = NULL;
		    continue;
		}
	    }
	}
    }

    ap_add_version_component(p, "mod_gnutls/" MOD_GNUTLS_VERSION);

    return OK;
}

void mgs_hook_child_init(apr_pool_t * p, server_rec * s)
{
    apr_status_t rv = APR_SUCCESS;
    mgs_srvconf_rec *sc = ap_get_module_config(s->module_config,
					       &gnutls_module);

    if (sc->cache_type != mgs_cache_none) {
	rv = mgs_cache_child_init(p, s, sc);
	if (rv != APR_SUCCESS) {
	    ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s,
			 "[GnuTLS] - Failed to run Cache Init");
	}
    } else {
	ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
		     "[GnuTLS] - No Cache Configured. Hint: GnuTLSCache");
    }
}

const char *mgs_hook_http_scheme(const request_rec * r)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(r->server->module_config,
						 &gnutls_module);

    if (sc->enabled == GNUTLS_ENABLED_FALSE) {
	return NULL;
    }

    return "https";
}

apr_port_t mgs_hook_default_port(const request_rec * r)
{
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(r->server->module_config,
						 &gnutls_module);

    if (sc->enabled == GNUTLS_ENABLED_FALSE) {
	return 0;
    }

    return 443;
}

#define MAX_HOST_LEN 255

#if USING_2_1_RECENT
typedef struct {
    mgs_handle_t *ctxt;
    mgs_srvconf_rec *sc;
    const char *sni_name;
} vhost_cb_rec;

static int vhost_cb(void *baton, conn_rec * conn, server_rec * s)
{
    mgs_srvconf_rec *tsc;
    vhost_cb_rec *x = baton;

    tsc = (mgs_srvconf_rec *) ap_get_module_config(s->module_config,
						   &gnutls_module);

    if (tsc->enabled != GNUTLS_ENABLED_TRUE || tsc->cert_cn == NULL) {
	return 0;
    }

    /* The CN can contain a * -- this will match those too. */
    if (ap_strcasecmp_match(x->sni_name, tsc->cert_cn) == 0) {
	/* found a match */
#if MOD_GNUTLS_DEBUG
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
		     x->ctxt->c->base_server,
		     "GnuTLS: Virtual Host CB: "
		     "'%s' == '%s'", tsc->cert_cn, x->sni_name);
#endif
	/* Because we actually change the server used here, we need to reset
	 * things like ClientVerify.
	 */
	x->sc = tsc;
	/* Shit. Crap. Dammit. We *really* should rehandshake here, as our
	 * certificate structure *should* change when the server changes. 
	 * acccckkkkkk. 
	 */
	return 1;
    }
    return 0;
}
#endif

mgs_srvconf_rec *mgs_find_sni_server(gnutls_session_t session)
{
    int rv;
    unsigned int sni_type;
    size_t data_len = MAX_HOST_LEN;
    char sni_name[MAX_HOST_LEN];
    mgs_handle_t *ctxt;
#if USING_2_1_RECENT
    vhost_cb_rec cbx;
#else
    server_rec *s;
    mgs_srvconf_rec *tsc;
#endif

    ctxt = gnutls_transport_get_ptr(session);

    sni_type = gnutls_certificate_type_get(session);
    if (sni_type != GNUTLS_CRT_X509) {
	/* In theory, we could support OpenPGP Certificates. Theory != code. */
	ap_log_error(APLOG_MARK, APLOG_CRIT, 0,
		     ctxt->c->base_server,
		     "GnuTLS: Only x509 Certificates are currently supported.");
	return NULL;
    }

    rv = gnutls_server_name_get(ctxt->session, sni_name,
				&data_len, &sni_type, 0);

    if (rv != 0) {
	return NULL;
    }

    if (sni_type != GNUTLS_NAME_DNS) {
	ap_log_error(APLOG_MARK, APLOG_CRIT, 0,
		     ctxt->c->base_server,
		     "GnuTLS: Unknown type '%d' for SNI: "
		     "'%s'", sni_type, sni_name);
	return NULL;
    }

    /**
     * Code in the Core already sets up the c->base_server as the base
     * for this IP/Port combo.  Trust that the core did the 'right' thing.
     */
#if USING_2_1_RECENT
    cbx.ctxt = ctxt;
    cbx.sc = NULL;
    cbx.sni_name = sni_name;

    rv = ap_vhost_iterate_given_conn(ctxt->c, vhost_cb, &cbx);
    if (rv == 1) {
	return cbx.sc;
    }
#else
    for (s = ap_server_conf; s; s = s->next) {

	tsc = (mgs_srvconf_rec *) ap_get_module_config(s->module_config,
						       &gnutls_module);
	if (tsc->enabled != GNUTLS_ENABLED_TRUE) {
	    continue;
	}
#if MOD_GNUTLS_DEBUG
	ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
		     ctxt->c->base_server,
		     "GnuTLS: sni-x509 cn: %s/%d pk: %s s: 0x%08X s->n: 0x%08X  sc: 0x%08X",
		     tsc->cert_cn, rv,
		     gnutls_pk_algorithm_get_name
		     (gnutls_x509_privkey_get_pk_algorithm
		      (ctxt->sc->privkey_x509)), (unsigned int) s,
		     (unsigned int) s->next, (unsigned int) tsc);
#endif
	/* The CN can contain a * -- this will match those too. */
	if (ap_strcasecmp_match(sni_name, tsc->cert_cn) == 0) {
#if MOD_GNUTLS_DEBUG
	    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0,
			 ctxt->c->base_server,
			 "GnuTLS: Virtual Host: "
			 "'%s' == '%s'", tsc->cert_cn, sni_name);
#endif
	    return tsc;
	}
    }
#endif
    return NULL;
}


static const int protocol_priority[] = {
    GNUTLS_TLS1_1, GNUTLS_TLS1_0, GNUTLS_SSL3, 0
};


static mgs_handle_t *create_gnutls_handle(apr_pool_t * pool, conn_rec * c)
{
    mgs_handle_t *ctxt;
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(c->base_server->
						 module_config,
						 &gnutls_module);

    ctxt = apr_pcalloc(pool, sizeof(*ctxt));
    ctxt->c = c;
    ctxt->sc = sc;
    ctxt->status = 0;

    ctxt->input_rc = APR_SUCCESS;
    ctxt->input_bb = apr_brigade_create(c->pool, c->bucket_alloc);
    ctxt->input_cbuf.length = 0;

    ctxt->output_rc = APR_SUCCESS;
    ctxt->output_bb = apr_brigade_create(c->pool, c->bucket_alloc);
    ctxt->output_blen = 0;
    ctxt->output_length = 0;

    gnutls_init(&ctxt->session, GNUTLS_SERVER);

    /* This is not very good as it trades security for compatibility,
     * but it is the only way to be ultra-portable.
     */
    gnutls_session_enable_compatibility_mode(ctxt->session);

    /* because we don't set any default priorities here (we set later at
     * the user hello callback) we need to at least set this in order for
     * gnutls to be able to read packets.
     */
    gnutls_protocol_set_priority(ctxt->session, protocol_priority);

    gnutls_handshake_set_post_client_hello_function(ctxt->session,
						    mgs_select_virtual_server_cb);

    return ctxt;
}

int mgs_hook_pre_connection(conn_rec * c, void *csd)
{
    mgs_handle_t *ctxt;
    mgs_srvconf_rec *sc =
	(mgs_srvconf_rec *) ap_get_module_config(c->base_server->
						 module_config,
						 &gnutls_module);

    if (!(sc && (sc->enabled == GNUTLS_ENABLED_TRUE))) {
	return DECLINED;
    }

    ctxt = create_gnutls_handle(c->pool, c);

    ap_set_module_config(c->conn_config, &gnutls_module, ctxt);

    gnutls_transport_set_pull_function(ctxt->session, mgs_transport_read);
    gnutls_transport_set_push_function(ctxt->session, mgs_transport_write);
    gnutls_transport_set_ptr(ctxt->session, ctxt);

    ctxt->input_filter =
	ap_add_input_filter(GNUTLS_INPUT_FILTER_NAME, ctxt, NULL, c);
    ctxt->output_filter =
	ap_add_output_filter(GNUTLS_OUTPUT_FILTER_NAME, ctxt, NULL, c);

    return OK;
}

int mgs_hook_fixups(request_rec * r)
{
    unsigned char sbuf[GNUTLS_MAX_SESSION_ID];
    char buf[AP_IOBUFSIZE];
    const char *tmp;
    size_t len;
    mgs_handle_t *ctxt;
    int rv = OK;

    apr_table_t *env = r->subprocess_env;

    ctxt =
	ap_get_module_config(r->connection->conn_config, &gnutls_module);

    if (!ctxt) {
	return DECLINED;
    }

    apr_table_setn(env, "HTTPS", "on");

    apr_table_setn(env, "SSL_VERSION_LIBRARY",
		   "GnuTLS/" LIBGNUTLS_VERSION);
    apr_table_setn(env, "SSL_VERSION_INTERFACE",
		   "mod_gnutls/" MOD_GNUTLS_VERSION);

    apr_table_setn(env, "SSL_PROTOCOL",
		   gnutls_protocol_get_name(gnutls_protocol_get_version
					    (ctxt->session)));

    /* should have been called SSL_CIPHERSUITE instead */
    apr_table_setn(env, "SSL_CIPHER",
		   gnutls_cipher_suite_get_name(gnutls_kx_get
						(ctxt->session),
						gnutls_cipher_get(ctxt->
								  session),
						gnutls_mac_get(ctxt->
							       session)));

    apr_table_setn(env, "SSL_COMPRESS_METHOD",
		   gnutls_compression_get_name(gnutls_compression_get
					       (ctxt->session)));

    apr_table_setn(env, "SSL_SRP_USER",
		   gnutls_srp_server_get_username(ctxt->session));

    if (apr_table_get(env, "SSL_CLIENT_VERIFY") == NULL)
	apr_table_setn(env, "SSL_CLIENT_VERIFY", "NONE");

    unsigned int key_size =
	8 * gnutls_cipher_get_key_size(gnutls_cipher_get(ctxt->session));
    tmp = apr_psprintf(r->pool, "%u", key_size);

    apr_table_setn(env, "SSL_CIPHER_USEKEYSIZE", tmp);

    apr_table_setn(env, "SSL_CIPHER_ALGKEYSIZE", tmp);

    apr_table_setn(env, "SSL_CIPHER_EXPORT",
		   (key_size <= 40) ? "true" : "false");

    len = sizeof(sbuf);
    gnutls_session_get_id(ctxt->session, sbuf, &len);
    tmp = mgs_session_id2sz(sbuf, len, buf, sizeof(buf));
    apr_table_setn(env, "SSL_SESSION_ID", apr_pstrdup(r->pool, tmp));

    mgs_add_common_cert_vars(r, ctxt->sc->cert_x509, 0,
			     ctxt->sc->export_certificates_enabled);

    return rv;
}

int mgs_hook_authz(request_rec * r)
{
    int rv;
    mgs_handle_t *ctxt;
    mgs_dirconf_rec *dc = ap_get_module_config(r->per_dir_config,
					       &gnutls_module);

    ctxt =
	ap_get_module_config(r->connection->conn_config, &gnutls_module);

    if (!ctxt) {
	return DECLINED;
    }

    if (dc->client_verify_mode == GNUTLS_CERT_IGNORE) {
	ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
		      "GnuTLS: Directory set to Ignore Client Certificate!");
    } else {
	if (ctxt->sc->client_verify_mode < dc->client_verify_mode) {
	    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
			  "GnuTLS: Attempting to rehandshake with peer. %d %d",
			  ctxt->sc->client_verify_mode,
			  dc->client_verify_mode);

	    gnutls_certificate_server_set_request(ctxt->session,
						  dc->client_verify_mode);

	    if (mgs_rehandshake(ctxt) != 0) {
		return HTTP_FORBIDDEN;
	    }
	} else if (ctxt->sc->client_verify_mode == GNUTLS_CERT_IGNORE) {
#if MOD_GNUTLS_DEBUG
	    ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
			  "GnuTLS: Peer is set to IGNORE");
#endif
	} else {
	    rv = mgs_cert_verify(r, ctxt);
	    if (rv != DECLINED) {
		return rv;
	    }
	}
    }

    return DECLINED;
}

/* variables that are not sent by default:
 *
 * SSL_CLIENT_CERT 	string 	PEM-encoded client certificate
 * SSL_SERVER_CERT 	string 	PEM-encoded client certificate
 */

/* side is either 0 for SERVER or 1 for CLIENT
 */
#define MGS_SIDE ((side==0)?"SSL_SERVER":"SSL_CLIENT")
static void
mgs_add_common_cert_vars(request_rec * r, gnutls_x509_crt cert, int side,
			 int export_certificates_enabled)
{
    unsigned char sbuf[64];	/* buffer to hold serials */
    char buf[AP_IOBUFSIZE];
    const char *tmp;
    size_t len;
    int alg;

    apr_table_t *env = r->subprocess_env;

    if (export_certificates_enabled != 0) {
	char cert_buf[10 * 1024];
	len = sizeof(cert_buf);

	if (gnutls_x509_crt_export
	    (cert, GNUTLS_X509_FMT_PEM, cert_buf, &len) >= 0)
	    apr_table_setn(env,
			   apr_pstrcat(r->pool, MGS_SIDE, "_CERT", NULL),
			   apr_pstrmemdup(r->pool, cert_buf, len));

    }

    len = sizeof(buf);
    gnutls_x509_crt_get_dn(cert, buf, &len);
    apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_S_DN", NULL),
		   apr_pstrmemdup(r->pool, buf, len));

    len = sizeof(buf);
    gnutls_x509_crt_get_issuer_dn(cert, buf, &len);
    apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_I_DN", NULL),
		   apr_pstrmemdup(r->pool, buf, len));

    len = sizeof(sbuf);
    gnutls_x509_crt_get_serial(cert, sbuf, &len);
    tmp = mgs_session_id2sz(sbuf, len, buf, sizeof(buf));
    apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_M_SERIAL", NULL),
		   apr_pstrdup(r->pool, tmp));

    tmp =
	mgs_time2sz(gnutls_x509_crt_get_expiration_time
		    (cert), buf, sizeof(buf));
    apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_V_END", NULL),
		   apr_pstrdup(r->pool, tmp));

    tmp =
	mgs_time2sz(gnutls_x509_crt_get_activation_time
		    (cert), buf, sizeof(buf));
    apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_V_START", NULL),
		   apr_pstrdup(r->pool, tmp));

    alg = gnutls_x509_crt_get_signature_algorithm(cert);
    if (alg >= 0) {
	apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_A_SIG", NULL),
		       gnutls_sign_algorithm_get_name(alg));
    }

    alg = gnutls_x509_crt_get_pk_algorithm(cert, NULL);
    if (alg >= 0) {
	apr_table_setn(env, apr_pstrcat(r->pool, MGS_SIDE, "_A_KEY", NULL),
		       gnutls_pk_algorithm_get_name(alg));
    }


}


static int mgs_cert_verify(request_rec * r, mgs_handle_t * ctxt)
{
    const gnutls_datum_t *cert_list;
    unsigned int cert_list_size, status, expired;
    int rv, ret;
    gnutls_x509_crt_t cert;
    apr_time_t activation_time, expiration_time, cur_time;

    cert_list =
	gnutls_certificate_get_peers(ctxt->session, &cert_list_size);

    if (cert_list == NULL || cert_list_size == 0) {
	/* It is perfectly OK for a client not to send a certificate if on REQUEST mode
	 */
	if (ctxt->sc->client_verify_mode == GNUTLS_CERT_REQUEST)
	    return OK;

	/* no certificate provided by the client, but one was required. */
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Failed to Verify Peer: "
		      "Client did not submit a certificate");
	return HTTP_FORBIDDEN;
    }

    if (cert_list_size > 1) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Failed to Verify Peer: "
		      "Chained Client Certificates are not supported.");
	return HTTP_FORBIDDEN;
    }

    gnutls_x509_crt_init(&cert);
    rv = gnutls_x509_crt_import(cert, &cert_list[0], GNUTLS_X509_FMT_DER);
    if (rv < 0) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Failed to Verify Peer: "
		      "Failed to import peer certificates.");
	ret = HTTP_FORBIDDEN;
	goto exit;
    }

    apr_time_ansi_put(&expiration_time,
		      gnutls_x509_crt_get_expiration_time(cert));
    apr_time_ansi_put(&activation_time,
		      gnutls_x509_crt_get_activation_time(cert));

    rv = gnutls_x509_crt_verify(cert, ctxt->sc->ca_list,
				ctxt->sc->ca_list_size, 0, &status);

    if (rv < 0) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Failed to Verify Peer certificate: (%d) %s",
		      rv, gnutls_strerror(rv));
	ret = HTTP_FORBIDDEN;
	goto exit;
    }

    expired = 0;
    cur_time = apr_time_now();
    if (activation_time > cur_time) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Failed to Verify Peer: "
		      "Peer Certificate is not yet activated.");
	expired = 1;
    }

    if (expiration_time < cur_time) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Failed to Verify Peer: "
		      "Peer Certificate is expired.");
	expired = 1;
    }

    if (status & GNUTLS_CERT_SIGNER_NOT_FOUND) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Could not find Signer for Peer Certificate");
    }

    if (status & GNUTLS_CERT_SIGNER_NOT_CA) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Peer's Certificate signer is not a CA");
    }

    if (status & GNUTLS_CERT_INVALID) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Peer Certificate is invalid.");
    } else if (status & GNUTLS_CERT_REVOKED) {
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r,
		      "GnuTLS: Peer Certificate is revoked.");
    }

    /* TODO: Further Verification. */
    /* Revocation is X.509 non workable paradigm, I really doubt implementation
     * is worth doing --nmav
     */
/// ret = gnutls_x509_crt_check_revocation(crt, crl_list, crl_list_size);

//    mgs_hook_fixups(r);
//    rv = mgs_authz_lua(r);

    mgs_add_common_cert_vars(r, cert, 1,
			     ctxt->sc->export_certificates_enabled);

    {
	/* days remaining */
	unsigned long remain =
	    (apr_time_sec(expiration_time) -
	     apr_time_sec(cur_time)) / 86400;
	apr_table_setn(r->subprocess_env, "SSL_CLIENT_V_REMAIN",
		       apr_psprintf(r->pool, "%lu", remain));
    }

    if (status == 0 && expired == 0) {
	apr_table_setn(r->subprocess_env, "SSL_CLIENT_VERIFY", "SUCCESS");
	ret = OK;
    } else {
	apr_table_setn(r->subprocess_env, "SSL_CLIENT_VERIFY", "FAILED");
	if (ctxt->sc->client_verify_mode == GNUTLS_CERT_REQUEST)
	    ret = OK;
	else
	    ret = HTTP_FORBIDDEN;
    }

  exit:
    gnutls_x509_crt_deinit(cert);
    return ret;


}