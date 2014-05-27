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
 * @brief IronBee --- Predicate Core Module
 *
 * This module provides the core predicate services that all other predicate
 * modules rely on.  Specifically, it allows for predicate expressions to be
 * registered at configuration time and then queried at per_context.  By
 * having a single module coordinate all such queries, information can be
 * shared across unrelated client modules.
 *
 * Other modules can make use of these services via the public API.  See
 * IBModPredicateCore.
 *
 * *To view the MergeGraph*
 *
 * - Use the `PredicateDebugReport` configuration directive.  Pass in a path
 *   to write the report to or "" for stderr. See
 *   MergeGraph::write_debug_report().
 *
 * *To define a template*
 *
 * - Use the `PredicateDefine` configuration directive.  Pass in a name,
 *   argument list, and body expression.
 **/

#include <predicate/ibmod_predicate_core.hpp>

#include <predicate/bfs.hpp>
#include <predicate/eval.hpp>
#include <predicate/merge_graph.hpp>
#include <predicate/parse.hpp>
#include <predicate/pre_eval_graph.hpp>
#include <predicate/reporter.hpp>
#include <predicate/standard.hpp>
#include <predicate/standard_template.hpp>
#include <predicate/transform_graph.hpp>
#include <predicate/validate_graph.hpp>

#include <ironbeepp/all.hpp>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/shared_ptr.hpp>

#include <algorithm>
#include <fstream>

using namespace std;
using boost::bind;

namespace IB = IronBee;
namespace P  = IB::Predicate;

namespace {

/* A note on indexes:
 *
 * There are three types of indices that show up in this code:
 *
 * - Root: Root indices are provided by the MergeGraph when a new root is
 *   added as part of oracle acquisition.  The root index is reused as the
 *   oracle index (see below).
 * - Oracle: Oracle indices have the same value as the root index.  At oracle
 *   acquisition the oracle index is bound to a function create the oracle.
 *   At close of context, a map of oracle index to root node is constructed.
 *   This combination, allows oracles to be returned immediately, before the
 *   final root node is known.
 * - Node: Node indices are generated at context close and are used to index
 *   the per-transaction graph evaluation state.
 */

// Configuration

//! Name of module.
const char* c_module_name = "predicate_core";

//! Directive to write output a debug report.
const char* c_debug_report_directive = "PredicateDebugReport";

//! Directive to define a template.
const char* c_define_directive = "PredicateDefine";

class Delegate;
class PerTransaction;

/**
 * Per context functionality.
 *
 * At configuration time, this is a MergeGraph.  At context close, the
 * MergeGraph is run through its life cycle, a map of oracle index to root
 * node and a list of roots is generated, and the MergeGraph is released.
 **/
class PerContext
{
public:
    /**
     * Base constructor.
     *
     * This is used to construct the initial PerContext at module load.  All
     * other PerContexts are created via the copy constructor by IronBee++
     * as part of the module configuration data copying.
     *
     * @param[in] delegate Module delegate.
     **/
    explicit
    PerContext(Delegate& delegate);

    /**
     * Copy constructor.
     *
     * This constructor is used to construct additional PerContexts as
     * configuration contexts are opened.  Very importantly, it constructs
     * a *copy* of its parents MergeGraph.  It does not copy runtime members
     * and should never be called after configuration time.
     *
     * @param[in] other PerContext to copy from; in all cases, should be the
     *                  PerContext of the parent context.
     **/
    PerContext(const PerContext& other);

    /**
     * Open the context.
     *
     * This member is called on context open and associated a specific context
     * with this context.  It is assumed that this is called immediately after
     * the copy constructor.
     *
     * @param[in] context Associated context.
     **/
    void open(IB::Context context);

    /**
     * Close the context.
     *
     * This member is called on context close and processes the MergeGraph and
     * converts it into the runtime data members.
     **/
    void close();

