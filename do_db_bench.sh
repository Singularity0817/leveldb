rm -f /mnt/pmem0/zwh_test/leveldb/*
./build/db_bench --benchmarks=fillrandom,readrandom --num=1000000000 --value_size=8 --reads=500000000 --db=/mnt/pmem0/zwh_test/leveldb
