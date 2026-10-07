create view revenue_view (supplier_no, total_revenue) as SELECT l_suppkey,
      SUM(l_extendedprice * (1 - l_discount)) FROM lineitem WHERE l_shipdate >= '1996-01-01'
      AND l_shipdate < '1996-04-01' GROUP BY l_suppkey;

      SELECT s_suppkey, s_name, s_address, s_phone, total_revenue FROM supplier, revenue_view
      WHERE s_suppkey = supplier_no AND total_revenue = (SELECT max(total_revenue)
      FROM revenue_view) ORDER BY s_suppkey;

      DROP VIEW revenue_view;
