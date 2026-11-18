"""Write an offline catalog for a checked-in KQP benchmark schema."""
import argparse
import json
from pathlib import Path
import re
import sys


SCHEMAS = {
    "five-way": ("join/data/schema/rstuv.sql", "/Root", "row"),
    "tpcc": ("join/data/schema/tpcc.sql", "/Root/test/tpcc", "row"),
    "tpch": ("join/data/schema/tpch.sql", "/Root", "row"),
    "tpcds": ("join/data/schema/tpcds.sql", "/Root/test/ds", "row"),
    "clickbench": ("rbo/data/schema/clickbench.sql", "/Root", "column"),
}


def table_schema(statement, table_id, storage):
    match = re.fullmatch(r"\s*CREATE\s+TABLE\s*`([^`]+)`\s*\((.*)\)\s*", statement, re.I | re.S)
    if not match:
        raise ValueError(f"Unsupported CREATE TABLE in the corpus schema: {statement[:80]}")
    path, body = match.groups()
    columns = []
    keys = []
    for line in body.splitlines():
        definition = line.strip().removesuffix(",").strip()
        if not definition:
            continue
        primary_key = re.fullmatch(r"PRIMARY\s+KEY\s*\(([^)]+)\)", definition, re.I)
        if primary_key:
            keys = [name.strip().strip("`") for name in primary_key[1].split(",")]
            continue
        column = re.fullmatch(r"`?(\w+)`?\s+(\w+(?:\(\d+\s*,\s*\d+\))?)\s*(NOT\s+NULL)?", definition, re.I)
        if not column:
            raise ValueError(f"Unsupported column in {path}: {definition}")
        name, type_name, not_null = column.groups()
        if type_name.casefold() == "bytes":
            type_name = "String"
        columns.append(dict(name=name, id=len(columns) + 1, type=type_name, not_null=bool(not_null),
                            generated=False, has_default=False))
    if not keys or not set(keys).issubset(column["name"] for column in columns):
        raise ValueError(f"Invalid primary key in {path}")
    return dict(path=path, owner_id=1, table_id=table_id, schema_version=1, storage=storage,
                has_write_indexes=False, key_columns=keys, columns=columns)


def many_joins(query_path):
    # The query builder and DDL come from the KQP ManyJoinsWithDecimalRanges test.
    projections = ["t0.key"] + [f"t{i}.value AS value{i}" for i in range(1, 91)]
    query = [f"SELECT {', '.join(projections)} FROM `/Root/t0` AS t0"]
    for i in range(1, 91):
        query.append(f"LEFT JOIN (SELECT key, value FROM `/Root/t{i}` WHERE scope = CAST('0' AS Decimal(7,0))) "
                     f"AS t{i} ON t0.key = t{i}.key")
    query_path.write_text("\n".join(query) + "\n", encoding="utf-8")
    body = "scope Decimal(7,0) NOT NULL,\nkey Uint64 NOT NULL,\nvalue String,\nPRIMARY KEY (scope, key)"
    return [table_schema(f"CREATE TABLE `/Root/t{i}` (\n{body}\n)", i + 1, "row") for i in range(91)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workload", choices=[*SCHEMAS, "many-joins"])
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="YDB checkout root (default: current directory)")
    parser.add_argument("--query-out", type=Path, help="write the generated ManyJoinsWithDecimalRanges SQL")
    args = parser.parse_args()
    if args.workload == "many-joins":
        if not args.query_out:
            parser.error("many-joins requires --query-out FILE")
        prefix = "/Root"
        tables = many_joins(args.query_out)
    else:
        if args.query_out:
            parser.error("--query-out is used with many-joins; other workloads already have SQL files in the checkout")
        source, prefix, storage = SCHEMAS[args.workload]
        ddl = (args.repo / "ydb/core/kqp/ut" / source).read_text(encoding="utf-8")
        statements = [part for part in re.sub(r"--[^\n]*", "", ddl).split(";") if part.strip()]
        tables = [table_schema(statement, i, storage) for i, statement in enumerate(statements, 1)]
    catalog = dict(format_version=1, identity="fixture", cluster="benchmark", database="/Root",
                   table_path_prefix=prefix, tables=tables)
    json.dump(catalog, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
