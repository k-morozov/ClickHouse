#include <functional>
#include <iterator>
#include <Access/ContextAccess.h>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/IdentifierNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/Passes/QueryAnalysisPass.h>
#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/Utils.h>
#include <Common/quoteString.h>
#include <Columns/ColumnString.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/getLeastSupertype.h>
#include <DataTypes/IDataType.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/addTypeConversionToAST.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/getHeaderForProcessingStage.h>
#include <Interpreters/replaceAliasColumnsInQuery.h>
#include <Interpreters/addMissingDefaults.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Planner/PlannerActionsVisitor.h>
#include <Planner/Utils.h>
#include <Processors/QueryPlan/BuildQueryPipelineSettings.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Processors/Sources/NullSource.h>
#include <Processors/Transforms/FilterTransform.h>
#include <Processors/Transforms/MaterializingTransform.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <QueryPipeline/narrowPipe.h>
#include <Storages/AlterCommands.h>
#include <Storages/ReadInOrderOptimizer.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageDistributed.h>
#include <Storages/StorageFactory.h>
#include <Storages/StorageMerge.h>
#include <Storages/StorageView.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/checkAndGetLiteralArgument.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>
#include <Common/checkStackSize.h>
#include <Common/typeid_cast.h>
#include <Core/NamesAndTypes.h>
#include <Functions/FunctionFactory.h>


namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_experimental_analyzer;
    extern const SettingsSeconds lock_acquire_timeout;
    extern const SettingsFloat max_streams_multiplier_for_merge_tables;
    extern const SettingsUInt64 merge_table_max_tables_to_look_for_schema_inference;
}

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int BAD_ARGUMENTS;
extern const int NOT_IMPLEMENTED;
extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
extern const int SAMPLING_NOT_SUPPORTED;
extern const int ALTER_OF_COLUMN_IS_FORBIDDEN;
extern const int CANNOT_EXTRACT_TABLE_STRUCTURE;
extern const int STORAGE_REQUIRES_PARAMETER;
}

namespace
{

bool columnIsPhysical(ColumnDefaultKind kind)
{
    return kind == ColumnDefaultKind::Default || kind == ColumnDefaultKind::Materialized;
}

bool columnDefaultKindHasSameType(ColumnDefaultKind lhs, ColumnDefaultKind rhs)
{
    if (lhs == rhs)
        return true;

    if (columnIsPhysical(lhs) == columnIsPhysical(rhs))
        return true;

    return false;
}

/// Adds to the select query section `WITH value AS column_name`
///
/// For example:
/// - `WITH 9000 as _port`.
void rewriteEntityInAst(ASTPtr ast, const String & column_name, const Field & value)
{
    auto & select = ast->as<ASTSelectQuery &>();
    if (!select.with())
        select.setExpression(ASTSelectQuery::Expression::WITH, std::make_shared<ASTExpressionList>());

    auto literal = std::make_shared<ASTLiteral>(value);
    literal->alias = column_name;
    literal->prefer_alias_to_column_name = true;
    select.with()->children.push_back(literal);
}

}

StorageMerge::DatabaseNameOrRegexp::DatabaseNameOrRegexp(
    const String & source_database_name_or_regexp_,
    bool database_is_regexp_,
    std::optional<OptimizedRegularExpression> source_database_regexp_,
    std::optional<OptimizedRegularExpression> source_table_regexp_,
    std::optional<DBToTableSetMap> source_databases_and_tables_)
    : source_database_name_or_regexp(source_database_name_or_regexp_)
    , database_is_regexp(database_is_regexp_)
    , source_database_regexp(std::move(source_database_regexp_))
    , source_table_regexp(std::move(source_table_regexp_))
    , source_databases_and_tables(std::move(source_databases_and_tables_))
{
}

StorageMerge::StorageMerge(
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const String & comment,
    const String & source_database_name_or_regexp_,
    bool database_is_regexp_,
    const DBToTableSetMap & source_databases_and_tables_,
    ContextPtr context_)
    : IStorage(table_id_)
    , WithContext(context_->getGlobalContext())
    , database_name_or_regexp(
        source_database_name_or_regexp_,
        database_is_regexp_,
        source_database_name_or_regexp_, {},
        source_databases_and_tables_)
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_.empty()
        ? getColumnsDescriptionFromSourceTables(context_)
        : columns_);
    storage_metadata.setComment(comment);
    setInMemoryMetadata(storage_metadata);
    setVirtuals(createVirtuals());
}

StorageMerge::StorageMerge(
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const String & comment,
    const String & source_database_name_or_regexp_,
    bool database_is_regexp_,
    const String & source_table_regexp_,
    ContextPtr context_)
    : IStorage(table_id_)
    , WithContext(context_->getGlobalContext())
    , database_name_or_regexp(
        source_database_name_or_regexp_,
        database_is_regexp_,
        source_database_name_or_regexp_,
        source_table_regexp_, {})
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_.empty()
        ? getColumnsDescriptionFromSourceTables(context_)
        : columns_);
    storage_metadata.setComment(comment);
    setInMemoryMetadata(storage_metadata);
    setVirtuals(createVirtuals());
}

StorageMerge::DatabaseTablesIterators StorageMerge::getDatabaseIterators(ContextPtr context_) const
{
    return database_name_or_regexp.getDatabaseIterators(context_);
}

ColumnsDescription StorageMerge::getColumnsDescriptionFromSourceTables(
    const ContextPtr & query_context,
    const String & source_database_name_or_regexp,
    bool database_is_regexp,
    const String & source_table_regexp,
    size_t max_tables_to_look)
{
    DatabaseNameOrRegexp database_name_or_regexp(source_database_name_or_regexp, database_is_regexp, source_database_name_or_regexp, source_table_regexp, {});
    return getColumnsDescriptionFromSourceTablesImpl(query_context, database_name_or_regexp, max_tables_to_look, nullptr);
}

ColumnsDescription StorageMerge::getColumnsDescriptionFromSourceTables(const ContextPtr & query_context) const
{
    auto max_tables_to_look = query_context->getSettingsRef()[Setting::merge_table_max_tables_to_look_for_schema_inference];
    auto res = getColumnsDescriptionFromSourceTablesImpl(query_context, database_name_or_regexp, max_tables_to_look, this);
    if (res.empty())
        throw Exception{DB::ErrorCodes::CANNOT_EXTRACT_TABLE_STRUCTURE, "There are no tables satisfied provided regexp, you must specify table structure manually"};
    return res;
}

ColumnsDescription StorageMerge::getColumnsDescriptionFromSourceTablesImpl(
    const ContextPtr & query_context,
    const DatabaseNameOrRegexp & database_name_or_regexp,
    size_t max_tables_to_look,
    const IStorage * ignore_self)
{
    auto access = query_context->getAccess();
    size_t table_num = 0;
    ColumnsDescription res;

    traverseTablesUntilImpl(query_context, ignore_self, database_name_or_regexp, [&table_num, &access, &res, max_tables_to_look](auto && t)
    {
        if (!t)
            return false;

        if (auto id = t->getStorageID(); !access->isGranted(AccessType::SHOW_TABLES, id.database_name, id.table_name))
            return false;

        auto structure = t->getInMemoryMetadataPtr()->getColumns();
        String prev_column_name;
        for (const ColumnDescription & column : structure)
        {
            if (!res.has(column.name))
            {
                res.add(column, prev_column_name);
            }
            else if (column != res.get(column.name))
            {
                res.modify(column.name, [&column](ColumnDescription & what)
                {
                    what.type = getLeastSupertypeOrVariant(DataTypes{what.type, column.type});
                    if (what.default_desc != column.default_desc)
                        what.default_desc = {};
                });
            }
            prev_column_name = column.name;
        }

        ++table_num;
        return table_num >= max_tables_to_look;
    });

    return res;
}

template <typename F>
StoragePtr StorageMerge::traverseTablesUntil(F && predicate) const
{
    return traverseTablesUntilImpl(getContext(), this, database_name_or_regexp, std::forward<F>(predicate));
}

