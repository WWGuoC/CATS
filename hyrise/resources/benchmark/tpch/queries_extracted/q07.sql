SELECT
          supp_nation,
          cust_nation,
          l_year,
          SUM(volume) as revenue
      FROM
          (SELECT
              n1.n_name as supp_nation,
              n2.n_name as cust_nation,
              SUBSTR(l_shipdate, 1, 4) as l_year,
              l_extendedprice * (1 - l_discount) as volume
          FROM
              supplier,
              lineitem,
              orders,
              customer,
              nation n1,
              nation n2
          WHERE
              s_suppkey = l_suppkey AND
              o_orderkey = l_orderkey AND
              c_custkey = o_custkey AND
              s_nationkey = n1.n_nationkey AND
              c_nationkey = n2.n_nationkey AND
              ((n1.n_name = ? AND n2.n_name = ?) OR
               (n1.n_name = ? AND n2.n_name = ?)) AND
              l_shipdate BETWEEN ? AND ?
          ) as shipping
      GROUP BY
          supp_nation, cust_nation, l_year
      ORDER BY
          supp_nation, cust_nation, l_year;
