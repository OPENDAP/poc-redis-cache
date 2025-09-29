# Proof of concept Cluster-level file cache using Redis for locking

This repo holds code to demonstrate using Redis as a lock manager for 
a cache shared by a cluster of machines in AWS. There maybe other tools
better suited to this, and we might want to look into that. However. this
will work with our existing AWS deployment. jhrg 9//29/25

To use this code, first get a Redis server instance running. One easy way
is to use the Redis Docker container. This can be used by both the Python
and the C++ code in this repo. 

## Get a redis Docker image and run it—run the server.

!NOTE if you run the tests--and that's the point of this repo--you will
need to clean the redis db between runs. Use ```del poc-cache:```

Run the server, but without key/values persisting:
```bash
docker run --name redis-server -d -p 6379:6379 redis
```

Run the redis server and persist key/value pairs across server start/stop cycles:
```bash
docker run --name redis-server -d -p 6379:6379 -v redis-data:/data redis redis-server --appendonly yes
```

## Run the redis client CLI using Docker
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

Other commands that are useful with 'key sets'
* TYPE key: What type is the key (this code uses a set)
* SSCAN key cursor [MATCH pattern] [COUNT count]: Show some of the key's values
* SMEMBERS key: Show all the key's values
* DEL key [key ...]: Delete a key and its value(s)

You don't have to use upper case...
```bash
127.0.0.1:6379> type poc-cache:keys:set
set
```

## Python code and C++ code

See the subdirectories for those.