template <typename F>
StoragePtr StorageMerge::traverseTablesUntilImpl(const ContextPtr & query_context, const IStorage * ignore_self, const DatabaseNameOrRegexp & database_name_or_regexp, F && predicate)
{
    auto database_table_iterators = database_name_or_regexp.getDatabaseIterators(query_context);

    for (auto & iterator : database_table_iterators)
    {
        while (iterator->isValid())
        {
            const auto & table = iterator->table();
            if (table.get() != ignore_self && predicate(table))
                return table;

            iterator->next();
        }
    }

    return {};
}

template <typename F>
void StorageMerge::forEachTable(F && func) const
{
    traverseTablesUntil([&func](const auto & table)
    {
        func(table);
        /// Always continue to the next table.
        return false;
    });
}

bool StorageMerge::isRemote() const
{
    auto first_remote_table = traverseTablesUntil([](const StoragePtr & table) { return table && table->isRemote(); });
    return first_remote_table != nullptr;
}

bool StorageMerge::supportsPrewhere() const
{
    return traverseTablesUntil([](const auto & table) { return !table->supportsPrewhere(); }) == nullptr;
}

bool StorageMerge::canMoveConditionsToPrewhere() const
{
    /// NOTE: This check and the above check are used during query analysis as condition for applying
    /// "move to PREWHERE" optimization. However, it contains a logical race:
    /// If new table that matches regexp for current storage and doesn't support PREWHERE
    /// will appear after this check and before calling "read" method, the optimized query may fail.
    /// Since it's quite rare case, we just ignore this possibility.
    /// TODO: Store tables inside StorageSnapshot
    ///
    /// NOTE: Type can be different, and in this case, PREWHERE cannot be
    /// applied for those columns, but there a separate method to return
    /// supported columns for PREWHERE - supportedPrewhereColumns().
    return traverseTablesUntil([](const auto & table) { return !table->canMoveConditionsToPrewhere(); }) == nullptr;
}

std::optional<NameSet> StorageMerge::supportedPrewhereColumns() const
{
    bool supports_prewhere = true;

    const auto & metadata = getInMemoryMetadata();
    const auto & columns = metadata.getColumns();

    NameSet supported_columns;

    std::unordered_map<std::string, std::pair<const IDataType *, ColumnDefaultKind>> column_info;
    for (const auto & name_type : columns.getAll())
    {
        const auto & column_default = columns.getDefault(name_type.name).value_or(ColumnDefault{});
        column_info.emplace(name_type.name, std::make_pair(
            name_type.type.get(),
            column_default.kind));
        supported_columns.emplace(name_type.name);
    }

    forEachTable([&](const StoragePtr & table)
    {
        const auto & table_metadata_ptr = table->getInMemoryMetadataPtr();
        if (!table_metadata_ptr)
            supports_prewhere = false;
        if (!supports_prewhere)
            return;

        const auto & table_columns = table_metadata_ptr->getColumns();
        for (const auto & column : table_columns.getAll())
        {
            const auto & column_default = table_columns.getDefault(column.name).value_or(ColumnDefault{});
            const auto & [root_type, src_default_kind] = column_info[column.name];
            if ((root_type && !root_type->equals(*column.type)) ||
                !columnDefaultKindHasSameType(src_default_kind, column_default.kind))
            {
                supported_columns.erase(column.name);
            }
        }
    });

    return supported_columns;
}

QueryProcessingStage::Enum StorageMerge::getQueryProcessingStage(
    ContextPtr local_context,
    QueryProcessingStage::Enum to_stage,
    const StorageSnapshotPtr &,
    SelectQueryInfo & query_info) const
{
    /// In case of JOIN the first stage (which includes JOIN)
    /// should be done on the initiator always.
    ///
    /// Since in case of JOIN query on shards will receive query without JOIN (and their columns).
    /// (see removeJoin())
    ///
    /// And for this we need to return FetchColumns.
    if (const auto * select = query_info.query->as<ASTSelectQuery>(); select && hasJoin(*select))
        return QueryProcessingStage::FetchColumns;

    auto stage_in_source_tables = QueryProcessingStage::FetchColumns;

    DatabaseTablesIterators database_table_iterators = database_name_or_regexp.getDatabaseIterators(local_context);

    size_t selected_table_size = 0;

    for (const auto & iterator : database_table_iterators)
    {
        while (iterator->isValid())
        {
            const auto & table = iterator->table();
            if (table && table.get() != this)
            {
                ++selected_table_size;
                stage_in_source_tables = std::max(
                    stage_in_source_tables,
                    table->getQueryProcessingStage(local_context, to_stage,
                        table->getStorageSnapshot(table->getInMemoryMetadataPtr(), local_context), query_info));
            }

            iterator->next();
        }
    }

    return selected_table_size == 1 ? stage_in_source_tables : std::min(stage_in_source_tables, QueryProcessingStage::WithMergeableState);
}

VirtualColumnsDescription StorageMerge::createVirtuals()
{
    VirtualColumnsDescription desc;

    desc.addEphemeral("_database", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "");
    desc.addEphemeral("_table", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "");

    return desc;
}

StorageSnapshotPtr StorageMerge::getStorageSnapshot(const StorageMetadataPtr & metadata_snapshot, ContextPtr query_context) const
{
    static const auto common_virtuals = createVirtuals();
    const auto & access = query_context->getAccess();

    auto virtuals = common_virtuals;
    if (auto first_table = traverseTablesUntil([access](auto && table)
    {
        if (!table)
            return false;

        auto id = table->getStorageID();
        return access->isGranted(AccessType::SHOW_TABLES, id.database_name, id.table_name);
    }))
    {
        auto table_virtuals = first_table->getVirtualsPtr();
        for (const auto & column : *table_virtuals)
        {
            if (virtuals.has(column.name))
                continue;

            virtuals.add(column);
        }
    }

    auto virtuals_ptr = std::make_shared<VirtualColumnsDescription>(std::move(virtuals));
    return std::make_shared<StorageSnapshot>(*this, metadata_snapshot, std::move(virtuals_ptr));
}

void StorageMerge::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum processed_stage,
    const size_t max_block_size,
    size_t num_streams)
{
    /// What will be result structure depending on query processed stage in source tables?
    auto common_header = getHeaderForProcessingStage(column_names, storage_snapshot, query_info, local_context, processed_stage);

    if (local_context->getSettingsRef()[Setting::allow_experimental_analyzer] && processed_stage == QueryProcessingStage::Complete)
    {
        auto block = *common_header;
        /// Remove constants.
        /// For StorageDistributed some functions like `hostName` that are constants only for local queries.
        for (auto & column : block)
            column.column = column.column->convertToFullColumnIfConst();
        common_header = std::make_shared<const Block>(std::move(block));
    }

    auto step = std::make_unique<ReadFromMerge>(
        column_names,
        query_info,
        storage_snapshot,
        local_context,
        common_header,
        max_block_size,
        num_streams,
        shared_from_this(),
        processed_stage);

    query_plan.addStep(std::move(step));
}

ReadFromMerge::ReadFromMerge(
    const Names & column_names_,
    const SelectQueryInfo & query_info_,
    const StorageSnapshotPtr & storage_snapshot_,
    const ContextPtr & context_,
    SharedHeader common_header_,
    size_t max_block_size,
    size_t num_streams,
    StoragePtr storage,
    QueryProcessingStage::Enum processed_stage)
    : SourceStepWithFilter(common_header_, column_names_, query_info_, storage_snapshot_, context_)
    , required_max_block_size(max_block_size)
    , requested_num_streams(num_streams)
    , common_header(common_header_)
    , all_column_names(column_names_)
    , storage_merge(std::move(storage))
    , merge_storage_snapshot(storage_snapshot)
    , common_processed_stage(processed_stage)
{
}

void ReadFromMerge::addFilter(FilterDAGInfo filter)
{
    output_header = std::make_shared<const Block>(FilterTransform::transformHeader(
            *output_header,
            &filter.actions,
            filter.column_name,
            filter.do_remove_column));
    pushed_down_filters.push_back(std::move(filter));
}