    /**
     * Acquire an oracle.
     *
     * Can only be called during configuration, i.e., before close().  Adds
     * @a node to the MergeGraph and returns an oracle for it.
     *
     * @sa IBModPredicateCore::acquire().
     *
     * @param[in] node   Root node of oracle expression.
     * @param[in] origin Origin string, e.g., file and line number.
     * @return Oracle.
     **/
    IBModPredicateCore::oracle_t acquire(
        P::node_p     node,
        const string& origin
    );

    //! Delegate accessor.
    const Delegate& delegate() const;
    //! Delegate accessor.
    Delegate& delegate();

    //! Fetch @ref PerTransaction associated with @a tx.
    PerTransaction& fetch_per_transaction(IB::Transaction tx) const;

    //! Turn debug report on.
    void set_debug_report(const string& to);

    /**
     * Run internal validations.
     *
     * This is automatically run before and after the graph lifecycle.
     *
     * Failures will be logged and then an exception thrown.
     **/
    void assert_valid() const;

private:
    /**
     * Query an oracle.
     *
     * This member is used to construct an oracle by binding this and an
     * oracle index to it.  It forwards to the PerTransaction::query() for the
     * PerTransaction of @a tx.
     *
     * This member is only valid at runtime, i.e., after close().
     *
     * @param[in] oracle_index Index of oracle, i.e., the root index returned
     *                         by the MergeGraph during acquisition.
     * @param[in] tx           Current transaction.  Context of transaction
     *                         must either be the context of this PerContext,
     *                         or that of a child context.
     * @return Result.
     **/
    IBModPredicateCore::result_t query(
        size_t          oracle_index,
        IB::Transaction tx
    ) const;

    //! Pre-evaluate all nodes.
    void pre_evaluate();

    //! Run MergeGraph through lifecycle.
    void graph_lifecycle();

    //! Delegate.
    Delegate& m_delegate;
    //! Context
    IB::Context m_context;

    //! Should we write a debug report?
    bool m_write_debug_report;
    //! Where should we write the debug report?
    string m_debug_report_to;

    //! MergeGraph.  Only valid during configuration, i.e., before close().
    boost::scoped_ptr<P::MergeGraph> m_merge_graph;

    // All following members are only valid at runtime, i.e., after close().

    //! Type of @ref m_oracle_index_to_root_node.
    typedef vector<P::node_cp> oracle_index_to_root_node_t;
    //! Map of oracle index to root node.
    oracle_index_to_root_node_t m_oracle_index_to_root_node;

    //! Type of @ref m_roots.
    typedef vector<P::node_cp> roots_t;
    //! List of all roots.  Used to construct PerTransaction.
    roots_t m_roots;

    //! Index limit.   Used to construct PerTransaction.
    size_t m_index_limit;
};

/**
 * Per transaction functionality.
 *
 * Each transaction has its own graph evaluation state.  The graph evaluation
 * state is initialized the first time the transaction state is requested.
 **/
class PerTransaction
{
public:
    /**
     * Constructor.
     *
     * Initializes graph evaluation state.
     *
     * @param[in] index_limit One more than maximum index of any node.
     * @param[in] roots       List of all roots.
     * @param[in] tx          Transaction this state is for.
     **/
    PerTransaction(
        size_t                    index_limit,
        const vector<P::node_cp>& roots,
        IB::Transaction           tx
    );

