SELECT o_year, SUM(case when nation = ? then volume else 0 end) / SUM(volume) as mkt_share
     FROM (SELECT SUBSTR(o_orderdate, 1, 4) as o_year, l_extendedprice * (1-l_discount) as volume,
     n2.n_name as nation FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region
     WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey AND l_orderkey = o_orderkey AND
     o_custkey = c_custkey AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey AND
     r_name = ? AND s_nationkey = n2.n_nationkey AND o_orderdate between ?
     AND ? AND p_type = ?) as all_nations GROUP BY o_year ORDER BY o_year;
