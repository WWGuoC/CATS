SELECT l_shipmode, SUM(case when o_orderpriority ='1-URGENT' or o_orderpriority ='2-HIGH' then 1 else 0 end)
      as high_line_count, SUM(case when o_orderpriority <> '1-URGENT' AND
      o_orderpriority <> '2-HIGH' then 1 else 0 end) as low_line_count FROM orders, lineitem
      WHERE o_orderkey = l_orderkey AND l_shipmode IN (?, ?) AND l_commitdate < l_receiptdate
      AND l_shipdate < l_commitdate AND l_receiptdate >= ? AND
      l_receiptdate < ? GROUP BY l_shipmode ORDER BY l_shipmode;
