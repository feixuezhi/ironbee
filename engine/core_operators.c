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
 * @brief IronBee
 *
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include "core_private.h"

#include <ironbee/bytestr.h>
#include <ironbee/debug.h>
#include <ironbee/engine.h>
#include <ironbee/escape.h>
#include <ironbee/field.h>
#include <ironbee/ipset.h>
#include <ironbee/mpool.h>
#include <ironbee/operator.h>
#include <ironbee/rule_engine.h>
#include <ironbee/string.h>
#include <ironbee/util.h>

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>

/**
 * Allocate a buffer and unescape operator arguments.
 * @param[in] ib IronBee engine used for logging.
 * @param[in] mp Memory pool that @a str_unesc will be allocated out of.
 * @param[in] str The parameter string to be unescaped.
 * @param[out] str_unesc On a successful unescaping, a new buffer allocated
 *             out of @a mp will be assigned to @a *str_unesc with the
 *             unescaped string in it.
 * @param[out] str_unesc_len On success *str_unesc_len is assigned the length
 *             of the unescaped string. Note that this may be different
 *             that strlen(*str_unesc) because \\x00 will place a NULL
 *             in the middle of @a *str_unesc.
 *             This string should be wrapped in an ib_bytestr_t.
 * @returns IB_OK on success. IB_EALLOC on error. IB_EINVAL if @a str
 *          was unable to be unescaped.
 */
