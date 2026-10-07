SELECT o_orderpriority, count(*) as order_count FROM orders WHERE o_orderdate >= ? AND
    o_orderdate < ? AND exists (
    SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate)
    GROUP BY o_orderpriority ORDER BY o_orderpriority;