void ReadFromMerge::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{
    filterTablesAndCreateChildrenPlans();

    if (selected_tables.empty())
    {
        pipeline.init(Pipe(std::make_shared<NullSource>(output_header)));
        return;
    }

    QueryPlanResourceHolder resources;
    std::vector<std::unique_ptr<QueryPipelineBuilder>> pipelines;

    auto table_it = selected_tables.begin();
    auto modified_context = Context::createCopy(context);
    for (size_t i = 0; i < selected_tables.size(); ++i, ++table_it)
    {
        auto & child_plan = child_plans->at(i);
        const auto & table = *table_it;
        auto source_pipeline = buildPipeline(child_plan, common_processed_stage);

        if (source_pipeline && source_pipeline->initialized())
        {
            resources.storage_holders.push_back(std::get<1>(table));
            resources.table_locks.push_back(std::get<2>(table));

            pipelines.emplace_back(std::move(source_pipeline));
        }
    }

    if (pipelines.empty())
    {
        pipeline.init(Pipe(std::make_shared<NullSource>(output_header)));
        return;
    }

    pipeline = QueryPipelineBuilder::unitePipelines(std::move(pipelines));

    if (!query_info.input_order_info)
    {
        size_t tables_count = selected_tables.size();
        Float64 num_streams_multiplier = std::min(
            tables_count, std::max(1UL, static_cast<size_t>(context->getSettingsRef()[Setting::max_streams_multiplier_for_merge_tables])));
        size_t num_streams = static_cast<size_t>(requested_num_streams * num_streams_multiplier);

        // It's possible to have many tables read from merge, resize(num_streams) might open too many files at the same time.
        // Using narrowPipe instead. But in case of reading in order of primary key, we cannot do it,
        // because narrowPipe doesn't preserve order.
        pipeline.narrow(num_streams);
    }

    pipeline.addResources(std::move(resources));
}

void ReadFromMerge::filterTablesAndCreateChildrenPlans()
{
    if (child_plans)
        return;

    has_database_virtual_column = false;
    has_table_virtual_column = false;
    column_names.clear();
    column_names.reserve(all_column_names.size());

    for (const auto & column_name : all_column_names)
    {
        if (column_name == "_database" && storage_merge->isVirtualColumn(column_name, merge_storage_snapshot->metadata))
            has_database_virtual_column = true;
        else if (column_name == "_table" && storage_merge->isVirtualColumn(column_name, merge_storage_snapshot->metadata))
            has_table_virtual_column = true;
        else
            column_names.push_back(column_name);
    }

    selected_tables = getSelectedTables(context, has_database_virtual_column, has_table_virtual_column);
    child_plans = createChildrenPlans(query_info);
}

std::vector<ReadFromMerge::ChildPlan> ReadFromMerge::createChildrenPlans(SelectQueryInfo & query_info_) const
{
    if (selected_tables.empty())
        return {};

    std::vector<ChildPlan> res;

    size_t tables_count = selected_tables.size();
    Float64 num_streams_multiplier
        = std::min(tables_count, std::max(1UL, static_cast<size_t>(context->getSettingsRef()[Setting::max_streams_multiplier_for_merge_tables])));
    size_t num_streams = static_cast<size_t>(requested_num_streams * num_streams_multiplier);
    size_t remaining_streams = num_streams;

    if (order_info)
    {
        query_info_.input_order_info = order_info;
    }
    else if (query_info.order_optimizer)
    {
        InputOrderInfoPtr input_sorting_info;
        for (auto it = selected_tables.begin(); it != selected_tables.end(); ++it)
        {
            auto storage_ptr = std::get<1>(*it);
            auto storage_metadata_snapshot = storage_ptr->getInMemoryMetadataPtr();
            auto current_info = query_info.order_optimizer->getInputOrder(storage_metadata_snapshot, context);
            if (it == selected_tables.begin())
                input_sorting_info = current_info;
            else if (!current_info || (input_sorting_info && *current_info != *input_sorting_info))
                input_sorting_info.reset();

            if (!input_sorting_info)
                break;
        }

        query_info_.input_order_info = input_sorting_info;
    }

    auto logger = getLogger("StorageMerge");

    /// Settings will be modified when planning children tables.
    for (const auto & table : selected_tables)
    {
        const auto & storage = std::get<1>(table);

        LOG_TRACE(logger, "Building plan for child table {}", storage->getStorageID().getNameForLogs());

        try
        {
            auto modified_context = Context::createCopy(context);

            size_t current_need_streams = tables_count >= num_streams ? 1 : (num_streams / tables_count);
            size_t current_streams = std::min(current_need_streams, remaining_streams);
            remaining_streams -= current_streams;
            current_streams = std::max(1uz, current_streams);

            bool sampling_requested = query_info.query->as<ASTSelectQuery>()->sampleSize() != nullptr;
            if (query_info.table_expression_modifiers)
                sampling_requested = query_info.table_expression_modifiers->hasSampleSizeRatio();

            /// If sampling requested, then check that table supports it.
            if (sampling_requested && !storage->supportsSampling())
                throw Exception(ErrorCodes::SAMPLING_NOT_SUPPORTED, "Illegal SAMPLE: table {} doesn't support sampling", storage->getStorageID().getNameForLogs());

            Aliases aliases;
            RowPolicyDataOpt row_policy_data_opt;
            auto storage_metadata_snapshot = storage->getInMemoryMetadataPtr();

            if (storage_metadata_snapshot->getColumns().empty())
            {
                /// (Assuming that view has empty list of columns if it's parameterized.)
                if (storage->isView() && storage->as<StorageView>() && storage->as<StorageView>()->isParameterizedView())
                    throw Exception(ErrorCodes::STORAGE_REQUIRES_PARAMETER, "Parameterized view can't be queried through a Merge table.");
                else
                    throw Exception(ErrorCodes::LOGICAL_ERROR, "Table has no columns.");
            }

            auto nested_storage_snapshot = storage->getStorageSnapshot(storage_metadata_snapshot, modified_context);

            Names column_names_as_aliases;
            Names real_column_names = column_names;
            /// If there are no real columns requested from this table, we will read the smallest column.
            /// We should remember it to not include this column in the result.
            bool is_smallest_column_requested = false;

            const auto & database_name = std::get<0>(table);
            const auto & table_name = std::get<3>(table);
            auto row_policy_filter_ptr = modified_context->getRowPolicyFilter(
                database_name,
                table_name,
                RowPolicyFilterType::SELECT_FILTER);
            if (row_policy_filter_ptr && !row_policy_filter_ptr->empty())
            {
                row_policy_data_opt = RowPolicyData(row_policy_filter_ptr, storage, modified_context);
                row_policy_data_opt->extendNames(real_column_names);
            }

            auto modified_query_info
                = getModifiedQueryInfo(modified_context, table, nested_storage_snapshot, real_column_names, column_names_as_aliases, is_smallest_column_requested, aliases);

            if (!context->getSettingsRef()[Setting::allow_experimental_analyzer])
            {
                auto storage_columns = storage_metadata_snapshot->getColumns();
                auto syntax_result = TreeRewriter(context).analyzeSelect(
                    modified_query_info.query, TreeRewriterResult({}, storage, nested_storage_snapshot));

                bool with_aliases = common_processed_stage == QueryProcessingStage::FetchColumns && !storage_columns.getAliases().empty();
                if (with_aliases)
                {
                    ASTPtr required_columns_expr_list = std::make_shared<ASTExpressionList>();
                    ASTPtr column_expr;

                    auto sample_block = merge_storage_snapshot->metadata->getSampleBlock();

                    for (const auto & column : real_column_names)
                    {
                        const auto column_default = storage_columns.getDefault(column);
                        bool is_alias = column_default && column_default->kind == ColumnDefaultKind::Alias;

                        if (is_alias)
                        {
                            column_expr = column_default->expression->clone();
                            replaceAliasColumnsInQuery(column_expr, storage_metadata_snapshot->getColumns(),
                                                    syntax_result->array_join_result_to_source, context);

                            const auto & column_description = storage_columns.get(column);
                            column_expr = addTypeConversionToAST(std::move(column_expr), column_description.type->getName(),
                                                                storage_metadata_snapshot->getColumns().getAll(), context);
                            column_expr = setAlias(column_expr, column);

                            /// use storage type for transient columns that are not represented in result
                            ///  e.g. for columns that needed to evaluate row policy
                            auto type = sample_block.has(column) ? sample_block.getByName(column).type : column_description.type;

                            aliases.push_back({ .name = column, .type = type, .expression = column_expr->clone() });
                        }
                        else
                            column_expr = std::make_shared<ASTIdentifier>(column);

                        required_columns_expr_list->children.emplace_back(std::move(column_expr));
                    }

                    syntax_result = TreeRewriter(context).analyze(
                        required_columns_expr_list, storage_columns.getAllPhysical(), storage, storage->getStorageSnapshot(storage_metadata_snapshot, context));

                    auto alias_actions = ExpressionAnalyzer(required_columns_expr_list, syntax_result, context).getActionsDAG(true);

                    column_names_as_aliases = alias_actions.getRequiredColumns().getNames();
                    if (column_names_as_aliases.empty())
                    {
                        column_names_as_aliases.push_back(ExpressionActions::getSmallestColumn(storage_metadata_snapshot->getColumns().getAllPhysical()).name);
                        is_smallest_column_requested = true;
                    }
                }
            }

            Names column_names_to_read = column_names_as_aliases.empty() ? std::move(real_column_names) : std::move(column_names_as_aliases);

            std::erase_if(column_names_to_read, [existing_columns = nested_storage_snapshot->getAllColumnsDescription()](const auto & column_name){ return !existing_columns.has(column_name) && !existing_columns.hasSubcolumn(column_name); });

            auto child = createPlanForTable(
                nested_storage_snapshot,
                modified_query_info,
                common_processed_stage,
                required_max_block_size,
                table,
                column_names_to_read,
                is_smallest_column_requested,
                row_policy_data_opt,
                modified_context,
                current_streams);

            child.plan.addInterpreterContext(modified_context);

            if (child.plan.isInitialized())
            {
                addVirtualColumns(child, modified_query_info, common_processed_stage, table);

                /// Source tables could have different but convertible types, like numeric types of different width.
                /// We must return streams with structure equals to structure of Merge table.
                convertAndFilterSourceStream(*common_header, modified_query_info, nested_storage_snapshot, aliases, row_policy_data_opt, context, child, is_smallest_column_requested);

                for (const auto & filter_info : pushed_down_filters)
                {
                    auto filter_step = std::make_unique<FilterStep>(
                        child.plan.getCurrentHeader(),
                        filter_info.actions.clone(),
                        filter_info.column_name,
                        filter_info.do_remove_column);

                    child.plan.addStep(std::move(filter_step));
                }

                child.plan.optimize(QueryPlanOptimizationSettings(modified_context));
            }

            res.emplace_back(std::move(child));
        }
        catch (Exception & e)
        {
            e.addMessage("Child table: " + storage->getStorageID().getNameForLogs());
            throw;
        }
    }

    return res;
}

