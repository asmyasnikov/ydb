import csv
import io
import json
import subprocess

import pytest
import yatest.common
from ydb.public.api.protos import ydb_query_pb2, ydb_value_pb2


TOOL = "ydb/tests/tools/parsed_query_bench/native/parsed_query_native_bench"


def test_native_roundtrip():
    subprocess.run([yatest.common.binary_path(TOOL), "--self-test"], check=True)


def test_measurement_boundary(tmp_path):
    tool = yatest.common.binary_path(TOOL)
    sql = tmp_path / "query.sql"
    query = "DECLARE $id AS Int32; SELECT payload FROM items WHERE id = $id;"
    sql.write_text(query)
    catalog = tmp_path / "catalog.json"
    catalog.write_text(
        json.dumps(
            {
                "format_version": 1,
                "identity": "fixture",
                "cluster": "benchmark",
                "database": "/Root",
                "table_path_prefix": "/Root",
                "tables": [
                    {
                        "path": "/Root/items",
                        "owner_id": 1,
                        "table_id": 1,
                        "schema_version": 1,
                        "storage": "row",
                        "has_write_indexes": False,
                        "key_columns": ["id"],
                        "columns": [
                            {
                                "id": 1,
                                "name": "id",
                                "type": "Int32",
                                "not_null": True,
                                "generated": False,
                                "has_default": False,
                            },
                            {
                                "id": 2,
                                "name": "payload",
                                "type": "String",
                                "not_null": False,
                                "generated": False,
                                "has_default": False,
                            },
                        ],
                    }
                ],
            }
        )
    )
    request = ydb_query_pb2.ExecuteQueryRequest(
        exec_mode=ydb_query_pb2.EXEC_MODE_EXECUTE,
        query_content=ydb_query_pb2.QueryContent(syntax=ydb_query_pb2.SYNTAX_YQL_V1, text=query),
    )
    request.tx_control.begin_tx.serializable_read_write.SetInParent()
    request.tx_control.commit_tx = True
    parameter = request.parameters["$id"]
    parameter.type.type_id = ydb_value_pb2.Type.INT32
    parameter.value.int32_value = 1
    template = tmp_path / "template.pb"
    template.write_bytes(request.SerializeToString())
    stages = ("text", "antlr_ast", "antlr_cst", "script", "native_query", "logical_plan")
    inputs = [tmp_path / (stage + ".pb") for stage in stages]
    subprocess.run([tool, "--catalog", catalog, "--template", template, "--export", sql, *inputs], check=True)
    checked = subprocess.run(
        [tool, "--catalog", catalog, "--check-request", inputs[-2]], capture_output=True, text=True, check=True
    )
    assert "BENCH_ROUTE=native_query" in checked.stdout
    checked_schema = [line for line in checked.stderr.splitlines() if line.startswith("BENCH_RESULT_COLUMNS=")]
    stale_catalog = tmp_path / "stale.json"
    stale = json.loads(catalog.read_text())
    stale["tables"][0]["schema_version"] += 1
    stale_catalog.write_text(json.dumps(stale))
    failed = subprocess.run(
        [tool, "--catalog", stale_catalog, "--check-request", inputs[-2]], capture_output=True, text=True
    )
    assert failed.returncode == 1 and "BENCH_ROUTE=" not in failed.stdout
    assert "NativeQuery table identity or schema changed" in failed.stderr
    subprocess.run([tool, "--catalog", stale_catalog, "--verify-import", inputs[0], inputs[-1]], check=True)
    stock = subprocess.run(
        [tool, "--catalog", catalog, "--host-text", "0", "2", inputs[0]], capture_output=True, text=True, check=True
    )
    stock_rows = list(csv.DictReader(io.StringIO(stock.stdout)))
    assert [r["iteration"] for r in stock_rows] == ["0", "1"]
    for row in stock_rows:
        assert row["stage"] == "host_text" and int(row["bytes"]) == inputs[0].stat().st_size
        for clock in ("wall_ns", "cpu_ns"):
            assert int(row["total_" + clock]) >= int(row["decode_" + clock]) >= 0
    schemas = [json.loads(line.split("=", 1)[1]) for line in stock.stderr.splitlines() if line.startswith("BENCH_RESULT_COLUMNS=")]
    assert [json.loads(line.split("=", 1)[1]) for line in checked_schema] == schemas
    assert len(schemas) == 1 and len(schemas[0]) == 1 and len(schemas[0][0]) == 1
    label, field_type = schemas[0][0][0]
    assert label == "payload" and "String" in field_type
    measured = subprocess.check_output(
        [tool, "--catalog", catalog, "--bench", "0", "2", *reversed(inputs)], text=True
    )
    rows = list(csv.DictReader(io.StringIO(measured)))
    assert len(rows) == len(stages) * 2
    assert {(r["stage"], r["iteration"]) for r in rows} == {(s, str(i)) for s in stages for i in range(2)}
    for row in rows:
        for clock in ("wall_ns", "cpu_ns"):
            phases = [int(row[phase + clock]) for phase in ("decode_", "parse_or_import_", "frontend_")]
            assert all(phase >= 0 for phase in phases)
            assert sum(phases) == int(row["native_query_" + clock])
            assert int(row["runner_" + clock]) > 0
            assert sum(phases) + int(row["runner_" + clock]) == int(row["total_" + clock])
            if row["stage"] in ("native_query", "logical_plan"):
                assert int(row["frontend_" + clock]) == 0


@pytest.mark.parametrize(
    ("other_literal", "difference"), [("3", "Logical plan differs"), ("2ul", "Logical result types differ")]
)
def test_rbo_results_match_text(tmp_path, other_literal, difference):
    tool = yatest.common.binary_path(TOOL)
    stages = ("text", "antlr_ast", "antlr_cst", "script", "native_query")
    inputs = []
    for i, literal in enumerate(("2", other_literal)):
        sql = tmp_path / (str(i) + ".sql")
        sql.write_text(f"SELECT 1; SELECT {literal};")
        paths = [tmp_path / f"{i}-{name}.pb" for name in stages]
        subprocess.run([tool, "--export", sql, *paths], check=True)
        inputs.append(paths)
    result = subprocess.run(
        [tool, "--bench", "0", "1", inputs[1][-1], *inputs[0][:-1]], capture_output=True, text=True
    )
    assert result.returncode == 1
    assert difference + " for native_query" in result.stderr
    assert not result.stdout


def test_unavailable_udf(tmp_path):
    sql = tmp_path / "query.sql"
    sql.write_text("SELECT TestMissingModule::Probe(1);")
    inputs = [tmp_path / (name + ".pb") for name in ("text", "ast", "cst", "script")]
    result = subprocess.run(
        [yatest.common.binary_path(TOOL), "--export", sql, *inputs], capture_output=True, text=True
    )
    assert result.returncode == 1
    assert "testmissingmodule" in result.stderr.lower()


def test_logical_import_rejects_different_literal(tmp_path):
    tool = yatest.common.binary_path(TOOL)
    sql = tmp_path / "query.sql"
    inputs = [tmp_path / (s + ".pb") for s in ("text", "ast", "cst", "script", "native", "logical")]
    sql.write_text("SELECT 1 AS n")
    subprocess.run([tool, "--export", sql, *inputs], check=True)
    reference = tmp_path / "reference.pb"
    inputs[0].rename(reference)
    sql.write_text("SELECT 2 AS n")
    subprocess.run([tool, "--export", sql, *inputs], check=True)
    result = subprocess.run([tool, "--verify-import", reference, inputs[-1]], capture_output=True, text=True)
    assert result.returncode == 1 and "LogicalPlan differs from text" in result.stderr
