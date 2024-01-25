-- GPDB used to define Datum as a signed type. That caused below
-- soring issues.
create table mac_tbl (id int, mac macaddr) distributed by (id);
insert into mac_tbl values
(0, '00:00:00:00:00:00'),
(0, 'ff:ff:ff:ff:ff:ff'),
(0, '11:11:11:11:11:11');
select * from mac_tbl order by mac;
create table uuid_tbl (id int, uid uuid) distributed by (id);
insert into uuid_tbl values
(0, '00000000000000000000000000000000'),
(0, 'ffffffffffffffffffffffffffffffff'),
(0, '11111111111111111111111111111111');
select * from uuid_tbl order by uid;

-- Test cases for GP's specical mksort
-- start_ignore
create language plpython3u;
-- end_ignore

-- compare the results between mksort and qsort
create or replace function gp_mksort_test() returns int as $$
# the compare utility function
def compare_results(query):
    plpy.execute("set gp_enable_mk_sort to on;")
    mksort_res = plpy.execute(query)
    plpy.execute("set gp_enable_mk_sort to off;")
    #plpy.info(plpy.execute("show gp_enable_mk_sort;")[0]["gp_enable_mk_sort"])
    qsort_res = plpy.execute(query)
    res1 = mksort_res
    res2 = qsort_res
    if res1.nrows() != res2.nrows():
        plpy.error("gp_mksort_test: query count failed")

    for i in range(res1.nrows()):
        if  res1[i]['a'] != res2[i]['a'] or \
            res1[i]['b'] != res2[i]['b'] or \
            res1[i]['c'] != res2[i]['c']:
            plpy.error("gp_mksort_test: query results failed")

# basic test: 1~13 rows
for i in range(13):
    plpy.execute("truncate gp_mksort_test_table;")
    insert = "insert into gp_mksort_test_table \
        select floor(random()*10), floor(random()*100), left(md5(g::text),4) \
        from generate_series(1,%d) g;" % (i+1)
    plpy.execute(insert)
    compare_results("select * from gp_mksort_test_table order by a,c")
plpy.info('gp_mksort_test: basic test passed')

# random test: run 10 times (10w rows)
for i in range(10):
    plpy.execute("truncate gp_mksort_test_table;")
    insert = "insert into gp_mksort_test_table \
        select floor(random()*10), floor(random()*100), left(md5(g::text),4) \
        from generate_series(1,100000) g;"
    plpy.execute(insert)
    compare_results("select * from gp_mksort_test_table order by a,b,c")
plpy.info('gp_mksort_test: random test passed')

# table with abbr keys test

# insert data with abbr keys (uuid)
# abbr keys of uuid are generated from the first `sizeof(Datum)` bytes of uuid data
# (see uuid_abbrev_convert()), so two uuids with only different tailed values should
# have same abbr keys but different "full" datum.
plpy.execute("truncate abbr_tbl;")
plpy.execute("insert into abbr_tbl values (generate_series(1,100), 'aaaaaaaaaaaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbb');")
plpy.execute("update abbr_tbl set b = 'aaaaaaaaaaaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbb' || (a % 7)::text;")
plpy.execute("update abbr_tbl set c = ('fffffffffffffffffffffffffffffff' || (a % 5)::text)::uuid where a % 4 = 0;")
plpy.execute("update abbr_tbl set c = ('0000000000000000000000000000000' || (a % 5)::text)::uuid where a % 4 = 1;")
plpy.execute("update abbr_tbl set c = ('1111111111111111111111111111111' || (a % 5)::text)::uuid where a % 4 = 2;")
plpy.execute("update abbr_tbl set c = null where a % 4 = 3;")

query1 = "select c, b, a from abbr_tbl order by c, b, a;"
compare_results(query1)
query1 = "select c, b, a from abbr_tbl order by c desc, b, a;"
compare_results(query1)
query1 = "select c, b, a from abbr_tbl order by c, b desc, a;"
compare_results(query1)
query1 = "select c, b, a from abbr_tbl order by c nulls first, b desc, a;"
compare_results(query1)
query1 = "select c, b, a from abbr_tbl order by c nulls last, b desc, a;"
compare_results(query1)

# CREATE INDEX will cover the scenario of sort IndexTuple
plpy.execute("drop index if exists idx_abbr_tbl;")
plpy.execute("create index idx_abbr_tbl on abbr_tbl(c desc, b, a);")
plpy.execute("analyze abbr_tbl;")
query1 = "select c, b, a from abbr_tbl where c = 'ffffffff-ffff-ffff-ffff-fffffffffff3' and b = 'aaaaaaaaaaaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbb1' and a = 8;"
compare_results(query1)

# Uniqueness check of CREATE INDEX

plpy.execute("drop index if exists idx_abbr_tbl;")

# insert a duplicated row with null
plpy.execute("insert into abbr_tbl (a, b, c) values (3, 'aaaaaaaaaaaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbb3', null);")
# should succeed because uniquess check is not applicable for rows with null
plpy.execute("create unique index idx_abbr_tbl on abbr_tbl(c desc, b, a);")

plpy.execute("drop index if exists idx_abbr_tbl;")

# insert a duplicated row without null
plpy.execute("insert into abbr_tbl (a, b, c) values (1, 'aaaaaaaaaaaaaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbb1', '00000000-0000-0000-0000-000000000001');")
# should fail because of duplicated rows
try:
  plpy.execute("create unique index idx_abbr_tbl on abbr_tbl(c desc, b, a);")
except Exception:
  plpy.info("duplicated rows")
  pass

plpy.info('gp_mksort_test: table with abbr keys test passed')

return 0
$$ language plpython3u;

create table gp_mksort_test_table(a int, b int, c varchar);
create table abbr_tbl (a int, b varchar(100), c uuid);

set statement_mem='100MB';
select gp_mksort_test();

drop table abbr_tbl;
drop table gp_mksort_test_table;
drop function gp_mksort_test();