namespace
{

class ApplyAliasColumnExpressionsVisitor : public InDepthQueryTreeVisitor<ApplyAliasColumnExpressionsVisitor>
{
public:
    explicit ApplyAliasColumnExpressionsVisitor(QueryTreeNodePtr replacement_table_expression_)
        : replacement_table_expression(replacement_table_expression_)
    {}

    void visitImpl(QueryTreeNodePtr & node)
    {
        if (auto * column = node->as<ColumnNode>())
        {
            if (column->hasExpression())
            {
                QueryTreeNodePtr column_expression = column->getExpressionOrThrow();
                column_expression->setAlias(column->getColumnName());
                node = std::move(column_expression);
            }
            else
                column->setColumnSource(replacement_table_expression);
        }
    }
private:
    QueryTreeNodePtr replacement_table_expression;
};

QueryTreeNodePtr replaceTableExpressionAndRemoveJoin(
    QueryTreeNodePtr query,
    QueryTreeNodePtr original_table_expression,
    QueryTreeNodePtr replacement_table_expression,
    const ContextPtr & context,
    const Names & required_column_names)
{
    auto * query_node = query->as<QueryNode>();
    auto join_tree_type = query_node->getJoinTree()->getNodeType();
    auto modified_query = query_node->cloneAndReplace(original_table_expression, replacement_table_expression);

    // For the case when join tree is just a table or a table function we don't need to do anything more.
    if (join_tree_type == QueryTreeNodeType::TABLE || join_tree_type == QueryTreeNodeType::TABLE_FUNCTION)
        return modified_query;

    // JOIN needs to be removed because StorageMerge should produce not joined data.
    // GROUP BY should be removed as well.

    auto * modified_query_node = modified_query->as<QueryNode>();

    // Remove the JOIN statement. As a result query will have a form like: SELECT * FROM <table> ...
    modified_query = modified_query->cloneAndReplace(modified_query_node->getJoinTree(), replacement_table_expression);
    modified_query_node = modified_query->as<QueryNode>();

    query_node = modified_query->as<QueryNode>();

    // For backward compatibility we need to leave all filters related to this table.
    // It may lead to some incorrect result.
    if (query_node->hasPrewhere())
        removeExpressionsThatDoNotDependOnTableIdentifiers(query_node->getPrewhere(), replacement_table_expression, context);
    if (query_node->hasWhere())
        removeExpressionsThatDoNotDependOnTableIdentifiers(query_node->getWhere(), replacement_table_expression, context);

    query_node->getGroupBy().getNodes().clear();
    query_node->getHaving() = {};
    query_node->getOrderBy().getNodes().clear();

    auto & projection = modified_query_node->getProjection().getNodes();
    projection.clear();
    NamesAndTypes projection_columns;

    // Select only required columns from the table, because projection list may contain:
    // 1. aggregate functions
    // 2. expressions referencing other tables of JOIN
    for (auto const & column_name : required_column_names)
    {
        QueryTreeNodePtr fake_node = std::make_shared<IdentifierNode>(Identifier{column_name});

        QueryAnalysisPass query_analysis_pass(original_table_expression);
        query_analysis_pass.run(fake_node, context);

        auto * resolved_column = fake_node->as<ColumnNode>();
        if (!resolved_column)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Required column '{}' is not resolved", column_name);
        auto fake_column = resolved_column->getColumn();

        // Identifier is resolved to ColumnNode, but we need to get rid of ALIAS columns
        // and also fix references to source expression (now column is referencing original table expression).
        ApplyAliasColumnExpressionsVisitor visitor(replacement_table_expression);
        visitor.visit(fake_node);

        projection.push_back(fake_node);
        projection_columns.push_back(fake_column);
    }

    query_node->resolveProjectionColumns(std::move(projection_columns));

    return modified_query;
}

}

