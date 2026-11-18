import csv
import io
import json
import subprocess

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
    stages = ("text", "antlr_ast", "antlr_cst", "script", "native_query")
    inputs = [tmp_path / (stage + ".pb") for stage in stages]
    subprocess.run([tool, "--catalog", catalog, "--template", template, "--export", sql, *inputs], check=True)
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
            assert sum(phases) == int(row["total_" + clock])
            if row["stage"] == "native_query":
                assert int(row["frontend_" + clock]) == 0
