/***************************************************************************
 * Copyright (c) 2009-2010, Open Information Security Foundation
 * Copyright (c) 2009-2012, Qualys, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * * Neither the name of the Qualys, Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/

/**
 * @file
 * @author Ivan Ristic <ivanr@webkreator.com>
 */

#include "htp_private.h"

/**
 * Invoked to process a part of request body data.
 *
 * @param[in] d
 */
int htp_ch_urlencoded_callback_request_body_data(htp_tx_data_t *d) {
    if (d->data != NULL) {
        // Process one chunk of data
        htp_urlenp_parse_partial(d->tx->request_urlenp_body, d->data, d->len);
    } else {
        // Finalize parsing
        htp_urlenp_finalize(d->tx->request_urlenp_body);

        if (d->tx->connp->cfg->parameter_processor == NULL) {
            // We are going to use the parser table directly
            d->tx->request_params_body = d->tx->request_urlenp_body->params;
            d->tx->request_params_body_reused = 1;

            htp_transcode_params(d->tx->connp, &d->tx->request_params_body, 0);
        } else {
            // We have a parameter processor defined, which means we'll
            // need to create a new table
            d->tx->request_params_body = htp_table_create(htp_table_size(d->tx->request_urlenp_body->params));

            // Transform parameters and store them into the new table
            bstr *name = NULL;
            bstr *value = NULL;
            for (int i = 0, n = htp_table_size(d->tx->request_urlenp_body->params); i < n; i++) {
                htp_table_get_index(d->tx->request_urlenp_body->params, i, &name, (void **)&value);
                d->tx->connp->cfg->parameter_processor(d->tx->request_params_body, name, value);
                // TODO Check return code
            }  

            htp_transcode_params(d->tx->connp, &d->tx->request_params_body, 1);
        }       
    }

    return HTP_OK;
}

/**
 * Determine if the request has a URLENCODED body, then
 * create and attach the URLENCODED parser if it does.
 */
int htp_ch_urlencoded_callback_request_headers(htp_connp_t *connp) {
    // Check the request content type to see if it matches our MIME type
    if ((connp->in_tx->request_content_type == NULL) || (bstr_cmp_c(connp->in_tx->request_content_type, HTP_URLENCODED_MIME_TYPE) != 0)) {
        #ifdef HTP_DEBUG
        fprintf(stderr, "htp_ch_urlencoded_callback_request_headers: Body not URLENCODED\n");
        #endif

        return HTP_OK;
    }
    
    #ifdef HTP_DEBUG
    fprintf(stderr, "htp_ch_urlencoded_callback_request_headers: Parsing URLENCODED body\n");
    #endif

    // Create parser instance
    connp->in_tx->request_urlenp_body = htp_urlenp_create(connp->in_tx);
    if (connp->in_tx->request_urlenp_body == NULL) {
        return HTP_ERROR;
    }

    // Register request body data callbacks
    htp_tx_register_request_body_data(connp->in_tx, htp_ch_urlencoded_callback_request_body_data);

    return HTP_OK;
}

/**
 * Parse query string, if available. This method is invoked after the
 * request line has been processed.
 *
 * @param[in] connp
 */