SelectQueryInfo ReadFromMerge::getModifiedQueryInfo(const ContextMutablePtr & modified_context,
    const StorageWithLockAndName & storage_with_lock_and_name,
    const StorageSnapshotPtr & storage_snapshot_,
    Names required_column_names,
    Names & column_names_as_aliases,
    bool & is_smallest_column_requested,
    Aliases & aliases) const
{
    const auto & [database_name, storage, storage_lock, table_name] = storage_with_lock_and_name;
    const StorageID current_storage_id = storage->getStorageID();

    SelectQueryInfo modified_query_info = query_info;

    modified_query_info.initial_storage_snapshot = merge_storage_snapshot;

    if (modified_query_info.planner_context)
        modified_query_info.planner_context = std::make_shared<PlannerContext>(modified_context, modified_query_info.planner_context);

    if (modified_query_info.table_expression)
    {
        auto replacement_table_expression = std::make_shared<TableNode>(storage, storage_lock, storage_snapshot_);
        replacement_table_expression->setAlias(modified_query_info.table_expression->getAlias());
        if (query_info.table_expression_modifiers)
            replacement_table_expression->setTableExpressionModifiers(*query_info.table_expression_modifiers);

        modified_query_info.query_tree = replaceTableExpressionAndRemoveJoin(modified_query_info.query_tree, modified_query_info.table_expression, replacement_table_expression, modified_context, required_column_names);
        modified_query_info.table_expression = replacement_table_expression;
        modified_query_info.planner_context->getOrCreateTableExpressionData(replacement_table_expression);

        auto get_column_options = GetColumnsOptions(GetColumnsOptions::All)
            .withExtendedObjects()
            .withSubcolumns(storage_snapshot_->storage.supportsSubcolumns());

        std::unordered_map<std::string, QueryTreeNodePtr> column_name_to_node;

        /// Consider only non-virtual columns of storage while checking for _table and _database columns.
        /// I.e. always override virtual columns with these names from underlying table (if any).
        if (!storage_snapshot_->tryGetColumn(get_column_options, "_table"))
        {
            auto table_name_node = std::make_shared<ConstantNode>(current_storage_id.table_name);
            auto table_name_alias = std::make_shared<ConstantNode>("__table1._table");

            auto function_node = std::make_shared<FunctionNode>("__actionName");
            function_node->getArguments().getNodes().push_back(std::move(table_name_node));
            function_node->getArguments().getNodes().push_back(std::move(table_name_alias));
            function_node->resolveAsFunction(FunctionFactory::instance().get("__actionName", context));

            column_name_to_node.emplace("_table", function_node);
        }

        if (!storage_snapshot_->tryGetColumn(get_column_options, "_database"))
        {
            auto database_name_node = std::make_shared<ConstantNode>(current_storage_id.database_name);
            auto database_name_alias = std::make_shared<ConstantNode>("__table1._database");

            auto function_node = std::make_shared<FunctionNode>("__actionName");
            function_node->getArguments().getNodes().push_back(std::move(database_name_node));
            function_node->getArguments().getNodes().push_back(std::move(database_name_alias));
            function_node->resolveAsFunction(FunctionFactory::instance().get("__actionName", context));

            column_name_to_node.emplace("_database", function_node);
        }

        get_column_options.withVirtuals();
        auto storage_columns = storage_snapshot_->metadata->getColumns();

        bool with_aliases = /* common_processed_stage == QueryProcessingStage::FetchColumns && */ !storage_columns.getAliases().empty();
        if (with_aliases)
        {
            auto filter_actions_dag = std::make_shared<ActionsDAG>();
            for (const auto & column : required_column_names)
            {
                const auto column_default = storage_columns.getDefault(column);
                bool is_alias = column_default && column_default->kind == ColumnDefaultKind::Alias;

                QueryTreeNodePtr column_node;

                // Replace all references to ALIAS columns in the query by expressions.
                if (is_alias)
                {
                    QueryTreeNodePtr fake_node = std::make_shared<IdentifierNode>(Identifier{column});

                    QueryAnalysisPass query_analysis_pass(modified_query_info.table_expression);
                    query_analysis_pass.run(fake_node, modified_context);

                    auto * resolved_column = fake_node->as<ColumnNode>();

                    column_node = fake_node;
                    ApplyAliasColumnExpressionsVisitor visitor(replacement_table_expression);
                    visitor.visit(column_node);

                    if (!resolved_column || !resolved_column->getExpression())
                        throw Exception(ErrorCodes::LOGICAL_ERROR, "Alias column is not resolved");

                    column_name_to_node.emplace(column, column_node);
                    aliases.push_back({ .name = column, .type = resolved_column->getResultType(), .expression = column_node->toAST() });
                }
                else
                {
                    column_node = std::make_shared<ColumnNode>(NameAndTypePair{column, storage_columns.getColumn(get_column_options, column).type }, modified_query_info.table_expression);
                }

                ColumnNodePtrWithHashSet empty_correlated_columns_set;
                PlannerActionsVisitor actions_visitor(modified_query_info.planner_context, empty_correlated_columns_set, false /*use_column_identifier_as_action_node_name*/);
                actions_visitor.visit(*filter_actions_dag, column_node);
            }
            column_names_as_aliases = filter_actions_dag->getRequiredColumnsNames();
            if (column_names_as_aliases.empty())
            {
                column_names_as_aliases.push_back(ExpressionActions::getSmallestColumn(storage_snapshot_->metadata->getColumns().getAllPhysical()).name);
                is_smallest_column_requested = true;
            }
        }

        if (!column_name_to_node.empty())
        {
            replaceColumns(modified_query_info.query_tree,
                replacement_table_expression,
                column_name_to_node);
        }

        modified_query_info.query = queryNodeToSelectQuery(modified_query_info.query_tree);
    }
    else
    {
        bool is_storage_merge_engine = storage->as<StorageMerge>();
        modified_query_info.query = query_info.query->clone();

        /// Original query could contain JOIN but we need only the first joined table and its columns.
        auto & modified_select = modified_query_info.query->as<ASTSelectQuery &>();
        TreeRewriterResult new_analyzer_res = *modified_query_info.syntax_analyzer_result;
        removeJoin(modified_select, new_analyzer_res, modified_context);
        modified_query_info.syntax_analyzer_result = std::make_shared<TreeRewriterResult>(std::move(new_analyzer_res));

        if (!is_storage_merge_engine)
        {
            rewriteEntityInAst(modified_query_info.query, "_table", current_storage_id.table_name);
            rewriteEntityInAst(modified_query_info.query, "_database", current_storage_id.database_name);
        }
    }

    return modified_query_info;
}

bool recursivelyApplyToReadingSteps(QueryPlan::Node * node, const std::function<bool(ReadFromMergeTree &)> & func)
{
    bool ok = true;
    for (auto * child : node->children)
        ok &= recursivelyApplyToReadingSteps(child, func);

    // This code is mainly meant to be used to call `requestReadingInOrder` on child steps.
    // In this case it is ok if one child will read in order and other will not (though I don't know when it is possible),
    // the only important part is to acknowledge this at the parent and don't rely on any particular ordering of input data.
    if (!ok)
        return false;

    if (auto * read_from_merge_tree = typeid_cast<ReadFromMergeTree *>(node->step.get()))
        ok &= func(*read_from_merge_tree);

    return ok;
}

void ReadFromMerge::addVirtualColumns(
    ChildPlan & child,
    SelectQueryInfo & modified_query_info,
    QueryProcessingStage::Enum processed_stage,
    const StorageWithLockAndName & storage_with_lock) const
{
    const auto & [database_name, _, storage, table_name] = storage_with_lock;

    /// Add virtual columns if we don't already have them.

    auto plan_header = child.plan.getCurrentHeader();

    if (context->getSettingsRef()[Setting::allow_experimental_analyzer])
    {
        String table_alias = modified_query_info.query_tree->as<QueryNode>()->getJoinTree()->as<TableNode>()->getAlias();

        String database_column = table_alias.empty() || processed_stage == QueryProcessingStage::FetchColumns ? "_database" : table_alias + "._database";
        String table_column = table_alias.empty() || processed_stage == QueryProcessingStage::FetchColumns ? "_table" : table_alias + "._table";

        if (has_database_virtual_column && common_header->has(database_column)
            && child.stage == QueryProcessingStage::FetchColumns && !plan_header->has(database_column))
        {
            ColumnWithTypeAndName column;
            column.name = database_column;
            column.type = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());
            column.column = column.type->createColumnConst(0, Field(database_name));

            auto adding_column_dag = ActionsDAG::makeAddingColumnActions(std::move(column));
            auto expression_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(adding_column_dag));
            child.plan.addStep(std::move(expression_step));
            plan_header = child.plan.getCurrentHeader();
        }

        if (has_table_virtual_column && common_header->has(table_column)
            && child.stage == QueryProcessingStage::FetchColumns && !plan_header->has(table_column))
        {
            ColumnWithTypeAndName column;
            column.name = table_column;
            column.type = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());
            column.column = column.type->createColumnConst(0, Field(table_name));

            auto adding_column_dag = ActionsDAG::makeAddingColumnActions(std::move(column));
            auto expression_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(adding_column_dag));
            child.plan.addStep(std::move(expression_step));
            plan_header = child.plan.getCurrentHeader();
        }
    }
    else
    {
        if (has_database_virtual_column && common_header->has("_database") && !plan_header->has("_database"))
        {
            ColumnWithTypeAndName column;
            column.name = "_database";
            column.type = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());
            column.column = column.type->createColumnConst(0, Field(database_name));

            auto adding_column_dag = ActionsDAG::makeAddingColumnActions(std::move(column));
            auto expression_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(adding_column_dag));
            child.plan.addStep(std::move(expression_step));
            plan_header = child.plan.getCurrentHeader();
        }

        if (has_table_virtual_column && common_header->has("_table") && !plan_header->has("_table"))
        {
            ColumnWithTypeAndName column;
            column.name = "_table";
            column.type = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());
            column.column = column.type->createColumnConst(0, Field(table_name));

            auto adding_column_dag = ActionsDAG::makeAddingColumnActions(std::move(column));
            auto expression_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(adding_column_dag));
            child.plan.addStep(std::move(expression_step));
            plan_header = child.plan.getCurrentHeader();
        }
    }
}

