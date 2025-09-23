# Proof of Concept python code using redis for cache control 

## Notes

### Get a redis Docker image and run it - run the server.

Run the server, but without key/values persisting:
```bash
docker run --name redis-server -d -p 6379:6379 redis
```

Run the redis server and persist key/value pairs across server start/stop cycles:
```bash
docker run --name redis-server -d -p 6379:6379 -v redis-data:/data redis redis-server --appendonly yes
```

### Run the redis client CLI using Docker
```bash
docker exec -it redis-server redis-cli
(redis) idoru-2:poc-cache-control jimg$ docker exec -it redis-server redis-cli
127.0.0.1:6379> set bar baz
OK
127.0.0.1:6379> get bar
"baz"
127.0.0.1:6379> KEYS *
1) "bar"
2) "foo"
127.0.0.1:6379> SCAN 0
1) "0"
2) 1) "bar"
   2) "foo"
127.0.0.1:6379> del foo
(integer) 1
```

### Set up a conda env for redis clients written in python; verify the environment has the redis client.

```bash
conda create -n redis -c conda-forge python=3.11 redis-py -y

conda activate redis

(redis) idoru-2:poc-cache-control jimg$ python3 -c "import redis; print(redis.__version__)"
6.4.0
```

Test the server using the ping command

```bash
(redis) idoru-2:poc-cache-control jimg$ python3 redis-ping.py 
True
```

How about set and get?

```bash
(redis) idoru-2:poc-cache-control jimg$ python3 redis-set-get.py 
bar
```

## Better tests

See test_poc_cache_mproc.py which tests the cache using N parallel processes (not threads).

```bash
python test_poc_cache_mproc.py --processes 6 --duration 30 --write-prob 0.20
```

With this test, the redis store will have to be cleaned after each run since the test uses
a redis 'set' to store the names of the files in the cache. This is how it finds a file aleady
in the cache to read. To do this, use ```del poc-cache:keys:set``` in the redis CLI.

Since the tests leave the /tmp/poc-cache directory full of stuff, that too, should be cleaned.

Here's the result of a 60 second run:

```bash
(redis) idoru-2:poc-cache-control jimg$ python test_poc_cache_mproc.py --processes 6 --duration 60 --write-prob 0.20

=== Per-process stats ===
worker-5 (pid=26312)
  iterations     : 3482
  READS  ok/busy/missing : 2801/0/0  bytes=5983593
  WRITES ok/busy/exists  : 664/0/17 bytes=1425199
  other_errors   : 0

worker-4 (pid=26311)
  iterations     : 3451
  READS  ok/busy/missing : 2743/0/0  bytes=5875720
  WRITES ok/busy/exists  : 682/0/26 bytes=1500207
  other_errors   : 0

worker-3 (pid=26310)
  iterations     : 3444
  READS  ok/busy/missing : 2736/0/0  bytes=5906553
  WRITES ok/busy/exists  : 689/0/19 bytes=1480129
  other_errors   : 0

worker-1 (pid=26308)
  iterations     : 3450
  READS  ok/busy/missing : 2741/0/0  bytes=5821898
  WRITES ok/busy/exists  : 686/0/23 bytes=1488048
  other_errors   : 0

worker-2 (pid=26309)
  iterations     : 3450
  READS  ok/busy/missing : 2740/0/2  bytes=5937817
  WRITES ok/busy/exists  : 681/0/27 bytes=1433366
  other_errors   : 0

worker-6 (pid=26313)
  iterations     : 3450
  READS  ok/busy/missing : 2739/0/0  bytes=5892904
  WRITES ok/busy/exists  : 686/0/25 bytes=1506703
  other_errors   : 0

=== Aggregate ===
aggregate (pid=0)
  iterations     : 20727
  READS  ok/busy/missing : 16500/0/2  bytes=35418485
  WRITES ok/busy/exists  : 4088/0/137 bytes=8833652
  other_errors   : 0

Note: Create-only semantics exercised (writes_exits > 0).
(redis) idoru-2:poc-cache-control jimg$ 
```