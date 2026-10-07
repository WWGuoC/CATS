SELECT c_count, count(*) as custdist FROM (SELECT c_custkey, count(o_orderkey) AS c_count
      FROM customer left outer join orders on c_custkey = o_custkey AND o_comment not like ?
      GROUP BY c_custkey) as c_orders GROUP BY c_count ORDER BY custdist DESC, c_count DESC;
