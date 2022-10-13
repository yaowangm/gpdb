1: CREATE TABLE vacuum_tbl (c1 int) DISTRIBUTED BY (c1);
1U: BEGIN;
1U: LOCK vacuum_tbl IN SHARE MODE;

2: VACUUM (SKIP_LOCKED) vacuum_tbl;

1U: COMMIT;
1: DROP TABLE IF EXISTS vacuum_tbl;
