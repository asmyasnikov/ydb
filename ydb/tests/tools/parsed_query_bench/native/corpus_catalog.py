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
        columns.append(dict(name=name, id=len(columns) + 1, type=type_name, not_null=bool(not_null),
                            generated=False, has_default=False))
    if not keys or not set(keys).issubset(column["name"] for column in columns):
        raise ValueError(f"Invalid primary key in {path}")
    return dict(path=path, owner_id=1, table_id=table_id, schema_version=1, storage=storage,
                has_write_indexes=False, key_columns=keys, columns=columns)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workload", choices=SCHEMAS)
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="YDB checkout root (default: current directory)")
    args = parser.parse_args()
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