static ib_status_t unescape_op_args(ib_engine_t *ib,
                                    ib_mpool_t *mp,
                                    char **str_unesc,
                                    size_t *str_unesc_len,
                                    const char *str)
{
    IB_FTRACE_INIT();

    assert(mp!=NULL);
    assert(ib!=NULL);
    assert(str!=NULL);
    assert(str_unesc!=NULL);
    assert(str_unesc_len!=NULL);

    ib_status_t rc;
    const size_t str_len = strlen(str);

    /* Temporary unescaped string holder. */
    char* tmp_unesc = ib_mpool_alloc(mp, str_len+1);
    size_t tmp_unesc_len;

    if ( tmp_unesc == NULL ) {
        ib_log_debug(ib, "Failed to allocate unescape string buffer.");
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    rc = ib_util_unescape_string(tmp_unesc,
                                 &tmp_unesc_len,
                                 str,
                                 str_len,
                                 IB_UTIL_UNESCAPE_NULTERMINATE);

    if ( rc != IB_OK ) {
        ib_log_debug(ib, "Failed to unescape string: %s", str);
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Commit changes on success. */
    *str_unesc = tmp_unesc;
    *str_unesc_len = tmp_unesc_len;

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Create function for the "str" family of operators
 *
 * @param[in] ib The IronBee engine (unused)
 * @param[in] ctx The current IronBee context (unused)
 * @param[in] rule Parent rule to the operator
 * @param[in,out] mp Memory pool to use for allocation
 * @param[in] parameters Constant parameters
 * @param[in,out] op_inst Instance operator
 *
 * @returns Status code
 */
static ib_status_t strop_create(ib_engine_t *ib,
                                ib_context_t *ctx,
                                const ib_rule_t *rule,
                                ib_mpool_t *mp,
                                const char *parameters,
                                ib_operator_inst_t *op_inst)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    bool expand;
    char *str;
    size_t str_len;

    if (parameters == NULL) {
        ib_log_error(ib, "Missing parameter for operator %s",
                     op_inst->op->name);
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    rc = unescape_op_args(ib, mp, &str, &str_len, parameters);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    rc = ib_data_expand_test_str(str, &expand);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }
    if (expand) {
        op_inst->flags |= IB_OPINST_FLAG_EXPAND;
    }

    op_inst->data = str;
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the "streq" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data C-style string to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_streq_execute(const ib_rule_exec_t *rule_exec,
                                    void *data,
                                    ib_flags_t flags,
                                    ib_field_t *field,
                                    ib_num_t *result)
{
    IB_FTRACE_INIT();
    assert(rule_exec != NULL);
    assert(data != NULL);
    assert(field != NULL);
    assert(result != NULL);

    /**
     * This works on C-style (NUL terminated) and byte strings.  Note
     * that data is assumed to be a NUL terminated string (because our
     * configuration parser can't produce anything else).
     **/
    ib_status_t  rc;
    const char  *cstr = (const char *)data;
    char        *expanded;
    ib_tx_t     *tx = rule_exec->tx;

    /* Expand the string */
    if ( (tx != NULL) && ( (flags & IB_OPINST_FLAG_EXPAND) != 0) ) {
        rc = ib_data_expand_str(tx->dpi, cstr, false, &expanded);
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }
    }
    else {
        expanded = (char *)cstr;
    }

    /* Handle NUL-terminated strings and byte strings */
    if (field->type == IB_FTYPE_NULSTR) {
        const char *fval;
        rc = ib_field_value(field, ib_ftype_nulstr_out(&fval));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        *result = (strcmp(fval, expanded) == 0);
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        const ib_bytestr_t *value;
        size_t                len;

        rc = ib_field_value(field, ib_ftype_bytestr_out(&value));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        len = ib_bytestr_length(value);

        if (len == strlen(expanded)) {
            *result = (
                memcmp(ib_bytestr_const_ptr(value), expanded, len) == 0
            );
        }
        else {
            *result = 0;
        }
    }
    else {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        ib_data_capture_set_item(rule_exec->tx, 0, field);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the "contains" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data C-style string to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_contains_execute(const ib_rule_exec_t *rule_exec,
                                       void *data,
                                       ib_flags_t flags,
                                       ib_field_t *field,
                                       ib_num_t *result)
{
    IB_FTRACE_INIT();
    assert(rule_exec != NULL);
    assert(data != NULL);
    assert(field != NULL);
    assert(result != NULL);

    ib_status_t  rc = IB_OK;
    const char  *cstr = (char *)data;
    char        *expanded;
    ib_tx_t     *tx = rule_exec->tx;

    /* Expand the string */
    if ( (tx != NULL) && ( (flags & IB_OPINST_FLAG_EXPAND) != 0) ) {
        rc = ib_data_expand_str(tx->dpi, cstr, false, &expanded);
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }
    }
    else {
        expanded = (char *)cstr;
    }

    /**
     * This works on C-style (NUL terminated) and byte strings.  Note
     * that data is assumed to be a NUL terminated string (because our
     * configuration parser can't produce anything else).
     **/
    if (field->type == IB_FTYPE_NULSTR) {
        const char *s;
        rc = ib_field_value(field, ib_ftype_nulstr_out(&s));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        if (strstr(s, expanded) == NULL) {
            *result = 0;
        }
        else {
            *result = 1;
        }
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        const ib_bytestr_t *str;
        rc = ib_field_value(field, ib_ftype_bytestr_out(&str));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        if (ib_bytestr_index_of_c(str, expanded) == -1) {
            *result = 0;
        }
        else {
            *result = 1;
        }
    }
    else {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    if ( (tx != NULL) && (ib_rule_should_capture(rule_exec, *result)) ) {
        ib_field_t *f;
        const char *name;

        ib_data_capture_clear(rule_exec->tx);

        name = ib_data_capture_name(0);
        rc = ib_field_create_bytestr_alias(&f, rule_exec->tx->mp,
                                           name, strlen(name),
                                           (uint8_t *)expanded,
                                           strlen(expanded));
        ib_data_capture_set_item(rule_exec->tx, 0, f);
    }

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Create function for the "ipmatch" operator
 *
 * @param[in] ib         The IronBee engine.
 * @param[in] ctx        The current IronBee context (unused).
 * @param[in] rule       Parent rule to the operator.
 * @param[in] mp         Memory pool to use for allocation.
 * @param[in] parameters Parameters (IPv4 address or networks)
 * @param[in] op_inst    Instance operator.
 *
 * @returns
 * - IB_OK if no failure.
 * - IB_EALLOC on allocation failure.
 * - IB_EINVAL on unable to parse @a parameters as IP addresses or networks.
 */
static
ib_status_t op_ipmatch_create(
    ib_engine_t        *ib,
    ib_context_t       *ctx,
    const ib_rule_t    *rule,
    ib_mpool_t         *mp,
    const char         *parameters,
    ib_operator_inst_t *op_inst
)
{
    IB_FTRACE_INIT();

    assert(ib      != NULL);
    assert(ctx     != NULL);
    assert(rule    != NULL);
    assert(mp      != NULL);
    assert(op_inst != NULL);

    ib_status_t        rc             = IB_OK;
    char              *copy           = NULL;
    size_t             copy_len       = 0;
    char              *p              = NULL;
    size_t             num_parameters = 0;
    ib_ipset4_entry_t *entries        = NULL;
    size_t             i              = 0;
    ib_ipset4_t       *ipset          = NULL;

    if (parameters == NULL) {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Make a copy of the parameters to operate on. */
    rc = unescape_op_args(ib, mp, &copy, &copy_len, parameters);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error unescaping rule parameters '%s'", parameters
        );
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    ipset = ib_mpool_alloc(mp, sizeof(*ipset));
    if (ipset == NULL) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Count the number of parameters. */
    for (p = copy; *p != '\0';) {
        while (*p == ' ') {++p;}
        if (*p != '\0') {
            ++num_parameters;
            while (*p && *p != ' ') {++p;}
        }
    }

    entries = ib_mpool_alloc(mp, num_parameters * sizeof(*entries));
    if (entries == NULL) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Fill entries. */
    i = 0;
    for (p = strtok(copy, " ");  p != NULL;  p = strtok(NULL, " ") ) {
        assert(i < num_parameters);
        entries[i].data = NULL;
        rc = ib_ip4_str_to_net(p, &entries[i].network);
        if (rc == IB_EINVAL) {
            rc = ib_ip4_str_to_ip(p, &(entries[i].network.ip));
            if (rc == IB_OK) {
                entries[i].network.size = 32;
            }
        }
        if (rc != IB_OK) {
            ib_log_error(ib, "Error parsing: %s", p);
            IB_FTRACE_RET_STATUS(rc);
        }

        ++i;
    }
    assert(i == num_parameters);

    rc = ib_ipset4_init(
        ipset,
        NULL, 0,
        entries, num_parameters
    );
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error initializing internal data: %s",
            ib_status_to_string(rc)
        );
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Done */
    op_inst->data = ipset;

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the "ipmatch" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data      IP Set data.
 * @param[in] flags     Operator instance flags.
 * @param[in] field     Field value.
 * @param[out] result   Pointer to number in which to store the result.
 *
 * @returns
 * - IB_OK if no failure, regardless of match status.
 * - IB_EALLOC on allocation failure.
 * - IB_EINVAL on unable to parse @a field as IP address.
 */
static
ib_status_t op_ipmatch_execute(
    const ib_rule_exec_t *rule_exec,
    void                 *data,
    ib_flags_t            flags,
    ib_field_t           *field,
    ib_num_t             *result
)
{
    IB_FTRACE_INIT();
    assert(rule_exec != NULL);
    assert(data      != NULL);
    assert(field     != NULL);
    assert(result    != NULL);

    ib_status_t        rc               = IB_OK;
    const ib_ipset4_t *ipset            = NULL;
    ib_ip4_t           ip               = 0;
    const char        *ipstr            = NULL;
    char               ipstr_buffer[17] = "\0";
    ib_tx_t           *tx               = rule_exec->tx;

    ipset = (const ib_ipset4_t *)data;

    if (field->type == IB_FTYPE_NULSTR) {
        rc = ib_field_value(field, ib_ftype_nulstr_out(&ipstr));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        if (ipstr == NULL) {
            ib_log_error_tx(tx, "Failed to get NULSTR from field");
            IB_FTRACE_RET_STATUS(IB_EUNKNOWN);
        }
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        const ib_bytestr_t *bs;
        rc = ib_field_value(field, ib_ftype_bytestr_out(&bs));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        assert(bs != NULL);
        assert(ib_bytestr_length(bs) < 17);

        strncpy(
            ipstr_buffer,
            (const char *)ib_bytestr_const_ptr(bs),
            ib_bytestr_length(bs)
        );
        ipstr_buffer[ib_bytestr_length(bs)] = '\0';
        ipstr = ipstr_buffer;
    }
    else {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    rc = ib_ip4_str_to_ip(ipstr, &ip);
    if (rc != IB_OK) {
        ib_log_info_tx(tx, "Could not parse as IP: %s", ipstr);
        IB_FTRACE_RET_STATUS(rc);
    }

    rc = ib_ipset4_query(ipset, ip, NULL, NULL, NULL);
    if (rc == IB_ENOENT) {
        *result = 0;
    }
    else if (rc == IB_OK) {
        *result = 1;
        if (ib_rule_should_capture(rule_exec, *result)) {
            ib_data_capture_clear(rule_exec->tx);
            ib_data_capture_set_item(rule_exec->tx, 0, field);
        }
    }
    else {
        ib_rule_log_error(rule_exec,
                          "Error searching set for ip %s: %s",
                          ipstr, ib_status_to_string(rc)
        );
        IB_FTRACE_RET_STATUS(rc);
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}


/**
 * Create function for the "ipmatch6" operator
 *
 * @param[in] ib         The IronBee engine.
 * @param[in] ctx        The current IronBee context (unused).
 * @param[in] rule       Parent rule to the operator.
 * @param[in] mp         Memory pool to use for allocation.
 * @param[in] parameters Parameters (IPv6 address or networks)
 * @param[in] op_inst    Instance operator.
 *
 * @returns
 * - IB_OK if no failure.
 * - IB_EALLOC on allocation failure.
 * - IB_EINVAL on unable to parse @a parameters as IP addresses or networks.
 */
static
ib_status_t op_ipmatch6_create(
    ib_engine_t        *ib,
    ib_context_t       *ctx,
    const ib_rule_t    *rule,
    ib_mpool_t         *mp,
    const char         *parameters,
    ib_operator_inst_t *op_inst
)
{
    IB_FTRACE_INIT();

    assert(ib      != NULL);
    assert(ctx     != NULL);
    assert(rule    != NULL);
    assert(mp      != NULL);
    assert(op_inst != NULL);

    ib_status_t        rc             = IB_OK;
    char              *copy           = NULL;
    size_t             copy_len       = 0;
    char              *p              = NULL;
    size_t             num_parameters = 0;
    ib_ipset6_entry_t *entries        = NULL;
    size_t             i              = 0;
    ib_ipset6_t       *ipset          = NULL;

    if (parameters == NULL) {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Make a copy of the parameters to operate on. */
    rc = unescape_op_args(ib, mp, &copy, &copy_len, parameters);
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error unescaping rule parameters '%s'", parameters
        );
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    ipset = ib_mpool_alloc(mp, sizeof(*ipset));
    if (ipset == NULL) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Count the number of parameters. */
    for (p = copy; *p != '\0';) {
        while (*p == ' ') {++p;}
        if (*p != '\0') {
            ++num_parameters;
            while (*p && *p != ' ') {++p;}
        }
    }

    entries = ib_mpool_alloc(mp, num_parameters * sizeof(*entries));
    if (entries == NULL) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Fill entries. */
    i = 0;
    for (p = strtok(copy, " ");  p != NULL;  p = strtok(NULL, " ") ) {
        assert(i < num_parameters);
        entries[i].data = NULL;
        rc = ib_ip6_str_to_net(p, &entries[i].network);
        if (rc == IB_EINVAL) {
            rc = ib_ip6_str_to_ip(p, &(entries[i].network.ip));
            if (rc == IB_OK) {
                entries[i].network.size = 128;
            }
        }
        if (rc != IB_OK) {
            ib_log_error(ib, "Error parsing: %s", p);
            IB_FTRACE_RET_STATUS(rc);
        }

        ++i;
    }
    assert(i == num_parameters);

    rc = ib_ipset6_init(
        ipset,
        NULL, 0,
        entries, num_parameters
    );
    if (rc != IB_OK) {
        ib_log_error(ib,
            "Error initializing internal data: %s",
            ib_status_to_string(rc)
        );
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Done */
    op_inst->data = ipset;

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the "ipmatch6" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data      IP Set data.
 * @param[in] flags     Operator instance flags.
 * @param[in] field     Field value.
 * @param[out] result   Pointer to number in which to store the result.
 *
 * @returns
 * - IB_OK if no failure, regardless of match status.
 * - IB_EALLOC on allocation failure.
 * - IB_EINVAL on unable to parse @a field as IP address.
 */
static
ib_status_t op_ipmatch6_execute(
    const ib_rule_exec_t *rule_exec,
    void                 *data,
    ib_flags_t            flags,
    ib_field_t           *field,
    ib_num_t             *result
)
{
    IB_FTRACE_INIT();
    assert(rule_exec != NULL);
    assert(data      != NULL);
    assert(field     != NULL);
    assert(result    != NULL);

    ib_status_t        rc               = IB_OK;
    const ib_ipset6_t *ipset            = NULL;
    ib_ip6_t           ip               = {{0, 0, 0, 0}};
    const char        *ipstr            = NULL;
    char               ipstr_buffer[41] = "\0";
    ib_tx_t           *tx               = rule_exec->tx;

    ipset = (const ib_ipset6_t *)data;

    if (field->type == IB_FTYPE_NULSTR) {
        rc = ib_field_value(field, ib_ftype_nulstr_out(&ipstr));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        if (ipstr == NULL) {
            ib_log_error_tx(tx, "Failed to get NULSTR from field");
            IB_FTRACE_RET_STATUS(IB_EUNKNOWN);
        }
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        const ib_bytestr_t *bs;
        rc = ib_field_value(field, ib_ftype_bytestr_out(&bs));
        if (rc != IB_OK) {
            IB_FTRACE_RET_STATUS(rc);
        }

        assert(bs != NULL);
        assert(ib_bytestr_length(bs) < 41);

        strncpy(
            ipstr_buffer,
            (const char *)ib_bytestr_const_ptr(bs),
            ib_bytestr_length(bs)
        );
        ipstr_buffer[ib_bytestr_length(bs)] = '\0';
        ipstr = ipstr_buffer;
    }
    else {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    rc = ib_ip6_str_to_ip(ipstr, &ip);
    if (rc != IB_OK) {
        ib_log_info_tx(tx, "Could not parse as IP: %s", ipstr);
        IB_FTRACE_RET_STATUS(rc);
    }

    rc = ib_ipset6_query(ipset, ip, NULL, NULL, NULL);
    if (rc == IB_ENOENT) {
        *result = 0;
    }
    else if (rc == IB_OK) {
        *result = 1;
        if (ib_rule_should_capture(rule_exec, *result)) {
            ib_data_capture_clear(tx);
            ib_data_capture_set_item(tx, 0, field);
        }
    }
    else {
        ib_rule_log_error(rule_exec,
                          "Error searching set for ip %s: %s",
                          ipstr, ib_status_to_string(rc)
        );
        IB_FTRACE_RET_STATUS(rc);
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Create function for the numeric comparison operators
 *
 * @param[in] ib The IronBee engine (unused)
 * @param[in] ctx The current IronBee context (unused)
 * @param[in] rule Parent rule to the operator
 * @param[in,out] mp Memory pool to use for allocation
 * @param[in] params Constant parameters
 * @param[in,out] op_inst Instance operator
 *
 * @returns Status code
 */
static ib_status_t op_numcmp_create(ib_engine_t *ib,
                                    ib_context_t *ctx,
                                    const ib_rule_t *rule,
                                    ib_mpool_t *mp,
                                    const char *params,
                                    ib_operator_inst_t *op_inst)
{
    IB_FTRACE_INIT();
    ib_field_t *f;
    ib_status_t rc;
    bool expandable;
    ib_num_t value;

    char *params_unesc;
    size_t params_unesc_len;

    rc = unescape_op_args(ib, mp, &params_unesc, &params_unesc_len, params);
    if (rc != IB_OK) {
        ib_log_debug(ib, "Unable to unescape parameter: %s", params);
        IB_FTRACE_RET_STATUS(rc);
    }

    if (params_unesc == NULL) {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    /* Is the string expandable? */
    rc = ib_data_expand_test_str(params_unesc, &expandable);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }
    if (expandable) {
        op_inst->flags |= IB_OPINST_FLAG_EXPAND;
    }
    else {
        rc = ib_string_to_num_ex(params_unesc, params_unesc_len, 0, &value);
        if (rc != IB_OK) {
            ib_log_error(ib, "Parameter \"%s\" for operator %s "
                         "is not a valid number",
                         params_unesc, op_inst->op->name);
            IB_FTRACE_RET_STATUS(rc);
        }
    }

    if (expandable) {
        rc = ib_field_create(&f, mp, IB_FIELD_NAME("param"),
                             IB_FTYPE_NULSTR, ib_ftype_nulstr_in(params_unesc));
    }
    else {
        rc = ib_field_create(&f, mp, IB_FIELD_NAME("param"),
                             IB_FTYPE_NUM, ib_ftype_num_in(&value));
    }

    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    op_inst->data = f;
    op_inst->fparam = f;
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Get expanded numeric value of a string
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] field Operator instance field
 * @param[in] flags Operator instance flags
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t get_num_value(const ib_rule_exec_t *rule_exec,
                                 const ib_field_t *field,
                                 ib_flags_t flags,
                                 ib_num_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    const char *original;
    char *expanded;

    /* Easy case: just return the number from the pdata structure */
    if ( (flags & IB_OPINST_FLAG_EXPAND) == 0) {
        rc = ib_field_value(field, ib_ftype_num_out(result));
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Get the string from the field */
    rc = ib_field_value(field, ib_ftype_nulstr_out(&original));
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Expand the string */
    rc = ib_data_expand_str(rule_exec->tx->dpi, original, false, &expanded);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Convert string the expanded string to a number */
    rc = ib_string_to_num(expanded, 0, result);
    if (rc != IB_OK) {
        ib_rule_log_error(rule_exec,
                          "Failed to convert expanded parameter \"%s\" "
                          "to a number: %s",
                          expanded, ib_status_to_string(rc));
    }
    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Get integer representation of a field
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t field_to_num(const ib_rule_exec_t *rule_exec,
                                ib_field_t *field,
                                ib_num_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t rc;

    switch (field->type) {
        case IB_FTYPE_NUM:
            rc = ib_field_value(field, ib_ftype_num_out(result));
            if (rc != IB_OK) {
                IB_FTRACE_RET_STATUS(rc);
            }
            break;

        case IB_FTYPE_UNUM :
            {
                ib_unum_t n;
                rc = ib_field_value(field, ib_ftype_unum_out(&n));
                if (rc != IB_OK) {
                    IB_FTRACE_RET_STATUS(rc);
                }

                if (n > INT64_MAX) {
                    ib_rule_log_error(rule_exec,
                                      "Overflow in converting number %"PRIu64,
                                      n);
                    IB_FTRACE_RET_STATUS(IB_EINVAL);
                }

                *result = (ib_num_t)n;
                break;
            }
        case IB_FTYPE_NULSTR :
            {
                const char *fval;
                rc = ib_field_value(field, ib_ftype_nulstr_out(&fval));
                if (rc != IB_OK) {
                    IB_FTRACE_RET_STATUS(rc);
                }

                rc = ib_string_to_num(fval, 0, result);
                if (rc != IB_OK) {
                    ib_rule_log_error(rule_exec,
                                      "Failed to convert string \"%s\" "
                                      "to a number: %s",
                                      fval, ib_status_to_string(rc));
                    IB_FTRACE_RET_STATUS(IB_EINVAL);
                }
            }
            break;

        case IB_FTYPE_BYTESTR:
            {
                const ib_bytestr_t *bs;
                rc = ib_field_value(field, ib_ftype_bytestr_out(&bs));
                if (rc != IB_OK) {
                    IB_FTRACE_RET_STATUS(rc);
                }

                rc = ib_string_to_num_ex(
                    (const char *)ib_bytestr_const_ptr(bs),
                    ib_bytestr_length(bs),
                    0,
                    result);
                if (rc != IB_OK) {
                    ib_rule_log_error(rule_exec,
                                      "Failed to convert byte string \"%.*s\" "
                                      "to a number: %s",
                                      (int)ib_bytestr_length(bs),
                                      (const char *)ib_bytestr_const_ptr(bs),
                                      ib_status_to_string(rc));
                    IB_FTRACE_RET_STATUS(IB_EINVAL);
                }
            }
            break;

        default:
            ib_rule_log_error(rule_exec,
                              "Unable to convert field type %d to a number",
                              field->type);
            IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Store a number in the capture buffer
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] capture The capture number
 * @param[in] value The actual value
 */
static ib_status_t capture_num(const ib_rule_exec_t *rule_exec,
                               int capture,
                               ib_num_t value)
{
    IB_FTRACE_INIT();
    assert(rule_exec != NULL);

    ib_status_t rc;
    ib_field_t *field;
    const char *name;
    const char *str;

    name = ib_data_capture_name(capture);

    str = ib_num_to_string(rule_exec->tx->mp, value);
    if (str == NULL) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }
    rc = ib_field_create_bytestr_alias(&field, rule_exec->tx->mp,
                                       name, strlen(name),
                                       (uint8_t *)str, strlen(str));
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }
    rc = ib_data_capture_set_item(rule_exec->tx, 0, field);
    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Execute function for the numeric "equal" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data Pointer to number to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_eq_execute(const ib_rule_exec_t *rule_exec,
                                 void *data,
                                 ib_flags_t flags,
                                 ib_field_t *field,
                                 ib_num_t *result)
{
    IB_FTRACE_INIT();
    const ib_field_t *pdata = (const ib_field_t *)data;
    ib_num_t          param_value;  /* Parameter value */
    ib_num_t          value;
    ib_status_t       rc;

    /* Get integer representation of the field */
    rc = field_to_num(rule_exec, field, &value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Get the numeric value from the param data (including expansion, etc) */
    rc = get_num_value(rule_exec, pdata, flags, &param_value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Do the comparison */
    *result = (value == param_value);
    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        capture_num(rule_exec, 0, value);
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the numeric "not equal" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data C-style string to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_ne_execute(const ib_rule_exec_t *rule_exec,
                                 void *data,
                                 ib_flags_t flags,
                                 ib_field_t *field,
                                 ib_num_t *result)
{
    IB_FTRACE_INIT();
    const ib_field_t *pdata = (const ib_field_t *)data;
    ib_num_t          param_value;  /* Parameter value */
    ib_num_t          value;
    ib_status_t       rc;

    /* Get integer representation of the field */
    rc = field_to_num(rule_exec, field, &value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Get the numeric value (including expansion, etc) */
    rc = get_num_value(rule_exec, pdata, flags, &param_value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Do the comparison */
    *result = (value != param_value);
    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        rc = capture_num(rule_exec, 0, value);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Error storing capture #0: %s",
                              ib_status_to_string(rc));
        }
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the "gt" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data Pointer to number to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_gt_execute(const ib_rule_exec_t *rule_exec,
                                 void *data,
                                 ib_flags_t flags,
                                 ib_field_t *field,
                                 ib_num_t *result)
{
    IB_FTRACE_INIT();
    const ib_field_t *pdata = (const ib_field_t *)data;
    ib_num_t          param_value;  /* Parameter value */
    ib_num_t          value;
    ib_status_t       rc;

    /* Get integer representation of the field */
    rc = field_to_num(rule_exec, field, &value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Get the numeric value (including expansion, etc) */
    rc = get_num_value(rule_exec, pdata, flags, &param_value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Do the comparison */
    *result = (value > param_value);
    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        rc = capture_num(rule_exec, 0, value);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Error storing capture #0: %s",
                              ib_status_to_string(rc));
        }
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the numeric "less-than" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data C-style string to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_lt_execute(const ib_rule_exec_t *rule_exec,
                                 void *data,
                                 ib_flags_t flags,
                                 ib_field_t *field,
                                 ib_num_t *result)
{
    IB_FTRACE_INIT();
    const ib_field_t *pdata = (const ib_field_t *)data;
    ib_num_t          param_value;  /* Parameter value */
    ib_num_t          value;
    ib_status_t       rc;

    /* Get integer representation of the field */
    rc = field_to_num(rule_exec, field, &value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Get the numeric value (including expansion, etc) */
    rc = get_num_value(rule_exec, pdata, flags, &param_value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Do the comparison */
    *result = (value < param_value);

    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        rc = capture_num(rule_exec, 0, value);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Error storing capture #0: %s",
                              ib_status_to_string(rc));
        }
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the numeric "greater than or equal to" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data Pointer to number to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_ge_execute(const ib_rule_exec_t *rule_exec,
                                 void *data,
                                 ib_flags_t flags,
                                 ib_field_t *field,
                                 ib_num_t *result)
{
    IB_FTRACE_INIT();
    const ib_field_t *pdata = (const ib_field_t *)data;
    ib_num_t          param_value;  /* Parameter value */
    ib_num_t          value;
    ib_status_t       rc;

    /* Get integer representation of the field */
    rc = field_to_num(rule_exec, field, &value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Expand the data value? */
    rc = get_num_value(rule_exec, pdata, flags, &param_value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Do the comparison */
    *result = (value >= param_value);
    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        rc = capture_num(rule_exec, 0, value);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Error storing capture #0: %s",
                              ib_status_to_string(rc));
        }
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the "less than or equal to" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data Pointer to number to compare to
 * @param[in] flags Operator instance flags
 * @param[in] field Field value
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code
 */
static ib_status_t op_le_execute(const ib_rule_exec_t *rule_exec,
                                 void *data,
                                 ib_flags_t flags,
                                 ib_field_t *field,
                                 ib_num_t *result)
{
    IB_FTRACE_INIT();
    const ib_field_t *pdata = (const ib_field_t *)data;
    ib_num_t          param_value;  /* Parameter value */
    ib_num_t          value;
    ib_status_t       rc;

    /* Get integer representation of the field */
    rc = field_to_num(rule_exec, field, &value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Expand the data value? */
    rc = get_num_value(rule_exec, pdata, flags, &param_value);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Do the comparison */
    *result = (value <= param_value);
    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        rc = capture_num(rule_exec, 0, value);
        if (rc != IB_OK) {
            ib_rule_log_error(rule_exec, "Error storing capture #0: %s",
                              ib_status_to_string(rc));
        }
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Execute function for the "nop" operator
 *
 * @param[in] rule_exec Rule execution object
 * @param[in] data Operator data (unused)
 * @param[in] flags Operator instance flags
 * @param[in] field Field value (unused)
 * @param[out] result Pointer to number in which to store the result
 *
 * @returns Status code (IB_OK)
 */
static ib_status_t op_nop_execute(const ib_rule_exec_t *rule_exec,
                                  void *data,
                                  ib_flags_t flags,
                                  ib_field_t *field,
                                  ib_num_t *result)
{
    IB_FTRACE_INIT();
    *result = 1;

    if (ib_rule_should_capture(rule_exec, *result)) {
        ib_data_capture_clear(rule_exec->tx);
        ib_data_capture_set_item(rule_exec->tx, 0, field);
    }
    IB_FTRACE_RET_STATUS(IB_OK);
}

/**
 * Initialize the core operators
 **/
ib_status_t ib_core_operators_init(ib_engine_t *ib, ib_module_t *mod)
{
    IB_FTRACE_INIT();
    ib_status_t rc;


    /**
     * String comparison operators
     */

    /* Register the string equal operator */
    rc = ib_operator_register(ib,
                              "streq",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              strop_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_streq_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the string contains operator */
    rc = ib_operator_register(ib,
                              "contains",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              strop_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_contains_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the ipmatch operator */
    rc = ib_operator_register(ib,
                              "ipmatch",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_ipmatch_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_ipmatch_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the ipmatch6 operator */
    rc = ib_operator_register(ib,
                              "ipmatch6",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_ipmatch6_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_ipmatch6_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /**
     * Numeric comparison operators
     */

    /* Register the numeric equal operator */
    rc = ib_operator_register(ib,
                              "eq",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_numcmp_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_eq_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the numeric not-equal operator */
    rc = ib_operator_register(ib,
                              "ne",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_numcmp_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_ne_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the numeric greater-than operator */
    rc = ib_operator_register(ib,
                              "gt",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_numcmp_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_gt_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the numeric less-than operator */
    rc = ib_operator_register(ib,
                              "lt",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_numcmp_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_lt_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the numeric greater-than or equal to operator */
    rc = ib_operator_register(ib,
                              "ge",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_numcmp_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_ge_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register the numeric less-than or equal to operator */
    rc = ib_operator_register(ib,
                              "le",
                              IB_OP_FLAG_PHASE | IB_OP_FLAG_CAPTURE,
                              op_numcmp_create,
                              NULL,
                              NULL, /* no destroy function */
                              NULL,
                              op_le_execute,
                              NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }

    /* Register NOP operator */
    rc = ib_operator_register(ib,
                              "nop",
                              ( IB_OP_FLAG_ALLOW_NULL |
                                IB_OP_FLAG_PHASE |
                                IB_OP_FLAG_STREAM |
                                IB_OP_FLAG_CAPTURE ),
                              NULL, NULL, /* No create function */
                              NULL, NULL, /* no destroy function */
                              op_nop_execute, NULL);
    if (rc != IB_OK) {
        IB_FTRACE_RET_STATUS(rc);
    }


    IB_FTRACE_RET_STATUS(IB_OK);
}
