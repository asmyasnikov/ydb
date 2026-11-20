# Native frontend benchmark

This executable compares request formats from serialized request bytes to the
logical roots consumed by KQP’s new RBO. The text path uses KQP's YQL parser. The AST, CST and script
paths import their own representations into an owning `TScript`. All four
routes use the same KQP name resolution, type annotation and RBO lowering.
The `native_query` path validates and imports the bound logical plan directly.
The `logical_plan` path resolves table and column names against the catalog,
checks each IU's declared type at its definition and imports the graph through
the same operator reader. Its timer includes this conversion and validation.

```text
Decode ExecuteQueryRequest
  text        → YQL lexer/parser → ANTLR CST ──────────────────┐
  antlr_ast   → restore ANTLR CST from grammar tree and tokens ├→ BuildNewYqlAst → TScript
  antlr_cst   → restore ANTLR CST from nodes and tokens ──────┘
  script      → restore TScript
  TScript     → ResolveNames → AnnotateAst → LowerToRbo → wrap logical roots ─┐
  native_query → ImportNativeQuery → check bindings against the catalog ─────┤
  logical_plan → ImportLogicalPlan → bind reads and validate typed graph ─────┴→ TNativeQuery
TNativeQuery → create runner → PrepareNativeQuery → DoTransform → assign Roots → ComputeParents → stop before if (Roots.size() > 1)
```

This target links the semantic frontend and RBO and has a larger build graph
than the syntax benchmark.

```sh
./ya make --threads=5 --build relwithdebinfo ydb/tests/tools/parsed_query_bench/native
bench=ydb/tests/tools/parsed_query_bench/native/parsed_query_native_bench
"$bench" --self-test
./ya make --threads=5 --build relwithdebinfo -tA ydb/tests/tools/parsed_query_bench/native/ut
```

## Prepared requests

Regenerate inputs written with the older prototype, which used different stage numbers.

```sh
"$bench" --check-request REQUEST.pb
"$bench" --client-ready 30 300 REQUEST.pb...
```

`--check-request` reports the selected route and ordered result columns after
reaching the RBO roots. Missing, unsupported or malformed stages fail. Verification
and benchmark rounds require each input to reach the roots through its declared
stage. Use `--client-ready` for client serialization when the structured
representation is already available.

For a separately constructed read-only LogicalPlan, verify and time two complete
requests with matching execution envelopes.

```sh
"$bench" --catalog catalog.json --verify-import text.pb logical.pb
"$bench" --catalog catalog.json --bench-import 30 300 text.pb logical.pb
```

The comparison follows bound column values through maps. It compares read
paths, join kinds and keys, filters, aggregates, result order, labels and YQL
types at the RBO roots. The supported profile has one public result per
statement, unordered output, unit rows, scans, maps, filters, equijoins,
COUNT and single-consumer replicate ports. Other destinations and operators,
including writes and materializations, fail explicitly.

## Six formats without a catalog

```sh
dir=$(mktemp -d)
printf '%s\n' 'SELECT 1;' > "$dir/query.yql"
set -- "$dir/text.pb" "$dir/ast.pb" "$dir/cst.pb" "$dir/script.pb" "$dir/native.pb" "$dir/logical.pb"
"$bench" --export "$dir/query.yql" "$@"
"$bench" --verify "$@"
"$bench" --bench 30 300 "$@" > "$dir/frontend.csv"
"$bench" --client 30 300 "$dir/query.yql" text antlr_ast antlr_cst script native_query logical_plan > "$dir/client.csv"
```

The native codec covers read-only plans built from a singleton source,
projection and filtering. It preserves native row lambdas, column
bindings, result order and Boolean and 32-bit and 64-bit integer literals.
The direct import rejects writes, physical properties and
unsupported expressions or operators. The smoke tests check
logical equivalence for literal selects, filters and multiple results, as well
as argument ownership and rejection of malformed plans.

## Stock text control

```sh
"$bench" --host-text 30 300 "$dir/text.pb" > "$dir/host-text.csv"
```

With a catalog, pass `--catalog catalog.json` before `--host-text`. This control
decodes the same request and calls `IKqpHost::PrepareGenericQuery` with its SQL
and parameter types. Each sample starts after creating a fresh host session in
a lightweight actor runtime with cached fixture metadata and block execution
disabled. The stock path includes host translation, parameter preparation and
the runner through the same RBO stop. The control requires the NewParser path. An untimed preflight compares the captured roots with the offline
text path, computes the public result columns from the stock RBO graph and
writes their ordered names and YQL types to stderr as `BENCH_RESULT_COLUMNS`.
The CSV reports decode and total wall and thread CPU time.

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

Before measuring, the five-way check compares the roots captured inside RBO
and their ordered result types against the text request. In this example `parent` is optional. To check rejection,
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
frontend. `native_query_*_ns` is cumulative through construction or import
of `TNativeQuery`. The offline read-only frontend wraps lowered roots and
rebinds their expressions in the same way as the stock host.
The host also prepares runtime parameter declarations for execution.
`runner_*_ns` covers runner creation, `PrepareNativeQuery`
(including result checks, block-engine configuration and pipeline setup),
and advancing that pipeline to `TKqpNewRBOTransformer::DoTransform`.
`total_*_ns` ends inside this method after it assigns `Roots` and computes
their parents, immediately before `if (Roots.size() > 1)`. The capture stops
the pipeline before statistics or optimizer rules run. The session and its
read-only table storage metadata are loaded outside the timer. Both benchmark
executables disable block execution and set the YQL log level to warning. `--client`
measures producer CPU and wall time starting from rendered YQL and ending
at full request serialization, using the same warmup and rotation.

These are offline frontend measurements. The process uses a read-only
schema fixture or snapshot, with no RPC, live table lookup, authorization,
compilation cache, optimizer, physical plan or execution. Result and effect
comparisons require a separate execution run.