int htp_ch_urlencoded_callback_request_line(htp_connp_t *connp) {    
    // Parse query string, when available
    if ((connp->in_tx->parsed_uri->query != NULL) && (bstr_len(connp->in_tx->parsed_uri->query) > 0)) {
        connp->in_tx->request_urlenp_query = htp_urlenp_create(connp->in_tx);
        if (connp->in_tx->request_urlenp_query == NULL) {
            return HTP_ERROR;
        }       

        htp_urlenp_parse_complete(connp->in_tx->request_urlenp_query,
            (unsigned char *) bstr_ptr(connp->in_tx->parsed_uri->query),
            bstr_len(connp->in_tx->parsed_uri->query));       

        // Is there a parameter processor?
        if (connp->cfg->parameter_processor == NULL) {
            // There's no parameter processor
            
            if (connp->cfg->internal_encoding == NULL) {
                // No transcoding; use the parser table directly
                connp->in_tx->request_params_query = connp->in_tx->request_urlenp_query->params;
                connp->in_tx->request_params_query_reused = 1;
            } else {
                // Transcode values
                connp->in_tx->request_params_query = connp->in_tx->request_urlenp_query->params;
                htp_transcode_params(connp, &connp->in_tx->request_params_query, 0);
            }
        } else {            
            // We have a parameter processor defined, which 
            // means we'll need to create a new table
            
            connp->in_tx->request_params_query = htp_table_create(htp_table_size(connp->in_tx->request_urlenp_query->params));

            // Use the parameter processor on each parameter, storing
            // the results in the newly created table            
            bstr *name = NULL;
            bstr *value = NULL;            
            for (int i = 0, n = htp_table_size(connp->in_tx->request_params_query); i < n; i++) {
                htp_table_get_index(connp->in_tx->request_params_query, i, &name, (void **)&value);
                connp->cfg->parameter_processor(connp->in_tx->request_params_query, name, value);
                // TODO Check return code
            }            

            // Transcode as necessary
            htp_transcode_params(connp, &connp->in_tx->request_params_query, 1);
        }       
    }

    return HTP_OK;
}

/**
 * Finalize MULTIPART processing.
 * 
 * @param[in] d
 */
int htp_ch_multipart_callback_request_body_data(htp_tx_data_t *d) {
    if (d->data != NULL) {
        // Process one chunk of data
        htp_mpartp_parse(d->tx->request_mpartp, d->data, d->len);
    } else {
        // Finalize parsing
        htp_mpartp_finalize(d->tx->request_mpartp);

        d->tx->request_params_body = htp_table_create(htp_list_size(d->tx->request_mpartp->parts));
        // TODO RC

        // Extract parameters
        d->tx->request_params_body_reused = 1;
                
        for (int i = 0, n = htp_list_size(d->tx->request_mpartp->parts); i < n; i++) {
            htp_mpart_part_t *part = htp_list_get(d->tx->request_mpartp->parts, i);

            // Only use text parameters
            if (part->type == MULTIPART_PART_TEXT) {                
                if (d->tx->connp->cfg->parameter_processor == NULL) {
                    htp_table_add(d->tx->request_params_body, part->name, part->value);
                } else {
                    d->tx->connp->cfg->parameter_processor(d->tx->request_params_body, part->name, part->value);
                }
            }
        }
    }

    return HTP_OK;
}

/**
 * Inspect request headers and register the MULTIPART request data hook
 * if it contains a multipart/form-data body.
 *
 * @param[in] connp
 */
int htp_ch_multipart_callback_request_headers(htp_connp_t *connp) {
    // Check the request content type to see if it matches our MIME type
    if ((connp->in_tx->request_content_type == NULL) || (bstr_cmp_c(connp->in_tx->request_content_type, HTP_MULTIPART_MIME_TYPE) != 0)) {
        #ifdef HTP_DEBUG
        fprintf(stderr, "htp_ch_multipart_callback_request_headers: Body not MULTIPART\n");
        #endif

        return HTP_OK;
    }
    
    #ifdef HTP_DEBUG
    fprintf(stderr, "htp_ch_multipart_callback_request_headers: Parsing MULTIPART body\n");
    #endif

    htp_header_t *ct = htp_table_get_c(connp->in_tx->request_headers, "content-type");
    if (ct == NULL) return HTP_OK;

    char *boundary = NULL;

    int rc = htp_mpartp_extract_boundary(ct->value, &boundary);
    if (rc != HTP_OK) {
        // TODO Invalid boundary
        return HTP_OK;
    }

    // Create parser instance
    connp->in_tx->request_mpartp = htp_mpartp_create(connp->cfg, boundary);
    if (connp->in_tx->request_mpartp == NULL) {
        free(boundary);
        return HTP_ERROR;
    }

    if (connp->cfg->extract_request_files) {
        connp->in_tx->request_mpartp->extract_files = 1;
        connp->in_tx->request_mpartp->extract_dir = connp->cfg->tmpdir;
    }

    free(boundary);

    // Register request body data callbacks
    htp_tx_register_request_body_data(connp->in_tx, htp_ch_multipart_callback_request_body_data);

    return HTP_OK;
}
