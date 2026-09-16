# Skip the test unless the server supports the VECTOR data type.
#
# Neither the engine nor the TidesDB library has any notion of a vector: there is no
# vector index, no distance function and no VECTOR-specific code on either side.  A
# VECTOR column is just bytes the row codec stores and gives back, which is exactly
# what the test it guards checks.  What this probes for is therefore only whether the
# server parses the VECTOR column type at all, which older releases do not.  The probe
# is a real CREATE rather than a version check, so it stays correct on every server.
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