QueryPipelineBuilderPtr ReadFromMerge::buildPipeline(
    ChildPlan & child,
    QueryProcessingStage::Enum processed_stage) const
{
    if (!child.plan.isInitialized())
        return nullptr;

    QueryPlanOptimizationSettings optimization_settings(context);
    /// All optimisations will be done at plans creation
    optimization_settings.optimize_plan = false;
    auto builder = child.plan.buildQueryPipeline(optimization_settings, BuildQueryPipelineSettings(context));

    if (!builder->initialized())
        return builder;

    if (processed_stage > child.stage
        || (context->getSettingsRef()[Setting::allow_experimental_analyzer] && processed_stage != QueryProcessingStage::FetchColumns))
    {
        /** Materialization is needed, since from distributed storage the constants come materialized.
          * If you do not do this, different types (Const and non-Const) columns will be produced in different threads,
          * And this is not allowed, since all code is based on the assumption that in the block stream all types are the same.
          */
        builder->addSimpleTransform([](const SharedHeader & stream_header) { return std::make_shared<MaterializingTransform>(stream_header); });
    }

    return builder;
}

ReadFromMerge::ChildPlan ReadFromMerge::createPlanForTable(
    const StorageSnapshotPtr & storage_snapshot_,
    SelectQueryInfo & modified_query_info,
    QueryProcessingStage::Enum processed_stage,
    UInt64 max_block_size,
    const StorageWithLockAndName & storage_with_lock,
    const Names & real_column_names_read_from_the_source_table,
    bool & is_smallest_column_requested,
    const RowPolicyDataOpt & row_policy_data_opt,
    ContextMutablePtr modified_context,
    size_t streams_num) const
{
    const auto & [database_name, storage, _, table_name] = storage_with_lock;

    auto & modified_select = modified_query_info.query->as<ASTSelectQuery &>();

    if (!InterpreterSelectQuery::isQueryWithFinal(modified_query_info) && storage->needRewriteQueryWithFinal(real_column_names_read_from_the_source_table))
    {
        /// NOTE: It may not work correctly in some cases, because query was analyzed without final.
        /// However, it's needed for Materialized...SQL and it's unlikely that someone will use it with Merge tables.
        modified_select.setFinal();
    }

    bool use_analyzer = modified_context->getSettingsRef()[Setting::allow_experimental_analyzer];

    auto storage_stage = storage->getQueryProcessingStage(modified_context,
        processed_stage,
        storage_snapshot_,
        modified_query_info);

    QueryPlan plan;

    if (processed_stage <= storage_stage)
    {
        /// If there are only virtual columns in query, we must request at least one other column.
        Names real_column_names = real_column_names_read_from_the_source_table;
        if (real_column_names.empty())
        {
            real_column_names.push_back(ExpressionActions::getSmallestColumn(storage_snapshot_->metadata->getColumns().getAllPhysical()).name);
            is_smallest_column_requested = true;
        }

        storage->read(plan,
            real_column_names,
            storage_snapshot_,
            modified_query_info,
            modified_context,
            processed_stage,
            max_block_size,
            UInt32(streams_num));

        if (!plan.isInitialized())
            return {};

        if (row_policy_data_opt)
        {
            if (auto * source_step_with_filter = dynamic_cast<SourceStepWithFilter *>((plan.getRootNode()->step.get())))
                row_policy_data_opt->addStorageFilter(source_step_with_filter);
        }
    }
    else if (processed_stage > storage_stage || use_analyzer)
    {
        /// Maximum permissible parallelism is streams_num
        modified_context->setSetting("max_threads", streams_num);
        modified_context->setSetting("max_streams_to_max_threads_ratio", 1);

        if (use_analyzer)
        {
            /// Converting query to AST because types might be different in the source table.
            /// Need to resolve types again.
            auto ast = modified_query_info.query_tree->toAST();
            InterpreterSelectQueryAnalyzer interpreter(ast,
                modified_context,
                SelectQueryOptions(processed_stage));

            auto & planner = interpreter.getPlanner();
            planner.buildQueryPlanIfNeeded();
            plan = std::move(planner).extractQueryPlan();
        }
        else
        {
            modified_select.replaceDatabaseAndTable(database_name, table_name);
            /// TODO: Find a way to support projections for StorageMerge
            InterpreterSelectQuery interpreter{modified_query_info.query,
                modified_context,
                SelectQueryOptions(processed_stage)};

            interpreter.buildQueryPlan(plan);
        }
    }

    return ChildPlan{std::move(plan), storage_stage};
}

ReadFromMerge::RowPolicyData::RowPolicyData(RowPolicyFilterPtr row_policy_filter_ptr,
    std::shared_ptr<DB::IStorage> storage,
    ContextPtr local_context)
{
    storage_metadata_snapshot = storage->getInMemoryMetadataPtr();
    auto storage_columns = storage_metadata_snapshot->getColumns();
    auto needed_columns = storage_columns.getAll();

    ASTPtr expr = row_policy_filter_ptr->expression;

    auto syntax_result = TreeRewriter(local_context).analyze(expr, needed_columns);
    auto expression_analyzer = ExpressionAnalyzer{expr, syntax_result, local_context};

    actions_dag = expression_analyzer.getActionsDAG(false /* add_aliases */, false /* project_result */);
    filter_actions = std::make_shared<ExpressionActions>(actions_dag.clone(), ExpressionActionsSettings(local_context, CompileExpressions::yes));
    const auto & required_columns = filter_actions->getRequiredColumnsWithTypes();
    const auto & sample_block_columns = filter_actions->getSampleBlock().getNamesAndTypesList();

    NamesAndTypesList added;
    NamesAndTypesList deleted;
    sample_block_columns.getDifference(required_columns, added, deleted);
    if (!deleted.empty() || added.size() != 1)
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Cannot determine row level filter; {} columns deleted, {} columns added",
            deleted.size(), added.size());
    }

    filter_column_name = added.getNames().front();
}

void ReadFromMerge::RowPolicyData::extendNames(Names & names) const
{
    boost::container::flat_set<std::string_view> names_set(names.begin(), names.end());
    NameSet added_names;

    for (const auto & req_column : filter_actions->getRequiredColumns())
    {
        if (!names_set.contains(req_column))
        {
            added_names.emplace(req_column);
        }
    }

    if (!added_names.empty())
    {
        std::copy(added_names.begin(), added_names.end(), std::back_inserter(names));
    }
}

void ReadFromMerge::RowPolicyData::addStorageFilter(SourceStepWithFilter * step) const
{
    step->addFilter(actions_dag.clone(), filter_column_name);
}

void ReadFromMerge::RowPolicyData::addFilterTransform(QueryPlan & plan) const
{
    auto filter_step = std::make_unique<FilterStep>(plan.getCurrentHeader(), actions_dag.clone(), filter_column_name, true /* remove filter column */);
    plan.addStep(std::move(filter_step));
}

