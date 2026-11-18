# Native frontend benchmark

This executable compares request formats through construction of a logical
`TNativeQuery`. The text path uses KQP's YQL parser. The AST, CST and script
paths import their own representations into an owning `TScript`. All four
routes use the same KQP name resolution, type annotation and RBO lowering.
The `native_query` path validates and imports the bound logical plan directly.

This target links the semantic frontend and RBO and has a larger build graph
than the syntax benchmark.

```sh
./ya make --build relwithdebinfo ydb/tests/tools/parsed_query_bench/native
bench=ydb/tests/tools/parsed_query_bench/native/parsed_query_native_bench
"$bench" --self-test
./ya make --build relwithdebinfo -tA ydb/tests/tools/parsed_query_bench/native/ut
```

## Five formats without a catalog

```sh
dir=$(mktemp -d)
printf '%s\n' 'SELECT 1;' > "$dir/query.yql"
set -- "$dir/text.pb" "$dir/ast.pb" "$dir/cst.pb" "$dir/script.pb" "$dir/native.pb"
"$bench" --export "$dir/query.yql" "$@"
"$bench" --verify "$@"
"$bench" --bench 30 300 "$@" > "$dir/frontend.csv"
"$bench" --client 30 300 "$dir/query.yql" text antlr_ast antlr_cst script native_query > "$dir/client.csv"
```

The native codec covers read-only plans built from a singleton source,
projection and filtering. It preserves native row lambdas, column
bindings, result order and Boolean and 32-bit and 64-bit integer literals.
The direct import rejects writes, physical properties and
unsupported expressions or operators. The smoke tests check
logical equivalence for literal selects, filters and multiple results, as well
as argument ownership and rejection of malformed plans.

## Catalog-backed queries

Save this example as `catalog.json`.

```json
{
  "format_version": 1,
  "identity": "fixture",
  "cluster": "benchmark",
  "database": "/Root/bench",
  "table_path_prefix": "/Root/bench",
  "tables": [
    {
      "path": "/Root/bench/items",
      "owner_id": 1,
      "table_id": 1,
      "schema_version": 1,
      "storage": "row",
      "has_write_indexes": false,
      "key_columns": ["id"],
      "columns": [
        {"name": "id", "id": 1, "type": "Uint64", "not_null": true, "generated": false, "has_default": false},
        {"name": "category_id", "id": 2, "type": "Uint64", "not_null": true, "generated": false, "has_default": false}
      ]
    },
    {
      "path": "/Root/bench/categories",
      "owner_id": 1,
      "table_id": 2,
      "schema_version": 1,
      "storage": "row",
      "has_write_indexes": false,
      "key_columns": ["id"],
      "columns": [
        {"name": "id", "id": 1, "type": "Uint64", "not_null": true, "generated": false, "has_default": false},
        {"name": "parent_id", "id": 2, "type": "Uint64", "not_null": true, "generated": false, "has_default": false}
      ]
    }
  ]
}
```

```sh
dir=$(mktemp -d)
printf '%s\n' 'SELECT id FROM items;' > "$dir/query.yql"
set -- "$dir/text.pb" "$dir/ast.pb" "$dir/cst.pb" "$dir/script.pb"
"$bench" --catalog catalog.json --export "$dir/query.yql" "$@"
"$bench" --catalog catalog.json --verify "$@"
"$bench" --catalog catalog.json --bench 30 300 "$@" > "$dir/frontend.csv"
```

The sample query can also use the native table-scan codec. To compare all
five formats, repeat the commands with `"$dir/native.pb"` appended to the
four output or input paths.

To supply parameters, insert `--template template.pb` before `--export` or
`--client`. The template is a binary `ExecuteQueryRequest` containing the
typed parameters. The tool replaces its query field for each format.

