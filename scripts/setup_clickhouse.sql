-- Test fixtures for clickhouse_scanner. Loaded by scripts/test_with_clickhouse.sh (`make smoke`).
SET enable_time_time64_type = 1;

DROP DATABASE IF EXISTS test_db;
CREATE DATABASE test_db;
DROP DATABASE IF EXISTS other_db;
CREATE DATABASE other_db;

CREATE TABLE other_db.other_table (x UInt8) ENGINE = Memory;
INSERT INTO other_db.other_table VALUES (1);

CREATE TABLE test_db.t1 (
    id UInt64,
    name String,
    value Float64,
    created_at DateTime('UTC')
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.t1 VALUES
    (1, 'Alice', 99.5, '2024-01-01 00:00:00'),
    (2, 'Bob', 150.25, '2024-01-02 00:00:00'),
    (3, 'Charlie', 200.0, '2024-01-03 00:00:00');

CREATE VIEW test_db.v1 AS SELECT id, upper(name) AS name FROM test_db.t1;

CREATE TABLE test_db.empty (id UInt64) ENGINE = MergeTree ORDER BY id;

CREATE TABLE test_db.`MixedCase` (`Id` UInt8) ENGINE = Memory;
INSERT INTO test_db.`MixedCase` VALUES (7);

CREATE TABLE test_db.scalars (
    id UInt8,
    b Bool,
    i8 Int8, i16 Int16, i32 Int32, i64 Int64,
    u8 UInt8, u16 UInt16, u32 UInt32, u64 UInt64,
    i128 Int128, u128 UInt128, i256 Int256, u256 UInt256,
    f32 Float32, f64 Float64,
    d9 Decimal(9, 2), d18 Decimal(18, 4), d38 Decimal(38, 10), d76 Decimal(76, 5),
    s String, fs FixedString(3),
    d Date, d32 Date32,
    dt DateTime('UTC'), dt64_3 DateTime64(3, 'UTC'), dt64_9 DateTime64(9, 'Asia/Tokyo'),
    uuid UUID, ip4 IPv4, ip6 IPv6,
    e8 Enum8('red' = 1, 'green' = 2, 'blue' = -3), e16 Enum16('small' = 1000, 'large' = 2000)
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.scalars VALUES
    (1, true,
     -128, -32768, -2147483648, -9223372036854775808,
     255, 65535, 4294967295, 18446744073709551615,
     -170141183460469231731687303715884105728, 340282366920938463463374607431768211455, -1, 1,
     1.5, -2.25,
     1234567.89, 12345678901234.5678, 1234567890123456789012345678.0123456789, 12345.67891,
     'hello', 'abc',
     '2024-02-29', '1900-01-01',
     '2024-02-29 12:34:56', '2024-02-29 12:34:56.789', '2024-02-29 21:34:56.123456789',
     '61f0c404-5cb3-11e7-907b-a6006ad3dba0', '192.168.0.1', '2001:db8::1',
     'blue', 'large'),
    (2, false,
     0, 0, 0, 0,
     0, 0, 0, 0,
     0, 0, 0, 0,
     0, 0,
     0, 0, 0, 0,
     '', 'xyz',
     '1970-01-01', '1970-01-01',
     '1970-01-01 00:00:00', '1970-01-01 00:00:00.000', '1970-01-01 09:00:00.000000000',
     '00000000-0000-0000-0000-000000000000', '0.0.0.0', '::',
     'red', 'small');

CREATE TABLE test_db.nullables (
    id UInt8,
    i Nullable(Int32),
    s Nullable(String),
    d Nullable(Date),
    dt Nullable(DateTime('UTC')),
    e Nullable(Enum8('a' = 1)),
    lc LowCardinality(Nullable(String)),
    lcs LowCardinality(String),
    dec Nullable(Decimal(10, 2)),
    ip Nullable(IPv4)
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.nullables VALUES
    (1, NULL, NULL, NULL, NULL, NULL, NULL, 'x', NULL, NULL),
    (2, 42, 'text', '2024-01-01', '2024-01-01 00:00:00', 'a', 'lc', 'y', 3.14, '10.0.0.1');

CREATE TABLE test_db.nested (
    id UInt8,
    arr Array(Int32),
    arr_null Array(Nullable(String)),
    arr2 Array(Array(UInt8)),
    tup Tuple(a Int32, b String),
    tup_unnamed Tuple(Int32, String),
    m Map(String, UInt64),
    m_ip Map(String, IPv4),
    arr_ip Array(IPv4),
    tup_ip Tuple(ip IPv4, n Int8)
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.nested VALUES
    (1, [1, 2, 3], ['a', NULL], [[1], [2, 3]], (1, 'x'), (2, 'y'),
     {'k1': 1, 'k2': 2}, {'h': '1.2.3.4'}, ['1.1.1.1', '2.2.2.2'], ('8.8.8.8', 5)),
    (2, [], [], [], (0, ''), (0, ''), {}, {}, [], ('0.0.0.0', 0));

CREATE TABLE test_db.semi (
    id UInt8,
    j JSON,
    v Variant(String, UInt64),
    dyn Dynamic
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.semi VALUES
    (1, '{"a": 1, "b": {"c": "x"}}', 'str', 42),
    (2, '{}', 7, 'hello');

CREATE TABLE test_db.times (id UInt8, t Time, t3 Time64(3), t9 Time64(9)) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.times VALUES
    (1, '12:34:56', '12:34:56.789', '12:34:56.123456789'),
    (2, '00:00:00', '00:00:00', '00:00:00');

CREATE TABLE test_db.bad_times (t Time) ENGINE = Memory;
INSERT INTO test_db.bad_times VALUES ('-01:00:00');

CREATE TABLE test_db.aggregates (
    k UInt8,
    total AggregateFunction(sum, UInt64),
    last SimpleAggregateFunction(anyLast, String)
) ENGINE = AggregatingMergeTree ORDER BY k;
INSERT INTO test_db.aggregates SELECT 1, sumState(toUInt64(10)), 'z';

CREATE TABLE test_db.binary (id UInt8, data String) ENGINE = Memory;
INSERT INTO test_db.binary VALUES (1, unhex('FF00'));

CREATE TABLE test_db.big (n UInt64, s String) ENGINE = MergeTree ORDER BY n
AS SELECT number, toString(number) FROM numbers(10000000);
