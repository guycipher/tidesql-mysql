# Skip the test unless the server supports the VECTOR data type.
# TidesDB implements the vector index itself, so what this probes for is only the
# SQL-layer VECTOR column type, which older servers do not parse.  The probe is a
# real CREATE rather than a version check, so it stays correct on every server.
--disable_query_log
--disable_warnings
# 4161 is the unknown-data-type error on servers that spell it ER_UNKNOWN_DATA_TYPE;
# it is given numerically because naming an error the running server does not define
# is itself a test failure.
--error 0,4161,ER_PARSE_ERROR,ER_NOT_SUPPORTED_YET
CREATE TEMPORARY TABLE tdb_vector_probe (v VECTOR(4) NOT NULL);
let $tdb_no_vector= $mysql_errno;
DROP TEMPORARY TABLE IF EXISTS tdb_vector_probe;
--enable_warnings
--enable_query_log
if ($tdb_no_vector)
{
  --skip TidesDB: this server does not support the VECTOR data type
}
