SELECT sum(l_extendedprice*l_discount) AS revenue
      FROM lineitem
      WHERE l_shipdate >= ? AND l_shipdate < ?
      AND l_discount BETWEEN ? - 0.01 AND ? + 0.01001 AND l_quantity < ?;
