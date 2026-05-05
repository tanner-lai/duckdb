#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/storage/data_table.hpp"

using namespace duckdb;

static ART &GetPKIndex(Connection &con, const string &table_name) {
	auto &context = *con.context;
	auto &catalog = Catalog::GetCatalog(context, INVALID_CATALOG);
	auto &table = catalog.GetEntry<TableCatalogEntry>(context, DEFAULT_SCHEMA, table_name);
	auto &storage = table.GetStorage();
	auto &indexes = storage.GetDataTableInfo()->GetIndexes();
	ART *result = nullptr;
	for (auto &index : indexes.Indexes()) {
		if (index.IsPrimary() && index.IsBound()) {
			result = &index.Cast<ART>();
			break;
		}
	}
	REQUIRE(result);
	return *result;
}

TEST_CASE("SearchEqualBatch - basic lookups", "[art]") {
	DuckDB db(nullptr);
	Connection con(db);

	// Create table with PK and insert 100 rows
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(id INTEGER PRIMARY KEY, val INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t SELECT i, i * 10 FROM range(100) tbl(i)"));

	con.BeginTransaction();
	auto &art = GetPKIndex(con, "t");

	SECTION("finds existing keys") {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		auto data = FlatVector::GetData<int32_t>(input.data[0]);
		// Look up keys 10, 20, 30
		data[0] = 10;
		data[1] = 20;
		data[2] = 30;
		input.SetCardinality(3);

		duckdb::vector<row_t> row_ids;
		art.SearchEqualBatch(input, row_ids);

		REQUIRE(row_ids.size() == 3);
		// row_ids should correspond to the inserted row positions
		// For a fresh table with sequential inserts, row_id == pk value
		set<row_t> found(row_ids.begin(), row_ids.end());
		REQUIRE(found.count(10) == 1);
		REQUIRE(found.count(20) == 1);
		REQUIRE(found.count(30) == 1);
	}

	SECTION("missing keys are skipped") {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		auto data = FlatVector::GetData<int32_t>(input.data[0]);
		// 5 exists, 999 does not, 50 exists
		data[0] = 5;
		data[1] = 999;
		data[2] = 50;
		input.SetCardinality(3);

		duckdb::vector<row_t> row_ids;
		art.SearchEqualBatch(input, row_ids);

		REQUIRE(row_ids.size() == 2);
		set<row_t> found(row_ids.begin(), row_ids.end());
		REQUIRE(found.count(5) == 1);
		REQUIRE(found.count(50) == 1);
	}

	SECTION("empty input returns empty results") {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		input.SetCardinality(0);

		duckdb::vector<row_t> row_ids;
		art.SearchEqualBatch(input, row_ids);

		REQUIRE(row_ids.empty());
	}

	SECTION("all keys missing returns empty results") {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		auto data = FlatVector::GetData<int32_t>(input.data[0]);
		data[0] = 200;
		data[1] = 300;
		input.SetCardinality(2);

		duckdb::vector<row_t> row_ids;
		art.SearchEqualBatch(input, row_ids);

		REQUIRE(row_ids.empty());
	}

	SECTION("full vector size batch") {
		DataChunk input;
		input.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		auto data = FlatVector::GetData<int32_t>(input.data[0]);
		// Fill with keys 0..99 (all exist)
		idx_t count = MinValue<idx_t>(100, STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < count; i++) {
			data[i] = static_cast<int32_t>(i);
		}
		input.SetCardinality(count);

		duckdb::vector<row_t> row_ids;
		art.SearchEqualBatch(input, row_ids);

		REQUIRE(row_ids.size() == count);
	}
}

TEST_CASE("SearchEqualBatch - BIGINT keys", "[art]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_big(id BIGINT PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_big SELECT i FROM range(5000) tbl(i)"));

	con.BeginTransaction();
	auto &art = GetPKIndex(con, "t_big");

	DataChunk input;
	input.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	auto data = FlatVector::GetData<int64_t>(input.data[0]);

	// Lookup a batch of 2048 keys (full STANDARD_VECTOR_SIZE)
	idx_t count = MinValue<idx_t>(2048, STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < count; i++) {
		data[i] = static_cast<int64_t>(i * 2); // even numbers 0,2,4,...,4094
	}
	input.SetCardinality(count);

	duckdb::vector<row_t> row_ids;
	art.SearchEqualBatch(input, row_ids);

	REQUIRE(row_ids.size() == count);
}

TEST_CASE("SearchEqualBatch - VARCHAR keys", "[art]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_str(id VARCHAR PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_str VALUES ('alpha'), ('beta'), ('gamma'), ('delta'), "
	                           "('a_very_long_string_that_exceeds_inline_storage_for_testing_purposes')"));

	con.BeginTransaction();
	auto &art = GetPKIndex(con, "t_str");

	DataChunk input;
	input.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});

	// Look up 3 keys: 2 exist, 1 doesn't
	input.data[0].SetValue(0, Value("beta"));
	input.data[0].SetValue(1, Value("missing"));
	input.data[0].SetValue(2, Value("a_very_long_string_that_exceeds_inline_storage_for_testing_purposes"));
	input.SetCardinality(3);

	duckdb::vector<row_t> row_ids;
	art.SearchEqualBatch(input, row_ids);

	REQUIRE(row_ids.size() == 2);
}

TEST_CASE("SearchEqualBatch - composite key", "[art]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_comp(a INTEGER, b INTEGER, PRIMARY KEY(a, b))"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_comp VALUES (1, 10), (1, 20), (2, 10), (2, 20), (3, 30)"));

	con.BeginTransaction();
	auto &art = GetPKIndex(con, "t_comp");

	DataChunk input;
	input.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER, LogicalType::INTEGER});
	auto col_a = FlatVector::GetData<int32_t>(input.data[0]);
	auto col_b = FlatVector::GetData<int32_t>(input.data[1]);

	// (1,10) exists, (1,30) doesn't, (2,20) exists, (9,9) doesn't
	col_a[0] = 1; col_b[0] = 10;
	col_a[1] = 1; col_b[1] = 30;
	col_a[2] = 2; col_b[2] = 20;
	col_a[3] = 9; col_b[3] = 9;
	input.SetCardinality(4);

	duckdb::vector<row_t> row_ids;
	art.SearchEqualBatch(input, row_ids);

	REQUIRE(row_ids.size() == 2);
}
