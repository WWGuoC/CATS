SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) as value FROM partsupp, supplier, nation
      WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = ?
      GROUP BY ps_partkey having SUM(ps_supplycost * ps_availqty) > (
      SELECT SUM(ps_supplycost * ps_availqty) * ? FROM partsupp, supplier, nation
      WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = ?) ORDER BY value DESC;
