# ParsedQuery benchmark

Compare the text and structured inputs to the YQL frontend from
[ydb-platform/ydb#58587](https://github.com/ydb-platform/ydb/pull/58587).
The wire prototype preserves the fields of `ExecuteQueryRequest`. Field 4
carries YQL text. Field 5 carries a `ParsedQuery`. Both branches import
`antlr_ast` (1) with the same generated grammar and token schema. NewParser also
supports `antlr_cst` (2), `script` (3) and `native_query` (4). The main branch
also supports `yql_ast` (5) and `kql_query_list` (6). The experimental
request descriptor lives in the benchmark so that the existing Query Service
and SDKs continue to use the published descriptor.

## Five-format benchmark

The [native executable](native) reads the complete serialized requests and
compares them at the same logical plan boundary. All five routes finish with
a KQP `TNativeQuery` and the same result contract. The benchmark verifies plan
equivalence before the timed rounds, and measures client preparation separately.
Build the native target directly.

```sh
./ya make --build relwithdebinfo ydb/tests/tools/parsed_query_bench/native
```

## Syntax benchmark

Build the executable and run the round-trip tests.

```sh
./ya make --build relwithdebinfo ydb/tests/tools/parsed_query_bench
./ya make --build relwithdebinfo -tA ydb/tests/tools/parsed_query_bench/ut
```

Prepare a binary request for each syntax input. The optional final argument
to `prepare` is an existing binary request containing the session, transaction
control, parameters and other request settings. The tool replaces its query.

```sh
bench=ydb/tests/tools/parsed_query_bench/parsed_query_bench
dir=$(mktemp -d)
printf '%s\n' 'SELECT 1;' > "$dir/query.yql"
for stage in text antlr_ast antlr_cst script; do
    "$bench" prepare "$stage" "$dir/query.yql" "$dir/$stage.pb"
done
"$bench" verify "$dir/query.yql" "$dir/"*.pb
"$bench" syntax 30 300 "$dir/"*.pb > "$dir/syntax.csv"
"$bench" decode 30 300 "$dir/text.pb" > "$dir/decode.csv"
"$bench" client 30 300 "$dir/query.yql" text antlr_ast antlr_cst script > "$dir/client.csv"
```

Use `client WARMUP REPEATS QUERY.sql --template REQUEST.pb STAGE...` to
include the same parameters and settings in the client measurement.

| Mode | Start | Stop |
| --- | --- | --- |
| `syntax` | Serialized request bytes in memory | An owning KQP `TScript` has been built |
| `decode` | The same text request bytes, alternating old and experimental generated C++ decoders | Full request decoded |
| `client` | YQL text and a request template in memory | Full request serialized |

Each CSV row records the input size, iteration, monotonic wall time and thread
CPU time in nanoseconds. The syntax mode also records decode time. Warmups run
in the same process and each round rotates the first input. File reads,
verification and CSV output occur outside the timers. The client starts with
rendered YQL; rendering from a PostgreSQL FDW plan is a separate cost.

The text path uses the reference ANTLR grammars and KQP AST builder. The early
wire stages restore the parse tree from the generated protobuf AST or from CST
nodes, validate rule shapes and token order, then call that builder.
The script stage imports the owning syntax tree directly. `verify` compares
the exported trees and the complete non-query part of each request. Unsupported
constructs produce an error. Run this check before comparing inputs.

The syntax target can measure table queries without a database. Its stop point
precedes table lookup, binding, type annotation and optimization. It accepts
local benchmark inputs. The codecs cover a subset of SQL v1 and the tool
reports errors for unsupported inputs such as optimizer hints. The Script producer checks the depth and size of its
generated tree before serialization. This check is part of the client timing
and uses the same tree limits as import. The full request has a 256-level
protobuf decoding limit.
