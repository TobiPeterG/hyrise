#include "sqlite_testrunner.hpp"

namespace hyrise {

void SQLiteTestRunner::SetUp() {
  BaseTestWithParam<SQLiteTestRunnerParam>::SetUp();

  const auto& param = GetParam();
  const auto encoding_type = std::get<1>(param);


  Hyrise::get().topology.use_numa_topology();
  Hyrise::get().set_scheduler(std::make_shared<NodeQueueScheduler>());

  _sqlite = std::make_unique<SQLiteWrapper>();

  _lqp_cache = std::make_shared<SQLLogicalPlanCache>();
  _pqp_cache = std::make_shared<SQLPhysicalPlanCache>();

  auto table_cache = TableCache{};

  std::ifstream file("resources/test_data/sqlite_testrunner.tables");
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }

    std::vector<std::string> args;
    boost::algorithm::split(args, line, boost::is_space());

    if (args.size() != 2) {
      continue;
    }

    const auto& table_file = args.at(0);
    const auto& table_name = args.at(1);

    auto table = load_table(table_file, CHUNK_SIZE);

    ChunkEncodingSpec chunk_encoding_spec{};
    if (encoding_type != EncodingType::Unencoded) {
      chunk_encoding_spec = create_compatible_chunk_encoding_spec(*table, SegmentEncodingSpec{encoding_type});
      ChunkEncoder::encode_all_chunks(table, chunk_encoding_spec);
    }

    table_cache.emplace(table_name, TableCacheEntry{table, table_file, chunk_encoding_spec, false});

    Hyrise::get().storage_manager.add_table(table_name, table);

    _sqlite->create_sqlite_table(*table, table_name);
    _sqlite->create_sqlite_table(*table, table_name + _master_table_suffix);
  }

  _table_cache_per_encoding.emplace(encoding_type, std::move(table_cache));
}

void SQLiteTestRunner::TearDown() {
  if (!_table_cache_per_encoding.empty()) {
    const auto encoding_type = std::get<1>(GetParam());
    auto it = _table_cache_per_encoding.find(encoding_type);
    if (it != _table_cache_per_encoding.end()) {
      for (const auto& [table_name, _] : it->second) {
        if (Hyrise::get().storage_manager.has_table(table_name)) {
          Hyrise::get().storage_manager.drop_table(table_name);
        }
      }
    }
  }

  _table_cache_per_encoding.clear();
  _lqp_cache.reset();
  _pqp_cache.reset();
  _sqlite.reset();

  BaseTestWithParam<SQLiteTestRunnerParam>::TearDown();
}

std::vector<std::pair<size_t, std::string>> SQLiteTestRunner::queries() {
  static std::vector<std::pair<size_t, std::string>> queries;

  if (!queries.empty()) {
    return queries;
  }

  std::ifstream file("resources/test_data/sqlite_testrunner_queries.sql");
  std::string query;

  auto next_line = size_t{0};  // Incremented before first use
  while (std::getline(file, query)) {
    ++next_line;
    if (query.empty() || query.substr(0, 2) == "--") {
      continue;
    }

    queries.emplace_back(next_line, std::move(query));
  }

  return queries;
}

TEST_P(SQLiteTestRunner, CompareToSQLite) {
  _last_run_successful = false;

  const auto [query_pair, encoding_type] = GetParam();
  const auto& [line, sql] = query_pair;

  SCOPED_TRACE("Query '" + sql + "' from line " + std::to_string(line) + " with encoding " +
               std::string{magic_enum::enum_name(encoding_type)});

  {
    auto sql_pipeline = SQLPipelineBuilder{sql}.create_pipeline();

    // Execute query in Hyrise and SQLite
    const auto [pipeline_status, result_table] = sql_pipeline.get_result_table();
    ASSERT_EQ(pipeline_status, SQLPipelineStatus::Success);
    const auto sqlite_result_table = _sqlite->main_connection.execute_query(sql);

    ASSERT_TRUE(result_table && result_table->row_count() > 0 && sqlite_result_table &&
                sqlite_result_table->row_count() > 0)
        << "The SQLiteTestRunner cannot handle queries without results. We can only infer column types from sqlite if "
           "they have at least one row";

    auto order_sensitivity = OrderSensitivity::No;
    const auto& parse_result = sql_pipeline.get_parsed_sql_statements().back();
    if (parse_result->getStatements().front()->is(hsql::kStmtSelect)) {
      auto select_statement = dynamic_cast<const hsql::SelectStatement*>(parse_result->getStatements().back());
      if (select_statement->order) {
        order_sensitivity = OrderSensitivity::Yes;
      }
    }

    const auto table_comparison_msg =
        check_table_equal(result_table, sqlite_result_table, order_sensitivity, TypeCmpMode::Lenient,
                          FloatComparisonMode::RelativeDifference, IgnoreNullable::Yes);

    if (table_comparison_msg) {
      FAIL() << "Query failed: " << *table_comparison_msg << std::endl;
    }

    // Mark Tables modified by the query as dirty
    for (const auto& plan : sql_pipeline.get_optimized_logical_plans()) {
      for (const auto& table_name : lqp_find_modified_tables(plan)) {
        auto encoding_it = _table_cache_per_encoding.find(encoding_type);
        if (encoding_it == _table_cache_per_encoding.end()) {
          continue;
        }
        auto& table_cache = encoding_it->second;
        if (!table_cache.contains(table_name)) {
          continue;
        }
        table_cache.at(table_name).dirty = true;
      }
    }

    // Delete newly created views in sqlite
    for (const auto& plan : sql_pipeline.get_optimized_logical_plans()) {
      if (const auto create_view = std::dynamic_pointer_cast<CreateViewNode>(plan)) {
        _sqlite->main_connection.execute_query("DROP VIEW IF EXISTS " + create_view->view_name + ";");
      }
    }
  }

  _last_run_successful = true;
}

}  // namespace hyrise