StorageMerge::StorageListWithLocks ReadFromMerge::getSelectedTables(
    ContextPtr query_context,
    bool filter_by_database_virtual_column,
    bool filter_by_table_virtual_column) const
{
    const Settings & settings = query_context->getSettingsRef();
    StorageListWithLocks res;
    DatabaseTablesIterators database_table_iterators = assert_cast<StorageMerge &>(*storage_merge).getDatabaseIterators(query_context);

    std::function<bool(const String&,const String&)> table_filter;
    if (filter_actions_dag && (filter_by_database_virtual_column || filter_by_table_virtual_column))
    {
        Block sample_block = {
            ColumnWithTypeAndName(std::make_shared<DataTypeString>(), "_database"),
            ColumnWithTypeAndName(std::make_shared<DataTypeString>(), "_table")
        };
        // Extract predicate part, that could be evaluated only with _database and _table columns
        auto table_filter_dag = VirtualColumnUtils::splitFilterDagForAllowedInputs(filter_actions_dag->getOutputs().at(0), &sample_block);
        if (table_filter_dag)
        {
            auto filter_expression = VirtualColumnUtils::buildFilterExpression(std::move(*table_filter_dag), query_context);
            auto filter_column_name = filter_expression->getActionsDAG().getOutputs().at(0)->result_name;
            table_filter = [filter=std::move(filter_expression), column_name=std::move(filter_column_name)] (const auto& database_name, const auto& table_name)
            {
                MutableColumnPtr database_column = ColumnString::create();
                MutableColumnPtr table_column = ColumnString::create();
                database_column->insert(database_name);
                table_column->insert(table_name);
                Block block{
                    ColumnWithTypeAndName(std::move(database_column), std::make_shared<DataTypeString>(), "_database"),
                    ColumnWithTypeAndName(std::move(table_column), std::make_shared<DataTypeString>(), "_table")
                };
                filter->execute(block);
                // Valid only when block has exactly one row.
                return block.getByName(column_name).column->getBool(0);
            };
        }
    }

    auto access = query_context->getAccess();
    for (const auto & iterator : database_table_iterators)
    {
        auto granted_show_on_all_tables = access->isGranted(AccessType::SHOW_TABLES, iterator->databaseName());
        auto granted_select_on_all_tables = access->isGranted(AccessType::SELECT, iterator->databaseName());
        while (iterator->isValid())
        {
            StoragePtr storage = iterator->table();
            if (!storage)
                continue;

            if (storage.get() != storage_merge.get())
                if (!table_filter || table_filter(iterator->databaseName(), iterator->name()))
                    if (granted_show_on_all_tables || access->isGranted(AccessType::SHOW_TABLES, iterator->databaseName(), iterator->name()))
                    {

                        if  (!granted_select_on_all_tables)
                            access->checkAccess(AccessType::SELECT, iterator->databaseName(), iterator->name(), column_names);

                        auto table_lock = storage->lockForShare(query_context->getCurrentQueryId(), settings[Setting::lock_acquire_timeout]);
                        res.emplace_back(iterator->databaseName(), storage, std::move(table_lock), iterator->name());
                    }
            iterator->next();
        }
    }

    return res;
}

DatabaseTablesIteratorPtr StorageMerge::DatabaseNameOrRegexp::getDatabaseIterator(const String & database_name, ContextPtr local_context) const
{
    auto database = DatabaseCatalog::instance().getDatabase(database_name);

    auto table_name_match = [this, database_name](const String & table_name_) -> bool
    {
        if (source_databases_and_tables)
        {
            if (auto it = source_databases_and_tables->find(database_name); it != source_databases_and_tables->end())
                return it->second.contains(table_name_);
            return false;
        }
        return source_table_regexp->match(table_name_);
    };

    return database->getTablesIterator(local_context, table_name_match);
}

StorageMerge::DatabaseTablesIterators StorageMerge::DatabaseNameOrRegexp::getDatabaseIterators(ContextPtr local_context) const
{
    try
    {
        checkStackSize();
    }
    catch (Exception & e)
    {
        e.addMessage("while getting table iterator of Merge table. Maybe caused by two Merge tables that will endlessly try to read each other's data");
        throw;
    }

    DatabaseTablesIterators database_table_iterators;

    if (!database_is_regexp)
    {
        /// database_name argument is not a regexp
        database_table_iterators.emplace_back(getDatabaseIterator(source_database_name_or_regexp, local_context));
    }
    else
    {
        /// database_name argument is a regexp
        auto databases = DatabaseCatalog::instance().getDatabases();

        for (const auto & db : databases)
        {
            if (source_database_regexp->match(db.first))
                database_table_iterators.emplace_back(getDatabaseIterator(db.first, local_context));
        }
    }

    return database_table_iterators;
}


void StorageMerge::checkAlterIsPossible(const AlterCommands & commands, ContextPtr local_context) const
{
    std::optional<NameDependencies> name_deps{};
    for (const auto & command : commands)
    {
        if (command.type != AlterCommand::Type::ADD_COLUMN && command.type != AlterCommand::Type::MODIFY_COLUMN
            && command.type != AlterCommand::Type::DROP_COLUMN && command.type != AlterCommand::Type::COMMENT_COLUMN
            && command.type != AlterCommand::Type::COMMENT_TABLE)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Alter of type '{}' is not supported by storage {}",
                command.type, getName());

        if (command.type == AlterCommand::Type::DROP_COLUMN && !command.clear)
        {
            if (!name_deps)
                name_deps = getDependentViewsByColumn(local_context);
            const auto & deps_mv = name_deps.value()[command.column_name];
            if (!deps_mv.empty())
            {
                throw Exception(ErrorCodes::ALTER_OF_COLUMN_IS_FORBIDDEN,
                    "Trying to ALTER DROP column {} which is referenced by materialized view {}",
                    backQuoteIfNeed(command.column_name), toString(deps_mv));
            }
        }
    }
}

void StorageMerge::alter(
    const AlterCommands & params, ContextPtr local_context, AlterLockHolder &)
{
    auto table_id = getStorageID();

    StorageInMemoryMetadata storage_metadata = getInMemoryMetadata();
    params.apply(storage_metadata, local_context);
    DatabaseCatalog::instance().getDatabase(table_id.database_name)->alterTable(local_context, table_id, storage_metadata);
    setInMemoryMetadata(storage_metadata);
    setVirtuals(createVirtuals());
}