    /**
     * Query a root.
     *
     * @param[in] root Root to query.
     * @return Result.
     **/
    IBModPredicateCore::result_t query(
        const P::node_cp& root
    );

private:
    //! Graph evaluation state.
    P::GraphEvalState m_graph_eval_state;
    //! Current transaction.
    IB::Transaction m_tx;
};

/**
 * Module delegate.
 *
 * This class serves four purposes:
 *
 * - On construction it sets up the hooks, configuration data, directives,
 *   etc. it needs.
 * - It forwards these hooks to the appropriate PerContext.
 * - It holds the call factory.
 * - It handles template definition.
 **/
class Delegate :
    public IB::ModuleDelegate
{
public:
    /**
     * Constructor.
     *
     * @param[in] module Module.
     **/
    explicit
    Delegate(IB::Module module);

    /**
     * Acquire an oracle.
     *
     * @sa IBModPredicateCore::acquire().
     *
     * Looks up PerContext of @a context and forwards to
     * PerContext::acquire().
     *
     * @param[in] context Current context.
     * @param[in] node    Root node of expression.
     * @param[in] origin  Origin string, e.g., file and line number.
     * @return Oracle.
     **/
    IBModPredicateCore::oracle_t acquire(
        IB::Context      context,
        const P::node_p& node,
        const string&    origin
    ) const;

    //! Call factory accessor.
    P::CallFactory& call_factory();
    //! Call factory accessor.
    const P::CallFactory& call_factory() const;

    /**
     * Define a template.
     *
     * @sa IBModPredicateCore::define_template().
     *
     * @param[in] name   Name of template; must be unique.
     * @param[in] args   Arguments.
     * @param[in] body   Body.
     * @param[in] origin Origin, e.g., file and line number.
     * @throw einval if function named @a name already exists.
     **/
    void define_template(
        const std::string&                 name,
        const list<string>&                args,
        const IronBee::Predicate::node_cp& body,
        const std::string&                 origin
    );

    //! Fetch @ref per_context_t associated with @a context.
    PerContext& fetch_per_context(IB::ConstContext context) const;

private:
    //! Handle context open; forward to PerContext::open().
    void context_open(IB::Context context) const;
    //! Handle context close; forward to PerContext::close().
    void context_close(IB::Context context) const;

    /**
     * Handle @ref c_debug_report_directive.
     *
     * See MergeGraph::write_debug_report().
     *
     * @param[in] cp Configuration parser.
     * @param[in] to Where to write report.  Empty string or - means cerr.
     **/
    void dir_debug_report(
        IB::ConfigurationParser& cp,
        const char*              to
    ) const;

    /**
     * Handle @ref c_define_directive.
     *
     * See Template section of reference.txt.
     *
     * @param[in] cp     Configuration parser.
     * @param[in] params Parameters of directive.
     **/
    void dir_define(
        IB::ConfigurationParser& cp,
        IB::List<const char*>    params
    ) const;

    //! Call factory.
    P::CallFactory m_call_factory;
};

//! Find the Delegate given an engine.
Delegate& fetch_delegate(IB::Engine engine);

/**
 * Parse an sexpression into a node.
 *
 * @param[in] expr         Expression to parse.
 * @param[in] call_factory Call factory to use.
 * @param[in] origin       Origin; used in error messages.
 * @return root node of parsed expression.
 * @throw IronBee::einval on parse error.
 **/
P::node_p parse_expr(
    const string&         expr,
    const P::CallFactory& call_factory,
    const string&         origin
);

/**
 * Report an error or warning for a node in a merge graph.
 *
 * Logs the message along with information about the node including
 * its roots and origins.
 *
 * Intended to be used with as a Predicate::reporter_t (after binding).
 *
 * @param[in]  engine      Engine to use for logging.
 * @param[in]  merge_graph Merge graph.
 * @param[out] num_errors  Incremented if @a is_error is true.
 * @param[in]  is_error    True if error; false if warning.
 * @param[in]  message     Message.
 * @param[in]  node        Node message is for.
 **/
void report(
    IB::Engine           engine,
    const P::MergeGraph& merge_graph,
    size_t&              num_errors,
    bool                 is_error,
    const string&        message,
    const P::node_cp&    node
);

} // Anonymous

IBPP_BOOTSTRAP_MODULE_DELEGATE(c_module_name, Delegate);

