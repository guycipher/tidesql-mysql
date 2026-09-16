---
title: Installing TideSQL
description: Building the plugin against a server source tree, loading it, and the one linkage detail that decides whether it loads at all.
---

# Installing TideSQL

TideSQL is a shared-object plugin, `ha_tidesdb.so`. It links the TidesDB library, so that has to be
installed first — the plugin's build looks for `libtidesdb` and fails configuration without it.

## Building the plugin

The plugin builds as part of a server source tree, using the server's own plugin machinery. Put the
plugin sources under the tree's `storage/` directory and configure the server as you normally would:

```bash
# 1. the TidesDB library, installed where the plugin's build can find it
git clone https://github.com/tidesdb/tidesdb.git
cmake -S tidesdb -B build-tidesdb -DCMAKE_BUILD_TYPE=Release
cmake --build build-tidesdb --parallel && sudo cmake --install build-tidesdb

# 2. the plugin, inside the server tree
git clone https://github.com/tidesdb/tidesql.git
cp -r tidesql/tidesdb /path/to/mysql-server/storage/tidesdb

# 3. configure and build the server; the plugin is the `tidesdb` target
cmake -S /path/to/mysql-server -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_UNIT_TESTS=OFF
cmake --build build --target tidesdb --parallel
```

That leaves `ha_tidesdb.so` in the build's plugin output directory. Copy it into the server's
`plugin_dir`, or point `plugin_dir` at the build output while developing.

The plugin declares itself with the server's `MYSQL_ADD_PLUGIN` macro as `MODULE_ONLY`, so it is
always a loadable module and never linked into the server binary.

## Loading it

At startup, from `my.cnf`:

```ini
[mysqld]
plugin-load-add=ha_tidesdb.so
```

or into a running server:

```sql
INSTALL PLUGIN TidesDB SONAME 'ha_tidesdb.so';
```

Once loaded it appears in `SHOW ENGINES`:

```
      ENGINE: TidesDB
     SUPPORT: YES
     COMMENT: LSM B+tree engine with ACID transactions, MVCC concurrency, secondary, spatial
              and full-text indexes, vector storage, and encryption
TRANSACTIONS: YES
          XA: YES
  SAVEPOINTS: YES
```

and reports its version through the plugin table and its own status variables:

```sql
SELECT PLUGIN_VERSION FROM information_schema.PLUGINS WHERE PLUGIN_NAME = 'TidesDB';   -- 2.0
SHOW STATUS LIKE 'tidesdb_version%';   -- tidesdb_version 2.0.0, tidesdb_version_hex 131072
```

## The allocator, and why the plugin may fail to load

This is the detail worth reading before the first install, because it decides whether the plugin
loads at all.

TidesDB can be built against a non-default allocator. `jemalloc`, `mimalloc` and `tcmalloc` place
their thread-local state in the initial-exec TLS model, which needs its space reserved when the
program starts. A plugin is loaded late, with `dlopen`, well after startup, so when `libtidesdb.so`
is linked against one of those the loader has no room left to reserve and the plugin does not load:

```
[ERROR] [MY-010901] [Server] Can't open shared library 'ha_tidesdb.so'
    (errno: 0 /lib/x86_64-linux-gnu/libjemalloc.so.2: cannot allocate memory in static TLS block).
[ERROR] [MY-010736] [Server] Couldn't load plugin named 'ha_tidesdb.so' with soname 'ha_tidesdb.so'.
```

Every statement then fails with `ERROR 1286 (42000): Unknown storage engine 'TidesDB'`, which is the
symptom you are likely to see first.

The fix is to put the allocator in the process image at startup, so its TLS is reserved up front:

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 mysqld --defaults-file=/etc/my.cnf
```

Under systemd, set it in the unit rather than the shell:

```ini
[Service]
Environment=LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2
```

A library built with the default system allocator has no such requirement and loads with no
preload. Check what a given build linked against with:

```bash
ldd /usr/local/lib/libtidesdb.so | grep -E 'jemalloc|mimalloc|tcmalloc'
```

This is a property of how the library was built, not of the plugin, so rebuilding the plugin alone
does not change it.

## Where the data lives

TidesDB data files live in a directory of their own rather than among the server's tablespaces,
`tidesdb_data` beside the server's data directory by default. `tidesdb_data_home_dir` sets it
explicitly at startup. See [System Variables](/reference/system-variables).