Catalog mode resolves read-only `SELECT` and `DECLARE` scripts against the
supplied schema. The tool loads the catalog once and reuses that immutable
snapshot for verification, warmups and measured iterations. Parameter types
come from the request and parameter values stay in its envelope. Type
annotation and lowering check the subset supported by the reference
frontend. The tool stops on unsupported inputs. Verification compares
the request envelopes, owned syntax trees, logical plans, projected IUs
and output types. The catalog-backed native codec supports full table
scans, column projections, derived tables with a single primary Replicate
port and trees of ordinary `INNER`, `LEFT`, `LEFT ONLY` and `CROSS` joins.
Equijoins accept direct column keys with matching comparable data types, including nullable keys. A left join makes its right outputs
nullable. A left-only join exposes only the left outputs. The codec
validates table identities, schema versions and types
against the supplied catalog. Scalar `COUNT(*)` and `COUNT(DISTINCT column)`
preserve the stock RBO aggregate and its `Uint64` result type. Supported
parameter types are checked against the request envelope. Computed join keys,
residual join predicates and shared operator nodes remain outside this subset.

### Five formats for a join chain

```sh
cat > "$dir/join.yql" <<'SQL'
SELECT i.id, c.id AS category, p.id AS parent
FROM items AS i
INNER JOIN categories AS c ON i.category_id = c.id
LEFT JOIN categories AS p ON c.parent_id = p.id;
SQL
set -- "$dir/join-text.pb" "$dir/join-ast.pb" "$dir/join-cst.pb" "$dir/join-script.pb" "$dir/join-native.pb"
"$bench" --catalog catalog.json --export "$dir/join.yql" "$@"
"$bench" --catalog catalog.json --verify "$@"
"$bench" --catalog catalog.json --bench 30 300 "$@" > "$dir/join-frontend.csv"
```

The five-way check compares the logical graph and ordered result types
before measuring. In this example `parent` is optional. To check rejection,
replace `LEFT JOIN` with `FULL JOIN` in the sample SQL and repeat the five-file
export. The codec reports an unsupported join kind.

The repository's [ClickBench q1](../../../../core/kqp/ut/rbo/data/yql-clickbench/q1.yql)
can use all five formats with an offline catalog containing `/Root/hits`.
The smoke tests also cover scalar counts and reject unsupported aggregate
metadata.

## Replay the public KQP corpus

From the checkout root, use the schema and query stored in the repository.

```sh
python3 ydb/tests/tools/parsed_query_bench/native/corpus_catalog.py five-way > "$dir/catalog.json"
query=ydb/core/kqp/ut/join/data/queries/five_way_join.sql
set -- "$dir/text.pb" "$dir/ast.pb" "$dir/cst.pb" "$dir/script.pb" "$dir/native.pb"
"$bench" --catalog "$dir/catalog.json" --export "$query" "$@"
"$bench" --catalog "$dir/catalog.json" --bench 30 300 "$@" > "$dir/frontend.csv"
```

The helper also reads the repository's TPC-C, TPC-H, TPC-DS and ClickBench
schemas. It assigns fixture identifiers in DDL order. ClickBench uses column
storage, matching the KQP compilation benchmark. SQL support depends on the
operators and expressions in the selected query.

To replay the 90-join [ManyJoinsWithDecimalRanges](../../../../core/kqp/ut/rbo/physical/kqp_rbo_compatibility_ut.cpp)
workload, generate its query and the corresponding 91-table catalog and use
the same export and benchmark commands.

```sh
query="$dir/many-joins.yql"
python3 ydb/tests/tools/parsed_query_bench/native/corpus_catalog.py many-joins --query-out "$query" > "$dir/catalog.json"
```

## Measurement boundary

The input files are read once. Each warmup and measured round visits all
formats in a rotating order. The CSV records byte size and monotonic wall
time and thread CPU time in nanoseconds. The phase columns record full
request decode, parsing or stage import, and the remaining semantic
frontend. `native_query` completes at the end of import. `--client`
measures producer CPU and wall time starting from rendered YQL and ending
at full request serialization, using the same warmup and rotation.

These are offline frontend measurements. The process uses a read-only
schema fixture or snapshot, with no RPC, live table lookup, authorization,
compilation cache, optimizer, physical plan or execution. Result and effect
comparisons require a separate execution run.
