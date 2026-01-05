
def configure(conf, env):
    print("Configuring rocks storage engine module")
    # Use local RocksDB source instead of system library
    env.Append(CPPPATH=['#/src/third_party/rocksdb/include'])
    if not conf.CheckCXXHeader("rocksdb/db.h"):
        print("Could not find <rocksdb/db.h>, required for RocksDB storage engine build.")
        env.Exit(1)
