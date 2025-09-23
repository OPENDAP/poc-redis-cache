#!/usr/bin/env python3
#
# The most basic test of Redis in a container.
# jhrg 9/23/25

import redis

r = redis.Redis(host="localhost", port=6379, db=0)
print(r.ping())  # should print True if a Redis server is running
