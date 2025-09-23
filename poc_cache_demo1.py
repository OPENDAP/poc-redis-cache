#!/usr/bin/env python3
#
# A very simple test of the POC code

from redis_poc_cache import RedisFileCache, CacheBusyError

cache = RedisFileCache(cache_dir="/tmp/poc-cache", redis_url="redis://localhost:6379/0")


# Writer (create-only)
def test_writer():
    try:
        cache.write_bytes_create("greeting.txt", b"hello world\n")
        print("Created greeting.txt")
    except FileExistsError:
        print("File already exists")
    except CacheBusyError as e:
        print("Cache busy:", e)


# Reader (multiple allowed)
def test_reader():
    try:
        with cache.open_for_read("greeting.txt") as f:
            print("Content:", f.read())
    except FileNotFoundError:
        print("Not found")
    except CacheBusyError:
        print("Currently being written; try later")


# Press the green button in the gutter to run the script.
if __name__ == '__main__':
    print(f"Starting simple test")
    test_writer()
    test_reader()
    print(f"Simple test complete")