void ReadFromMerge::convertAndFilterSourceStream(
    const Block & header,
    SelectQueryInfo & modified_query_info,
    const StorageSnapshotPtr & snapshot,
    const Aliases & aliases,
    const RowPolicyDataOpt & row_policy_data_opt,
    ContextPtr local_context,
    ChildPlan & child,
    bool is_smallest_column_requested)
{
    auto before_block_header = child.plan.getCurrentHeader();

    auto pipe_columns = before_block_header->getNamesAndTypesList();

    if (local_context->getSettingsRef()[Setting::allow_experimental_analyzer])
    {
        for (const auto & alias : aliases)
        {
            pipe_columns.emplace_back(NameAndTypePair(alias.name, alias.type));

            ActionsDAG actions_dag(pipe_columns);

            QueryTreeNodePtr query_tree = buildQueryTree(alias.expression, local_context);
            query_tree->setAlias(alias.name);

            QueryAnalysisPass query_analysis_pass(modified_query_info.table_expression);
            query_analysis_pass.run(query_tree, local_context);

            ColumnNodePtrWithHashSet empty_correlated_columns_set;
            PlannerActionsVisitor actions_visitor(modified_query_info.planner_context, empty_correlated_columns_set, false /*use_column_identifier_as_action_node_name*/);
            const auto & [nodes, _] = actions_visitor.visit(actions_dag, query_tree);

            if (nodes.size() != 1)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Expected to have 1 output but got {}", nodes.size());

            actions_dag.addOrReplaceInOutputs(actions_dag.addAlias(*nodes.front(), alias.name));
            auto expression_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(actions_dag));
            child.plan.addStep(std::move(expression_step));
        }
    }
    else
    {
        for (const auto & alias : aliases)
        {
            pipe_columns.emplace_back(NameAndTypePair(alias.name, alias.type));
            ASTPtr expr = alias.expression;
            auto syntax_result = TreeRewriter(local_context).analyze(expr, pipe_columns);
            auto expression_analyzer = ExpressionAnalyzer{alias.expression, syntax_result, local_context};

            auto dag = std::make_shared<ActionsDAG>(pipe_columns);
            auto actions_dag = expression_analyzer.getActionsDAG(true, false);
            auto expression_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(actions_dag));
            child.plan.addStep(std::move(expression_step));
        }
    }

    /// This is the filter for the individual source table, that's why filtering has to be done before all structure adaptations.
    if (row_policy_data_opt)
        row_policy_data_opt->addFilterTransform(child.plan);

    /** Output headers may differ from what StorageMerge expects in some cases.
      * When the child table engine produces a query plan for the stage after FetchColumns,
      * execution names in the output header may be different.
      * The same happens with StorageDistributed, even in the case of FetchColumns.
      */

    /** Convert types of columns according to the resulting Merge table.
      * And convert column names to the expected ones.
       */
    ColumnsWithTypeAndName current_step_columns = child.plan.getCurrentHeader()->getColumnsWithTypeAndName();
    ColumnsWithTypeAndName converted_columns;
    size_t size = current_step_columns.size();
    converted_columns.reserve(current_step_columns.size());
    String smallest_column_name = ExpressionActions::getSmallestColumn(snapshot->metadata->getColumns().getAllPhysical()).name;
    for (size_t i = 0; i < size; ++i)
    {
        const auto & source_elem = current_step_columns[i];
        if (header.has(source_elem.name))
        {
            converted_columns.push_back(header.getByName(source_elem.name));
        }
        else if (is_smallest_column_requested && smallest_column_name == source_elem.name)
        {
            /// This column is unneeded in the result.
            converted_columns.push_back(source_elem);
        }
        else if (header.columns() == current_step_columns.size())
        {
            /// Virtual columns and columns read from Distributed tables (having different name but matched by position).
            converted_columns.push_back(header.getByPosition(i));
        }
        else
        {
            /// Matching by name, but some columns are unneeded.
            converted_columns.push_back(source_elem);
        }
    }

    auto convert_actions_dag = ActionsDAG::makeConvertingActions(
        current_step_columns,
        converted_columns,
        ActionsDAG::MatchColumnsMode::Position);

    auto expression_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(convert_actions_dag));
    child.plan.addStep(std::move(expression_step));

    /// Add missing columns for the resulting Merge table.
    {
        auto adding_missing_defaults_dag = addMissingDefaults(
            *child.plan.getCurrentHeader(),
            header.getNamesAndTypesList(),
            snapshot->getAllColumnsDescription(),
            local_context,
            false);

        auto adding_missing_defaults_step = std::make_unique<ExpressionStep>(child.plan.getCurrentHeader(), std::move(adding_missing_defaults_dag));
        child.plan.addStep(std::move(adding_missing_defaults_step));
    }
}

const ReadFromMerge::StorageListWithLocks & ReadFromMerge::getSelectedTables()
{
    filterTablesAndCreateChildrenPlans();
    return selected_tables;
}

bool ReadFromMerge::requestReadingInOrder(InputOrderInfoPtr order_info_)
{
    filterTablesAndCreateChildrenPlans();

    /// Disable read-in-order optimization for reverse order with final.
    /// Otherwise, it can lead to incorrect final behavior because the implementation may rely on the reading in direct order).
    if (order_info_->direction != 1 && InterpreterSelectQuery::isQueryWithFinal(query_info))
        return false;

    auto request_read_in_order = [order_info_](ReadFromMergeTree & read_from_merge_tree)
    {
        return read_from_merge_tree.requestReadingInOrder(
            order_info_->used_prefix_of_sorting_key_size, order_info_->direction, order_info_->limit, {});
    };

    bool ok = true;
    for (const auto & child_plan : *child_plans)
        if (child_plan.plan.isInitialized())
            ok &= recursivelyApplyToReadingSteps(child_plan.plan.getRootNode(), request_read_in_order);

    if (!ok)
        return false;

    order_info = order_info_;
    query_info.input_order_info = order_info;
    return true;
}

void ReadFromMerge::applyFilters(ActionDAGNodes added_filter_nodes)
{
    for (const auto & filter_info : pushed_down_filters)
        added_filter_nodes.nodes.push_back(&filter_info.actions.findInOutputs(filter_info.column_name));

    SourceStepWithFilter::applyFilters(added_filter_nodes);

    filterTablesAndCreateChildrenPlans();
}

QueryPlanRawPtrs ReadFromMerge::getChildPlans()
{
    filterTablesAndCreateChildrenPlans();

    QueryPlanRawPtrs plans;
    for (auto & child_plan : *child_plans)
        if (child_plan.plan.isInitialized())
            plans.push_back(&child_plan.plan);

    return plans;
}

IStorage::ColumnSizeByName StorageMerge::getColumnSizes() const
{
    ColumnSizeByName column_sizes;

    forEachTable([&](const auto & table)
    {
        for (const auto & [name, size] : table->getColumnSizes())
            column_sizes[name].add(size);
    });

    return column_sizes;
}


std::tuple<bool /* is_regexp */, ASTPtr> StorageMerge::evaluateDatabaseName(const ASTPtr & node, ContextPtr context_)
{
    if (const auto * func = node->as<ASTFunction>(); func && func->name == "REGEXP")
    {
        if (func->arguments->children.size() != 1)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "REGEXP in Merge ENGINE takes only one argument");

        auto * literal = func->arguments->children[0]->as<ASTLiteral>();
        if (!literal || literal->value.getType() != Field::Types::Which::String || literal->value.safeGet<String>().empty())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Argument for REGEXP in Merge ENGINE should be a non empty String Literal");

        return {true, func->arguments->children[0]};
    }

    auto ast = evaluateConstantExpressionForDatabaseName(node, context_);
    return {false, ast};
}

bool StorageMerge::supportsTrivialCountOptimization(const StorageSnapshotPtr &, ContextPtr ctx) const
{
    /// Here we actually need storage snapshot of all nested tables.
    /// But to avoid complexity pass nullptr to make more lightweight check in MergeTreeData.
    return traverseTablesUntil([&](const auto & table) { return !table->supportsTrivialCountOptimization(nullptr, ctx); }) == nullptr;
}

std::optional<UInt64> StorageMerge::totalRows(ContextPtr query_context) const
{
    return totalRowsOrBytes([&](const auto & table) { return table->totalRows(query_context); });
}

std::optional<UInt64> StorageMerge::totalBytes(ContextPtr query_context) const
{
    return totalRowsOrBytes([&](const auto & table) { return table->totalBytes(query_context); });
}

template <typename F>
std::optional<UInt64> StorageMerge::totalRowsOrBytes(F && func) const
{
    UInt64 total_rows_or_bytes = 0;
    auto first_table = traverseTablesUntil([&](const auto & table)
    {
        if (auto rows_or_bytes = func(table))
        {
            total_rows_or_bytes += *rows_or_bytes;
            return false;
        }
        return true;
    });

    return first_table ? std::nullopt : std::make_optional(total_rows_or_bytes);
}

void registerStorageMerge(StorageFactory & factory)
{
    factory.registerStorage("Merge", [](const StorageFactory::Arguments & args)
    {
        /** In query, the name of database is specified as table engine argument which contains source tables,
          *  as well as regex for source-table names.
          */

        ASTs & engine_args = args.engine_args;

        if (engine_args.size() != 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                            "Storage Merge requires exactly 2 parameters - name "
                            "of source database and regexp for table names.");

        auto [is_regexp, database_ast] = StorageMerge::evaluateDatabaseName(engine_args[0], args.getLocalContext());

        if (!is_regexp)
            engine_args[0] = database_ast;

        String source_database_name_or_regexp = checkAndGetLiteralArgument<String>(database_ast, "database_name");

        engine_args[1] = evaluateConstantExpressionAsLiteral(engine_args[1], args.getLocalContext());
        String table_name_regexp = checkAndGetLiteralArgument<String>(engine_args[1], "table_name_regexp");

        return std::make_shared<StorageMerge>(
            args.table_id, args.columns, args.comment, source_database_name_or_regexp, is_regexp, table_name_regexp, args.getLocalContext());
    },
    {
        .supports_schema_inference = true
    });
}

}
