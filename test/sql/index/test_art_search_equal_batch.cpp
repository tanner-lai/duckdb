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
