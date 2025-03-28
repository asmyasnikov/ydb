/* syntax version 1 */
/* postgres can not */
PRAGMA EmitAggApply;

SELECT
    sum(key)
FROM (
    VALUES
        (CAST('1.51' AS Decimal (10, 3))),
        (CAST('2.22' AS Decimal (10, 3))),
        (CAST('3.49' AS Decimal (10, 3)))
) AS a (
    key
);
