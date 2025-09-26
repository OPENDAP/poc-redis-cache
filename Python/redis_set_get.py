#!/usr/bin/env python3
#
# The second most basic test of Redis in a container.
# jhrg 9/23/25

import redis

# Connect to Redis (adjust host/port if needed)
r = redis.Redis(host='localhost', port=6379, db=0)

# Set a key-value pair
r.set("foo", "bar")

# Get the value back
value = r.get("foo")
print(value.decode("utf-8"))  # Output: bar
