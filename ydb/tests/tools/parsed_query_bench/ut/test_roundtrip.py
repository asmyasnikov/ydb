import csv
import io
import subprocess

import pytest
import yatest.common


TOOL = "ydb/tests/tools/parsed_query_bench/parsed_query_bench"
STAGES = ("text", "antlr_ast", "antlr_cst", "script")


@pytest.mark.parametrize("sql", [
    "SELECT 1;",
    "DECLARE $x AS Int64; SELECT a.k, count(*) FROM a JOIN b ON a.k = b.k "
    "WHERE b.v > $x GROUP BY a.k ORDER BY a.k;",
    "UPSERT INTO t (k, v) VALUES (1, 'v');",
    '--!syntax_v1\n--!ansi_lexer\nSELECT 1 AS "кириллица";',
    "SELECT 1 WHERE " + " AND ".join(["1=1"] * 50) + ";",
    "SELECT sum(v) OVER w AS direct_sum, sum(v) OVER (w) AS inherited_sum, "
    "sum(v) OVER (base ORDER BY v ROWS BETWEEN 2 PRECEDING AND 1 FOLLOWING EXCLUDE TIES) AS shifted_sum "
    "FROM t WINDOW base AS (PARTITION COMPACT BY k), w AS (base ORDER BY v DESC);",
    "SELECT sum(v) OVER (ORDER BY k RANGE UNBOUNDED PRECEDING) AS running_total, "
    "count(*) OVER (ORDER BY k GROUPS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING EXCLUDE NO OTHERS) "
    "AS group_size FROM t;",
])
def test_wire_to_owning_script(sql, tmp_path):
    tool = yatest.common.binary_path(TOOL)
    source = tmp_path / "query.yql"
    source.write_text(sql, encoding="utf-8")
    inputs = []
    for stage in STAGES:
        target = tmp_path / (stage + ".pb")
        subprocess.run([tool, "prepare", stage, source, target], check=True)
        inputs.append(target)
    subprocess.run([tool, "verify", source, *inputs], check=True)
    rows = subprocess.check_output([tool, "syntax", "1", "2", *inputs], text=True)
    assert {(row["stage"], row["iteration"]) for row in csv.DictReader(io.StringIO(rows))} == {
        (stage, str(i)) for stage in STAGES for i in range(2)
    }
    rows = subprocess.check_output([tool, "decode", "0", "1", inputs[0]], text=True)
    assert {row["stage"] for row in csv.DictReader(io.StringIO(rows))} == {"old", "new"}


def test_script_rejects_unsupported_depth(tmp_path):
    tool = yatest.common.binary_path(TOOL)
    source = tmp_path / "query.yql"
    source.write_text("SELECT 1 WHERE " + " AND ".join(["1=1"] * 300) + ";", encoding="utf-8")
    result = subprocess.run(
        [tool, "prepare", "script", source, tmp_path / "script.pb"],
        capture_output=True, text=True,
    )
    assert result.returncode == 1
    assert "SyntaxScript AST depth or size limit exceeded" in result.stderr
