// test_RedisFileCacheLRU.cpp
// CppUnit tests for RedisFileCache (LRU + blocking).
// No main() here; integrate with your existing CppUnit runner.

// test_RedisFileCacheLRU.cpp
// CppUnit tests for RedisFileCache (LRU + blocking) with temp-dir cleaner.
// No main() here; integrate with your existing CppUnit runner.

#include "RedisFileCacheLRU.h"

#include "run_tests_cppunit.h"

#include <hiredis/hiredis.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <thread>
#include <cerrno>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

namespace {

// --- tiny hiredis helpers for test cleanup/inspect ---
struct RcCloser { void operator()(redisContext* c) const { if (c) redisFree(c); } };
using RcPtr = std::unique_ptr<redisContext, RcCloser>;

inline RcPtr rc_connect(const std::string& host, int port, int db) {
    redisContext* rc = redisConnect(host.c_str(), port);
    if (!rc || rc->err) {
        if (rc) { fprintf(stderr, "redis err: %s\n", rc->errstr); redisFree(rc); }
        return RcPtr(nullptr);
    }
    if (db != 0) {
        if (auto* r = (redisReply*)redisCommand(rc, "SELECT %d", db)) freeReplyObject(r);
    }
    // Prefer RESP2 for stable replies
    if (auto* r = (redisReply*)redisCommand(rc, "HELLO 2")) freeReplyObject(r);
    return RcPtr(rc);
}

inline void del_namespace(RcPtr& rc, const std::string& ns) {
    std::string cursor = "0";
    std::string patt = ns + ":*";
    do {
        redisReply* r = (redisReply*)redisCommand(rc.get(), "SCAN %s MATCH %b COUNT 200",
                                                  cursor.c_str(), patt.data(), (size_t)patt.size());
        if (!r) break;
        std::unique_ptr<redisReply, void(*)(void*)> G(r, freeReplyObject);
        if (r->type != REDIS_REPLY_ARRAY || r->elements < 2) break;
        cursor = (r->element[0]->type==REDIS_REPLY_STRING) ? std::string(r->element[0]->str, r->element[0]->len) : "0";
        auto* arr = r->element[1];
        for (size_t i=0; i<arr->elements; ++i) {
            if (arr->element[i]->type == REDIS_REPLY_STRING) {
                std::string k(arr->element[i]->str, arr->element[i]->len);
                if (auto* d = (redisReply*)redisCommand(rc.get(), "DEL %b", k.data(), (size_t)k.size()))
                    freeReplyObject(d);
            }
        }
    } while (cursor != "0");
}

inline bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

inline std::string rand_hex(int n=8) {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    static const char* hexd="0123456789abcdef";
    std::uniform_int_distribution<int> d(0,15);
    std::string s; s.reserve(n);
    for (int i=0;i<n;++i) s.push_back(hexd[d(gen)]);
    return s;
}

// -------- temp-dir cleaner (recursive) --------
inline int remove_path_recursive(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        return (errno == ENOENT) ? 0 : -1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return -1;
        struct dirent* de;
        while ((de = readdir(dir)) != nullptr) {
            if (std::strcmp(de->d_name, ".") == 0 || std::strcmp(de->d_name, "..") == 0) continue;
            std::string child = path + "/" + de->d_name;
            if (remove_path_recursive(child) != 0) {
                // keep going; best-effort cleanup for tests
            }
        }
        closedir(dir);
        if (rmdir(path.c_str()) != 0) {
            return -1;
        }
        return 0;
    }

    // regular file or symlink
    if (unlink(path.c_str()) != 0) {
        return -1;
    }
    return 0;
}

// Ensure a directory exists and is empty
inline void ensure_empty_dir(const std::string& dir) {
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0) {
        // exists — nuke contents
        remove_path_recursive(dir);
    }
    // (re)create
    ::mkdir(dir.c_str(), 0777);
}

} // namespace

class RedisFileCacheLRUTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(RedisFileCacheLRUTest);
        CPPUNIT_TEST(testBasicWriteReadAndIndices);
        CPPUNIT_TEST(testCreateOnlySemantics);
        CPPUNIT_TEST(testReadBusyWhenWriterLockPresent);
        CPPUNIT_TEST(testBlockingHelpers);
        CPPUNIT_TEST(testLRUEviction);
    CPPUNIT_TEST_SUITE_END();

  public:
    // Config overridable via env vars
    std::string host   = getenv("REDIS_HOST") ? getenv("REDIS_HOST") : std::string("127.0.0.1");
    int         port   = getenv("REDIS_PORT") ? std::atoi(getenv("REDIS_PORT")) : 6379;
    int         db     = getenv("REDIS_DB")   ? std::atoi(getenv("REDIS_DB"))   : 0;

    std::string cache_dir;
    std::string ns;

    RcPtr rc;

    void setUp() override {
        ns = std::string("poc-cache-ut:") + rand_hex(6);
        cache_dir = "/tmp/poc-cache-" + rand_hex(6);
        ensure_empty_dir(cache_dir);

        rc = rc_connect(host, port, db);
        CPPUNIT_ASSERT(rc && "redis connect failed in test setUp");
        del_namespace(rc, ns);
    }

    void tearDown() override {
        if (rc) {
            del_namespace(rc, ns);
            rc.reset();
        }
        // Fully remove the temp cache directory and its contents
        remove_path_recursive(cache_dir);
    }

    // ---------- TESTS ----------

    void testBasicWriteReadAndIndices() {
        RedisFileCache c(cache_dir, host, port, db, /*ttl_ms*/60000, ns, /*max_bytes*/0);

        const std::string key = "k-" + rand_hex(6) + ".bin";
        const std::string data = "hello world";

        // Write & read
        c.write_bytes_create(key, data);
        CPPUNIT_ASSERT(c.exists(key));
        std::string got = c.read_bytes(key);
        CPPUNIT_ASSERT_EQUAL(data, got);

        // Indices: sizes, total, lru, keys set
        const std::string h_sizes = ns + ":idx:size";
        const std::string k_total = ns + ":idx:total";
        const std::string z_lru   = ns + ":idx:lru";
        const std::string s_keys  = ns + ":keys:set";

        // Size
        {
            redisReply* r = (redisReply*)redisCommand(rc.get(), "HGET %s %b", h_sizes.c_str(), key.data(), (size_t)key.size());
            CPPUNIT_ASSERT(r && r->type == REDIS_REPLY_STRING);
            std::string sz(r->str, r->len);
            freeReplyObject(r);
            CPPUNIT_ASSERT_EQUAL((long long)data.size(), std::stoll(sz));
        }
        // Total
        {
            redisReply* r = (redisReply*)redisCommand(rc.get(), "GET %s", k_total.c_str());
            CPPUNIT_ASSERT(r && (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_INTEGER));
            long long total = (r->type==REDIS_REPLY_INTEGER) ? r->integer : std::stoll(std::string(r->str, r->len));
            freeReplyObject(r);
            CPPUNIT_ASSERT_EQUAL((long long)data.size(), total);
        }
        // Keys set
        {
            redisReply* r = (redisReply*)redisCommand(rc.get(), "SISMEMBER %s %b", s_keys.c_str(), key.data(), (size_t)key.size());
            CPPUNIT_ASSERT(r && r->type == REDIS_REPLY_INTEGER);
            CPPUNIT_ASSERT_EQUAL((long long)1, r->integer);
            freeReplyObject(r);
        }
        // LRU touched on read: ensure member exists
        {
            redisReply* r = (redisReply*)redisCommand(rc.get(), "ZSCORE %s %b", z_lru.c_str(), key.data(), (size_t)key.size());
            CPPUNIT_ASSERT(r && (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_NIL));
            CPPUNIT_ASSERT(r->type == REDIS_REPLY_STRING);
            freeReplyObject(r);
        }
    }

    void testCreateOnlySemantics() {
        RedisFileCache c(cache_dir, host, port, db, 60000, ns, 0);

        const std::string key = "dup-" + rand_hex(6) + ".bin";
        const std::string data = "abc";
        c.write_bytes_create(key, data);

        bool threw = false;
        try {
            c.write_bytes_create(key, "xyz");
        } catch (const std::system_error& se) {
            threw = (se.code().value() == EEXIST);
        }
        CPPUNIT_ASSERT(threw);
    }

    void testReadBusyWhenWriterLockPresent() {
        RedisFileCache c(cache_dir, host, port, db, 60000, ns, 0);

        const std::string key = "busy-" + rand_hex(6) + ".bin";
        const std::string data = "payload";
        c.write_bytes_create(key, data);

        // Simulate a writer holding the write lock (no readers)
        const std::string wlock = ns + ":lock:write:" + key;
        {
            redisReply* r = (redisReply*)redisCommand(rc.get(), "SET %s token PX %d NX", wlock.c_str(), 3000);
            if (r) freeReplyObject(r);
        }

        bool gotBusy = false;
        try {
            std::string s = c.read_bytes(key);
            (void)s;
        } catch (const CacheBusyError&) {
            gotBusy = true;
        }
        CPPUNIT_ASSERT(gotBusy);

        // Cleanup simulated lock
        if (auto* r = (redisReply*)redisCommand(rc.get(), "DEL %s", wlock.c_str())) freeReplyObject(r);
    }

    void testBlockingHelpers() {
        RedisFileCache c(cache_dir, host, port, db, 60000, ns, 0);

        const std::string key = "blk-" + rand_hex(6) + ".bin";
        const std::string data = "0123456789";

        // Simulate transient writer lock
        const std::string wlock = ns + ":lock:write:" + key;
        {
            redisReply* r = (redisReply*)redisCommand(rc.get(), "SET %s x PX %d NX", wlock.c_str(), 250);
            if (r) freeReplyObject(r);
        }

        // Writer: should block then succeed
        bool w_ok = c.write_bytes_create_blocking(key, data, std::chrono::milliseconds(1500), std::chrono::milliseconds(20));
        CPPUNIT_ASSERT(w_ok);
        CPPUNIT_ASSERT(file_exists(cache_dir + "/" + key));

        // Reader: should succeed even with brief lock
        {
            redisReply* r = (redisReply*)redisCommand(rc.get(), "SET %s y PX %d NX", wlock.c_str(), 150);
            if (r) freeReplyObject(r);
        }
        std::string out;
        bool r_ok = c.read_bytes_blocking(key, out, std::chrono::milliseconds(1500), std::chrono::milliseconds(20));
        CPPUNIT_ASSERT(r_ok);
        CPPUNIT_ASSERT_EQUAL(data, out);
    }

    void testLRUEviction() {
        // Tiny capacity to force eviction
        const long long cap = 8 * 1024; // 8 KB
        RedisFileCache c(cache_dir, host, port, db, 60000, ns, cap);

        // Write several files > cap
        std::vector<std::string> keys;
        for (int i=0; i<6; ++i) {
            const std::string key = "ev-" + rand_hex(4) + ".bin";
            std::string data(4096, char('A' + i));
            c.write_bytes_create(key, data);
            keys.push_back(key);
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // separate LRU timestamps
        }

        // Let eviction settle
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        // Check total <= cap
        const std::string totalk = ns + ":idx:total";
        long long total = 0;
        if (auto* r = (redisReply*)redisCommand(rc.get(), "GET %s", totalk.c_str())) {
            if (r->type == REDIS_REPLY_STRING) total = std::stoll(std::string(r->str, r->len));
            else if (r->type == REDIS_REPLY_INTEGER) total = r->integer;
            freeReplyObject(r);
        }
        CPPUNIT_ASSERT(total <= cap);

        // At least one of the earliest files should be gone on disk
        int gone = 0;
        for (const auto& k : keys) {
            if (!file_exists(cache_dir + "/" + k)) ++gone;
        }
        CPPUNIT_ASSERT(gone >= 1);

        // Eviction log should have entries (best-effort)
        const std::string evlog = ns + ":evict:log";
        long long evcount = 0;
        if (auto* r = (redisReply*)redisCommand(rc.get(), "LLEN %s", evlog.c_str())) {
            if (r->type == REDIS_REPLY_INTEGER) evcount = r->integer;
            freeReplyObject(r);
        }
        CPPUNIT_ASSERT(evcount >= 1);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(RedisFileCacheLRUTest);

int main(int argc, char *argv[]) { return run_tests<RedisFileCacheLRUTest>(argc, argv) ? 0 : 1; }