// Implementation
namespace {

// PerContext

PerContext::PerContext(Delegate& delegate) :
    m_delegate(delegate),
    m_write_debug_report(false),
    m_merge_graph(new P::MergeGraph())
{
    // nop
}

PerContext::PerContext(const PerContext& other) :
    m_delegate(other.m_delegate),
    m_context(), // Context not copied.
    m_write_debug_report(other.m_write_debug_report),
    m_debug_report_to(other.m_debug_report_to),
    m_merge_graph(
        new P::MergeGraph(*other.m_merge_graph, m_delegate.call_factory())
    )
    // Note: Runtime members are not copied.
{
    // nop
}

void PerContext::open(IB::Context context)
{
    assert(! m_context);

    m_context = context;
}

void PerContext::close()
{
    // Life cycle.
    graph_lifecycle();

    // Index nodes.
    m_index_limit = 0;
    P::bfs_down(
        m_merge_graph->roots().first, m_merge_graph->roots().second,
        P::make_indexer(m_index_limit)
    );

    // Pre evaluate.
    pre_evaluate();

    // Build roots
    m_roots.resize(m_merge_graph->size());
    copy(
        m_merge_graph->roots().first, m_merge_graph->roots().second,
        m_roots.begin()
    );

    // Build oracle_index_to_root_node.
    m_oracle_index_to_root_node.resize(m_merge_graph->size());
    BOOST_FOREACH(const P::node_cp& root, m_roots) {
        BOOST_FOREACH(size_t index, m_merge_graph->root_indices(root)) {
            m_oracle_index_to_root_node[index] = root;
        }
    }

    // Drop configuration data.
    m_merge_graph.reset();
}

IBModPredicateCore::oracle_t PerContext::acquire(
    P::node_p     node,
    const string& origin
)
{
    size_t root_index = m_merge_graph->add_root(node);
    m_merge_graph->add_origin(node, origin);

    return bind(&PerContext::query, this, root_index, _1);
}

IBModPredicateCore::result_t PerContext::query(
    size_t          oracle_index,
    IB::Transaction tx
) const
{
    assert(oracle_index < m_oracle_index_to_root_node.size());

    PerTransaction& per_transaction = fetch_per_transaction(tx);
    const P::node_cp& node = m_oracle_index_to_root_node[oracle_index];
    return per_transaction.query(node);
}

void PerContext::assert_valid() const
{
    bool is_okay = false;

    stringstream report;
    is_okay = m_merge_graph->write_validation_report(report);

    if (! is_okay) {
        ib_log_error(
            delegate().module().engine().ib(),
            "Predicate Internal Validation Failure for context %s.",
            m_context.full_name()
        );
        string report_s = report.str();
        vector<string> messages;
        boost::algorithm::split(messages, report_s, boost::is_any_of("\n"));
        BOOST_FOREACH(const string& message, messages) {
            ib_log_error(
                delegate().module().engine().ib(),
                "  %s", message.c_str()
            );
        }
        BOOST_THROW_EXCEPTION(
            IB::einval() << IB::errinfo_what(
                "Predicate Internal Validation Failure"
            )
        );
    }
}

void PerContext::pre_evaluate()
{
    size_t num_errors = 0;
    P::reporter_t reporter = bind(
        &report,
        m_delegate.module().engine(),
        boost::ref(*m_merge_graph),
        boost::ref(num_errors),
        _1, _2, _3
    );
    P::pre_eval_graph(
        reporter,
        *m_merge_graph,
        m_context
    );
    if (num_errors > 0) {
        BOOST_THROW_EXCEPTION(
            IB::einval() << IB::errinfo_what(
                "Errors occurred during pre-evaluation."
                " See above."
            )
        );
    }
}

void PerContext::graph_lifecycle()
{
    ostream* debug_out;
    boost::scoped_ptr<ostream> debug_out_resource;

    if (
        m_write_debug_report &&
        ! m_debug_report_to.empty() &&
        m_debug_report_to != "-"
    ) {
        debug_out_resource.reset(
            new ofstream(m_debug_report_to.c_str(), ios_base::app)
        );
        debug_out = debug_out_resource.get();
        if (! *debug_out) {
            ib_log_error(m_delegate.module().engine().ib(),
                "Could not open %s for writing.",
                m_debug_report_to.c_str()
            );
            BOOST_THROW_EXCEPTION(IB::einval());
        }
    }
    else {
        debug_out = &cerr;
    }

    // Graph Lifecycle
    //
    // Below, we will...
    // 1. Pre-Transform: Validate graph before transformations.
    // 2. Transform: Transform graph until stable.
    // 3. Post-Transform: Validate graph after transformations.
    // 4. Pre-Evaluate: Provide the Engine to every node in the graph
    //    and instruct them to setup whatever data they need to evaluate.
    //
    // At each stage, any warnings and errors will be reported.  If errors
    // occur, the remaining stages are skipped and einval is thrown.
    // However, within each stage we gather as many errors and warnings
    // as possible.

    assert_valid();

    size_t num_errors = 0;
    P::reporter_t reporter = bind(
        &report,
        delegate().module().engine(),
        boost::ref(*m_merge_graph),
        boost::ref(num_errors),
        _1, _2, _3
    );

    if (m_write_debug_report) {
        *debug_out << "Before Transform: " << endl;
        m_merge_graph->write_debug_report(*debug_out);
    }

    // Pre-Transform
    {
        num_errors = 0;
        P::validate_graph(P::VALIDATE_PRE, reporter, *m_merge_graph);
        if (num_errors > 0) {
            BOOST_THROW_EXCEPTION(
                IB::einval() << IB::errinfo_what(
                    "Errors occurred during pre-transform validation."
                    " See above."
                )
            );
        }
    }

    // Transform
    {
        bool needs_transform = true;
        num_errors = 0;
        while (needs_transform) {
            needs_transform = P::transform_graph(
                reporter,
                *m_merge_graph,
                delegate().call_factory(),
                m_context
            );
            if (num_errors > 0) {
                BOOST_THROW_EXCEPTION(
                    IB::einval() << IB::errinfo_what(
                        "Errors occurred during DAG transformation."
                        " See above."
                    )
                );
            }
        }
    }

    assert_valid();

    if (m_write_debug_report) {
        *debug_out << "After Transform: " << endl;
        m_merge_graph->write_debug_report(*debug_out);
    }

    // Post-Transform
    {
        num_errors = 0;
        P::validate_graph(P::VALIDATE_POST, reporter, *m_merge_graph);
        if (num_errors > 0) {
            BOOST_THROW_EXCEPTION(
                IB::einval() << IB::errinfo_what(
                    "Errors occurred during post-transform validation."
                    " See above."
                )
            );
        }
    }
}

PerTransaction& PerContext::fetch_per_transaction(IB::Transaction tx) const
{
    typedef boost::shared_ptr<PerTransaction> per_transaction_p;

    per_transaction_p per_tx;
    try {
        per_tx = tx.get_module_data<per_transaction_p>(m_delegate.module());
    }
    catch (IB::enoent) {
        // nop
    }

    if (! per_tx) {
        per_tx.reset(new PerTransaction(m_index_limit, m_roots, tx));
        tx.set_module_data(m_delegate.module(), per_tx);
    }

    return *per_tx;
}

void PerContext::set_debug_report(const string& to)
{
    m_write_debug_report = true;
    m_debug_report_to = to;
}

const Delegate& PerContext::delegate() const
{
    return m_delegate;
}

Delegate& PerContext::delegate()
{
    return m_delegate;
}

// PerTransaction

PerTransaction::PerTransaction(
    size_t                    index_limit,
    const vector<P::node_cp>& roots,
    IB::Transaction           tx
) :
    m_graph_eval_state(index_limit),
    m_tx(tx)
{
    P::bfs_down(
        roots.begin(), roots.end(),
        P::make_initializer(m_graph_eval_state, tx)
    );
}

IBModPredicateCore::result_t PerTransaction::query(
    const P::node_cp& root
)
{
    m_graph_eval_state.eval(root, m_tx);

    return IBModPredicateCore::result_t(
        m_graph_eval_state.value(root->index()),
        m_graph_eval_state.is_finished(root->index())
    );
}

// Delegate

Delegate::Delegate(IB::Module module) :
    IB::ModuleDelegate(module)
{
    assert(module);

    IB::Engine engine = module.engine();

    // Call factory.
    P::Standard::load(m_call_factory);

    // Configuration data.
    PerContext base(*this);

    module.set_configuration_data<PerContext>(base);

    // Context close.
    engine.register_hooks()
        .context_open(
            boost::bind(&Delegate::context_open, this, _2)
        )
        .context_close(
            boost::bind(&Delegate::context_close, this, _2)
        )
        ;

    // Directives.
    engine.register_configuration_directives()
        .param1(
            c_debug_report_directive,
            bind(&Delegate::dir_debug_report, this, _1, _3)
        )
        .list(
            c_define_directive,
            bind(&Delegate::dir_define, this, _1, _3)
        )
        ;
}

IBModPredicateCore::oracle_t Delegate::acquire(
    IB::Context      context,
    const P::node_p& node,
    const string&    origin
) const
{
    return fetch_per_context(context).acquire(node, origin);
}

P::CallFactory& Delegate::call_factory()
{
    return m_call_factory;
}

const P::CallFactory& Delegate::call_factory() const
{
    return m_call_factory;
}

void Delegate::define_template(
    const std::string&                 name,
    const list<string>&                args,
    const IronBee::Predicate::node_cp& body,
    const std::string&                 origin
)
{
    m_call_factory.add(
        name,
        P::Standard::define_template(
            args,
            body,
            origin
        )
    );
}

PerContext& Delegate::fetch_per_context(IB::ConstContext context) const
{
    return module().configuration_data<PerContext>(context);
}

void Delegate::context_open(IB::Context context) const
{
    fetch_per_context(context).open(context);
}

void Delegate::context_close(IB::Context context) const
{
    fetch_per_context(context).close();
}

void Delegate::dir_debug_report(
    IB::ConfigurationParser& cp,
    const char*              to
) const
{
    fetch_per_context(cp.current_context()).set_debug_report(to);
}

void Delegate::dir_define(
    IB::ConfigurationParser& cp,
    IB::List<const char*>    params
) const
{
    if (params.size() != 3) {
        ib_cfg_log_error(
            cp.ib(),
            "%s must have three arguments: name, args, and body.",
            c_define_directive
        );
        BOOST_THROW_EXCEPTION(IB::einval());
    }

    IB::List<const char*>::const_iterator i = params.begin();
    string name = *i;
    ++i;
    string args = *i;
    ++i;
    string body = *i;

    P::Standard::template_arg_list_t arg_list;
    {
        size_t i = 0;
        while (i != string::npos) {
            size_t next_i = args.find_first_of(' ', i);
            arg_list.push_back(args.substr(i, next_i - i));
            i = args.find_first_not_of(' ', next_i);
        }
    }

    string origin = (
        boost::format("%s:%d ") %
        cp.current_file() %cp.ib()->curr->line
    ).str();

    IBModPredicateCore::define_template(
        module().engine(),
        name, arg_list, body,
        origin
    );
}

// Helpers

Delegate& fetch_delegate(IB::Engine engine)
{
    IB::Module module = IB::Module::with_name(engine, c_module_name);
    return module.configuration_data<PerContext>(
        engine.main_context()
    ).delegate();
}

P::node_p parse_expr(
    const string&         expr,
    const P::CallFactory& call_factory,
    const string&         origin
)
{
    size_t i = 0;
    P::node_p node = P::parse_call(expr, i, call_factory);
    if (i != expr.length() - 1) {
        // Parse failed.
        size_t pre_length  = max(i + 1,             size_t(10));
        size_t post_length = max(expr.length() - i, size_t(10));
        BOOST_THROW_EXCEPTION(
            IB::einval() << IB::errinfo_what(
                string("Predicate parser error: ") +
                expr.substr(i - pre_length, pre_length) +
                " --ERROR-- " +
                expr.substr(i + 1, post_length) +
                "[" + origin.c_str() + "]"
            )
        );
    }

    return node;
}

// Report Helpers

/**
 * Log a message.
 *
 * @param[in] engine   Engine to log to.
 * @param[in] is_error true if error; false if warning.
 * @param[in] message  Message to log.
 **/
void report_log(
    IB::Engine    engine,
    bool          is_error,
    const string& message
)
{
    if (is_error) {
        ib_log_error(engine.ib(), "%s", message.c_str());
    }
    else {
        ib_log_warning(engine.ib(), "%s", message.c_str());
    }
}

//! Push @a node to @a result if it is a root in @a merge_graph.
void report_find_roots_helper(
    list<P::node_cp>&    result,
    const P::MergeGraph& merge_graph,
    const P::node_cp&    node
)
{
    if (merge_graph.is_root(node)) {
        result.push_back(node);
    }
}

/**
 * Find all roots in @a merge_graph that have @a node as a descendant.
 *
 * @param[in] result      List to append roots to.
 * @param[in] merge_graph MergeGraph that determines roots.
 * @param[in] node        Node to find roots of.
 **/
void report_find_roots(
    list<P::node_cp>&    result,
    const P::MergeGraph& merge_graph,
    const P::node_cp&    node
)
{
    P::bfs_up(
        node,
        boost::make_function_output_iterator(
            boost::bind(
                report_find_roots_helper,
                boost::ref(result), boost::ref(merge_graph), _1
            )
        )
    );
}

// End report helpers

void report(
    IB::Engine           engine,
    const P::MergeGraph& merge_graph,
    size_t&              num_errors,
    bool                 is_error,
    const string&        message,
    const P::node_cp&    node
)
{
    if (is_error) {
        ++num_errors;
    }

    if (node) {
        report_log(engine, is_error, node->to_s() + " : " + message);
        BOOST_FOREACH(const string& origin, merge_graph.origins(node)) {
            report_log(engine, is_error, "  origin " + origin);
        }

        list<P::node_cp> roots;
        report_find_roots(roots, merge_graph, node);

        BOOST_FOREACH(const P::node_cp& root, roots) {
            report_log(engine, is_error, "  root " + root->to_s());
            BOOST_FOREACH(
                const string& origin, merge_graph.origins(root)
            ) {
                report_log(engine, is_error, "    origin " + origin);
            }
        }
    }
    else {
        report_log(engine, is_error, message);
    }
}

} // Anonymous

namespace IBModPredicateCore {

oracle_t acquire(
    IB::Engine    engine,
    IB::Context   context,
    const string& expr,
    const string& origin
)
{
    return acquire(
        engine,
        context,
        parse_expr(expr, call_factory(engine), origin),
        origin
    );
}

oracle_t acquire(
    IB::Engine       engine,
    IB::Context      context,
    const P::node_p& expr,
    const string&    origin
)
{
    return fetch_delegate(engine).acquire(context, expr, origin);
}

void define_template(
    IronBee::Engine               engine,
    const std::string&            name,
    const std::list<std::string>& args,
    const std::string&            body,
    const std::string&            origin
)
{
    return define_template(
        engine,
        name,
        args,
        parse_expr(body, call_factory(engine), origin),
        origin
    );
}

void define_template(
    IronBee::Engine                    engine,
    const std::string&                 name,
    const list<string>&                args,
    const IronBee::Predicate::node_cp& body,
    const std::string&                 origin
)
{
    fetch_delegate(engine).define_template(name, args, body, origin);
}

P::CallFactory& call_factory(
    IronBee::Engine engine
)
{
    return fetch_delegate(engine).call_factory();
}

} // IBModPredicateCore