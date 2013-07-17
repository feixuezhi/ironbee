/***************************************************************************
 * Copyright (c) 2009-2010 Open Information Security Foundation
 * Copyright (c) 2010-2013 Qualys, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.

 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.

 * - Neither the name of the Qualys, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
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
 * Extract one request header. A header can span multiple lines, in
 * which case they will be folded into one before parsing is attempted.
 *
 * @param[in] connp
 * @param[in] data
 * @param[in] len
 * @return HTP_OK or HTP_ERROR
 */
htp_status_t htp_process_request_header_apache_2_2(htp_connp_t *connp, unsigned char *data, size_t len) {
    // Create a new header structure.
    htp_header_t *h = calloc(1, sizeof (htp_header_t));
    if (h == NULL) return HTP_ERROR;

    // Now try to parse the header.
    if (htp_parse_request_header_apache_2_2(connp, h, data, len) != HTP_OK) {
        free(h);
        return HTP_ERROR;
    }

    #ifdef HTP_DEBUG
    fprint_bstr(stderr, "Header name", h->name);
    fprint_bstr(stderr, "Header value", h->value);
    #endif

    // Do we already have a header with the same name?
    htp_header_t *h_existing = htp_table_get(connp->in_tx->request_headers, h->name);
    if (h_existing != NULL) {
        // TODO Do we want to have a list of the headers that are
        //      allowed to be combined in this way?

        // Add to the existing header.
        bstr *new_value = bstr_expand(h_existing->value, bstr_len(h_existing->value) + 2 + bstr_len(h->value));
        if (new_value == NULL) {
            bstr_free(h->name);
            bstr_free(h->value);
            free(h);
            return HTP_ERROR;
        }

        h_existing->value = new_value;
        bstr_add_mem_noex(h_existing->value, (unsigned char *) ", ", 2);
        bstr_add_noex(h_existing->value, h->value);

        // The new header structure is no longer needed.
        bstr_free(h->name);
        bstr_free(h->value);
        free(h);

        // Keep track of repeated same-name headers.
        h_existing->flags |= HTP_FIELD_REPEATED;
    } else {
        // Add as a new header.
        htp_table_add(connp->in_tx->request_headers, h->name, h);
    }

    return HTP_OK;
}

/**
 * Parses a message header line as Apache 2.2 does.
 *
 * @param[in] connp
 * @param[in] h
 * @param[in] data
 * @param[in] len
 * @return HTP_OK or HTP_ERROR
 */
htp_status_t htp_parse_request_header_apache_2_2(htp_connp_t *connp, htp_header_t *h, unsigned char *data, size_t len) {
    size_t name_start, name_end;
    size_t value_start, value_end;

    htp_chomp(data, &len);

    name_start = 0;

    // Look for the colon.
    size_t colon_pos = 0;
    while ((colon_pos < len) && (data[colon_pos] != '\0') && (data[colon_pos] != ':')) colon_pos++;

    if ((colon_pos == len) || (data[colon_pos] == '\0')) {
        // Missing colon.
        
        h->flags |= HTP_FIELD_UNPARSEABLE;

        // Log only once per transaction.
        if (!(connp->in_tx->flags & HTP_FIELD_UNPARSEABLE)) {
            connp->in_tx->flags |= HTP_FIELD_UNPARSEABLE;            
            htp_log(connp, HTP_LOG_MARK, HTP_LOG_WARNING, 0, "Request field invalid: colon missing");
        }

        // We handle this case as a header with an empty name, with the value equal
        // to the entire input string.

        // TODO Apache will respond to this problem with a 400.

        // Now extract the name and the value
        h->name = bstr_dup_c("");
        if (h->name == NULL) return HTP_ERROR;

        h->value = bstr_dup_mem(data, len);
        if (h->value == NULL) {
            bstr_free(h->name);
            return HTP_ERROR;
        }

        return HTP_OK;
    }

    if (colon_pos == 0) {
        // Empty header name.

        h->flags |= HTP_FIELD_INVALID;

        // Log only once per transaction.
        if (!(connp->in_tx->flags & HTP_FIELD_INVALID)) {
            connp->in_tx->flags |= HTP_FIELD_INVALID;            
            htp_log(connp, HTP_LOG_MARK, HTP_LOG_WARNING, 0, "Request field invalid: empty name");
        }
    }

    name_end = colon_pos;

    // Ignore LWS after field-name.
    size_t prev = name_end;
    while ((prev > name_start) && (htp_is_lws(data[prev - 1]))) {
        // LWS after header name.

        prev--;
        name_end--;

        h->flags |= HTP_FIELD_INVALID;

        // Log only once per transaction.
        if (!(connp->in_tx->flags & HTP_FIELD_INVALID)) {
            connp->in_tx->flags |= HTP_FIELD_INVALID;            
            htp_log(connp, HTP_LOG_MARK, HTP_LOG_WARNING, 0, "Request field invalid: LWS after name");
        }
    }

    // Header value.

    value_start = colon_pos;

    // Go over the colon.
    if (value_start < len) {
        value_start++;
    }

    // Ignore LWS before field-content.
    while ((value_start < len) && (htp_is_lws(data[value_start]))) {
        value_start++;
    }

    // Look for the end of field-content.
    value_end = value_start;
    while ((value_end < len) && (data[value_end] != '\0')) value_end++;

    // Ignore LWS after field-content.
    prev = value_end - 1;
    while ((prev > value_start) && (htp_is_lws(data[prev]))) {
        prev--;
        value_end--;
    }

    // Check that the header name is a token.
    size_t i = name_start;
    while (i < name_end) {
        if (!htp_is_token(data[i])) {
            // Incorrectly formed header name.
            
            h->flags |= HTP_FIELD_INVALID;

            // Log only once per transaction.
            if (!(connp->in_tx->flags & HTP_FIELD_INVALID)) {
                connp->in_tx->flags |= HTP_FIELD_INVALID;
                htp_log(connp, HTP_LOG_MARK, HTP_LOG_WARNING, 0, "Request header name is not a token");
            }

            break;
        }

        i++;
    }

    // Now extract the name and the value
    h->name = bstr_dup_mem(data + name_start, name_end - name_start);
    if (h->name == NULL) return HTP_ERROR;

    h->value = bstr_dup_mem(data + value_start, value_end - value_start);
    if (h->value == NULL) {
        bstr_free(h->name);
        return HTP_ERROR;
    }

    return HTP_OK;
}

/**
 * Parse request line as Apache 2.2 does.
 *
 * @param[in] connp
 * @return HTP_OK or HTP_ERROR
 */
htp_status_t htp_parse_request_line_apache_2_2(htp_connp_t *connp) {
    return htp_parse_request_line_generic_ex(connp, 1 /* NUL terminates line */);
}