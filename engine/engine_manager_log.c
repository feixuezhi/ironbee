/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee --- Engine Manager Logging
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include "engine_manager_private.h"

#include <ironbee/config.h>
#include <ironbee/context.h>
#include <ironbee/engine.h>
#include <ironbee/engine_manager.h>
#include <ironbee/list.h>
#include <ironbee/log.h>
#include <ironbee/mpool.h>
#include <ironbee/server.h>

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Logging buffer sizes */
static const size_t fmt_size_default = 256;   /**< Size of default format buf */
static const size_t fmt_pad_size = 128;       /**< Size of format padding */
static const size_t log_buf_size = 16 * 1024; /**< Size of log buffer */

ib_status_t manager_logger_open(ib_logger_t *logger, void *data) {
    return IB_OK;
}

ib_status_t manager_logger_close(ib_logger_t *logger, void *data) {
    ib_manager_t *manager = (ib_manager_t *)data;

    manager->log_flush_fn(manager->log_flush_cbdata);
    return IB_OK;
}

ib_status_t manager_logger_reopen(ib_logger_t *logger, void *data) {
    ib_manager_t *manager = (ib_manager_t *)data;

    manager->log_flush_fn(manager->log_flush_cbdata);
    return IB_OK;
}

ib_status_t manager_logger_format(
    ib_logger_t           *logger, 
    const ib_logger_rec_t *rec,
    const uint8_t         *msg,
    const size_t           msg_sz,
    void                  *writer_record,
    void                  *data
)
{
    assert(data != NULL);

    ib_manager_t               *manager = (ib_manager_t *)data;
    ib_engine_t                *ib = NULL;
    ib_status_t                 rc;
    ib_log_level_t              logger_level;
    char                        fmt_buf_default[fmt_size_default+1];
    char                       *fmt_buf = NULL;
    char                       *fmt_free = NULL;
    size_t                      fmt_buf_size = 0;
    size_t                      fmt_required;
    char                       *log_buf = NULL;
    ib_manager_logger_record_t *manager_logger_record;

    rc = ib_manager_engine_acquire(manager, &ib);
    if (rc == IB_ENOENT) {
        logger_level = ib_logger_level_get(manager->logger);
    } else if (rc == IB_OK ) {
        logger_level = ib_logger_level_get(ib_engine_logger_get(ib));
    }
    else {
        goto cleanup;
    }

    /* Add padding bytes to required size */
    fmt_required = msg_sz + fmt_pad_size;
    if (fmt_required > fmt_size_default) {
        fmt_buf = (char *)malloc(fmt_required+1);
        if (fmt_buf == NULL) {
            goto cleanup;
        }
        fmt_free = fmt_buf;
    }
    else {
        fmt_buf = fmt_buf_default;
    }
    fmt_buf_size = fmt_required;
    snprintf(
        fmt_buf,
        fmt_buf_size,
        "%-10s- ",
        ib_log_level_to_string(rec->level));

    /* If this is a transaction, add the TX id */
    if ( rec->tx != NULL ) {
        const ib_tx_t *tx = rec->tx;
        static const size_t line_info_size = 64;
        char                line_info[line_info_size];

        strcpy(line_info, "[tx:");
        strcat(line_info, tx->id);
        strcat(line_info, "] ");
        strcat(fmt_buf, line_info);
    }

    /* Add the file name and line number if available and log level >= DEBUG */
    if ( (rec->file != NULL) && (rec->line_number > 0) && (logger_level >= IB_LOG_DEBUG)) {
        size_t              flen;
        static const size_t line_info_size = 35;
        char                line_info[line_info_size];
        const char         *file = rec->file;

        while ( (file != NULL) && (strncmp(file, "../", 3) == 0) ) {
            file += 3;
        }
        flen = strlen(file);
        if (flen > 23) {
            file += (flen - 23);
        }

        snprintf(line_info, line_info_size, "(%23s:%-5d) ", file, (int)rec->line_number);
        strcat(fmt_buf, line_info);
    }
    strcat(fmt_buf, (char *)msg);

    manager_logger_record = malloc(sizeof(*manager_logger_record));
    if (manager_logger_record == NULL) {
        rc = IB_EALLOC;
        goto cleanup;
    }

    manager_logger_record->msg    = (uint8_t *)fmt_buf;
    manager_logger_record->msg_sz = strlen(fmt_buf);
    manager_logger_record->level  = rec->level;

    /* Return the formatted log message. */
    *(void**)writer_record = manager_logger_record;

    /* Do not free the buffer on success.
     * The log writing function will do this. */
    fmt_buf = NULL;

cleanup:
    if (fmt_free != NULL) {
        free(fmt_free);
    }
    if (log_buf != NULL) {
        free(log_buf);
    }
    return rc;
}

ib_status_t manager_logger_record(
    ib_logger_t        *logger,
    ib_logger_writer_t *writer,
    void *data
)
{
    ib_status_t rc;

    ib_manager_t *manager = (ib_manager_t *)data;
    ib_manager_logger_record_t *rec;

    for (
        rc = ib_logger_dequeue(logger, writer, &rec);
        rc == IB_OK;
        rc = ib_logger_dequeue(logger, writer, &rec)
    )
    {
        manager->log_buf_fn(rec, manager->log_buf_cbdata);
        free(rec->msg);
        free(rec);
    }

    return IB_OK;
}

void ib_manager_log_flush(
    const ib_manager_t *manager
)
{
    assert(manager != NULL);

    /* If there is a flush function, call it, otherwise do nothing */
    if (manager->log_flush_fn != NULL) {
        manager->log_flush_fn(manager->log_flush_cbdata);
    }
}

void DLL_LOCAL ib_manager_log_ex(
    ib_manager_t       *manager,
    ib_log_level_t      level,
    const char         *file,
    const char         *func,
    int                 line,
    ib_log_call_data_t *calldata,
    const char         *fmt,
    ...
)
{
    assert(manager != NULL);
    assert(manager->logger != NULL);

    va_list      ap;

    if (manager->log_buf_fn != NULL) {
        const size_t               msg_sz_mx = 1024;
        ib_manager_logger_record_t rec = {
            .level  = level,
            .msg    = NULL,
            .msg_sz = 0
        };

        rec.msg = malloc(msg_sz_mx);
        if (rec.msg == NULL) {
            return;
        }

        va_start(ap, fmt);
        rec.msg_sz = vsnprintf((char *)rec.msg, msg_sz_mx, fmt, ap);
        va_end(ap);

        manager->log_buf_fn(&rec, manager->log_buf_cbdata);

        if (manager->log_flush_fn != NULL) {
            manager->log_flush_fn(manager->log_flush_cbdata);
        }

        free(rec.msg);
    }

}

