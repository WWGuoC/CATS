SELECT p_brand, p_type, p_size, count(distinct ps_suppkey) as supplier_cnt
      FROM partsupp, part WHERE p_partkey = ps_partkey AND p_brand <> ?
      AND p_type not like ? AND p_size in (?, ?, ?, ?, ?, ?, ?, ?)
      AND ps_suppkey not in (SELECT s_suppkey FROM supplier WHERE s_comment like '%Customer%Complaints%')
      GROUP BY p_brand, p_type, p_size ORDER BY supplier_cnt DESC, p_brand, p_type, p_size;
