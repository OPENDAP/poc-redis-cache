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

/// Return a unique pointer to a redisConnect object with an open database
/// @param host Connect to redis on this host (DNS name or TCP/IP address)
/// @param port Connect using this port
/// @param db Open this database on the Redis server
inline RcPtr rc_connect(const std::string& host, int port, int db) {
    redisContext* rc = redisConnect(host.c_str(), port);
    if (!rc || rc->err) {
        if (rc) { fprintf(stderr, "redis err: %s\n", rc->errstr); redisFree(rc); }
        return {nullptr};
    }
    if (db != 0) {
        if (auto* r = static_cast<redisReply *>(redisCommand(rc, "SELECT %d", db))) freeReplyObject(r);
    }
    // Prefer RESP2 for stable replies
    if (auto* r = static_cast<redisReply *>(redisCommand(rc, "HELLO 2"))) freeReplyObject(r);
    return RcPtr(rc);
}

/// Delete all the Redis in the Redis database reference by 'rc' that have the namespace 'ns'
/// @param rc Open redisConnect object with an associated database
/// @param ns The namespace of the keys to delete.
inline void del_namespace(RcPtr& rc, const std::string& ns) {
    std::string cursor = "0";
    const std::string patt = ns + ":*";
    do {
        auto* r = static_cast<redisReply *>(redisCommand(rc.get(), "SCAN %s MATCH %b COUNT 200",
                                                               cursor.c_str(), patt.data(), (size_t) patt.size()));
        if (!r) break;
        std::unique_ptr<redisReply, void(*)(void*)> G(r, freeReplyObject);
        if (r->type != REDIS_REPLY_ARRAY || r->elements < 2) break;
        cursor = (r->element[0]->type==REDIS_REPLY_STRING) ? std::string(r->element[0]->str, r->element[0]->len) : "0";
        const auto* arr = r->element[1];
        for (size_t i=0; i<arr->elements; ++i) {
            if (arr->element[i]->type == REDIS_REPLY_STRING) {
                std::string k(arr->element[i]->str, arr->element[i]->len);
                if (auto* d = static_cast<redisReply *>(redisCommand(rc.get(), "DEL %b", k.data(), (size_t) k.size())))
                    freeReplyObject(d);
            }
        }
    } while (cursor != "0");
}

/// Return true if the file exists and is a regular file.
inline bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/// Return a random hex number of 'n' digits. Default is 8 digits.
inline std::string rand_hex(int n=8) {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    static const char* hexd="0123456789abcdef";
    std::uniform_int_distribution<int> d(0,15);
    std::string s; s.reserve(n);
    for (int i=0;i<n;++i) s.push_back(hexd[d(gen)]);
    return s;
}

/// Recursively clean/remove a directory.
/// @param path Delete this file or directory (and its contents)
/// @return 0 indicates no error, -1 indicates an error.
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
                // ignore this error and keep going
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

/// Delete a directory, then re-make it.
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
        CPPUNIT_TEST(test_basic_write_read_and_indices);
        CPPUNIT_TEST(test_create_only_semantics);
        CPPUNIT_TEST(test_read_busy_when_writer_lock_present);
        CPPUNIT_TEST(test_blocking_writer);
        CPPUNIT_TEST(test_blocking_reader);
        CPPUNIT_TEST(test_lru_eviction);
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
            del_namespace(rc, ns); // clean redis
            rc.reset();
        }
        // Fully remove the temp cache directory and its contents
        remove_path_recursive(cache_dir);
    }

    // ---------- TESTS ----------

    void test_basic_write_read_and_indices() {
        DBG(std::cerr << __func__ << std::endl);
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
            auto r = static_cast<redisReply *>(redisCommand(rc.get(), "HGET %s %b", h_sizes.c_str(), key.data(), (size_t) key.size()));
            CPPUNIT_ASSERT(r && r->type == REDIS_REPLY_STRING);
            std::string sz(r->str, r->len);
            freeReplyObject(r);
            DBG(std::cerr << "size: " << sz << std::endl);
            CPPUNIT_ASSERT_EQUAL((long long)data.size(), std::stoll(sz));
        }
        // Total
        {
            auto r = static_cast<redisReply *>(redisCommand(rc.get(), "GET %s", k_total.c_str()));
            CPPUNIT_ASSERT(r && (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_INTEGER));
            long long total = (r->type==REDIS_REPLY_INTEGER) ? r->integer : std::stoll(std::string(r->str, r->len));
            freeReplyObject(r);
            DBG(std::cerr << "total: " << total << std::endl);
            CPPUNIT_ASSERT_EQUAL((long long)data.size(), total);
        }
        // Keys set
        {
            auto r = static_cast<redisReply *>(redisCommand(rc.get(), "SISMEMBER %s %b", s_keys.c_str(), key.data(), (size_t) key.size()));
            CPPUNIT_ASSERT(r && r->type == REDIS_REPLY_INTEGER);
            CPPUNIT_ASSERT_EQUAL((long long)1, r->integer);
            freeReplyObject(r);
        }
        // LRU touched on read: ensure member exists
        {
            auto r = static_cast<redisReply *>(redisCommand(rc.get(), "ZSCORE %s %b", z_lru.c_str(), key.data(), (size_t) key.size()));
            CPPUNIT_ASSERT(r && (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_NIL));
            CPPUNIT_ASSERT(r->type == REDIS_REPLY_STRING);
            DBG(std::cerr << "zscore: " << r->str << std::endl);    // we don't test this value, but it's interesting to see. jhrg 10/4/25
            freeReplyObject(r);
        }
        DBG(std::cerr << std::endl);
    }

    void test_create_only_semantics() {
        DBG(std::cerr << __func__ << std::endl);
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
        DBG(std::cerr << std::endl);
    }

    void test_read_busy_when_writer_lock_present() {
        DBG(std::cerr << __func__ << std::endl);
        RedisFileCache c(cache_dir, host, port, db, 60000, ns, 0);

        const std::string key = "busy-" + rand_hex(6) + ".bin";
        const std::string data = "payload";
        c.write_bytes_create(key, data);

        // Simulate a writer holding the write lock (no readers)
        const std::string wlock = ns + ":lock:write:" + key;
        {
            // NB: PX: set the time to expire in ms; NX: only set the key if it does not exist. jhrg 10/4/25
            const auto r = static_cast<redisReply *>(redisCommand(rc.get(), "SET %s token PX %d NX", wlock.c_str(), 3000));
            if (r) freeReplyObject(r);
        }

        CPPUNIT_ASSERT_THROW_MESSAGE("Attempt to read while write locked should fail", c.read_bytes(key), CacheBusyError);

        // Cleanup simulated lock
        if (const auto r = static_cast<redisReply *>(redisCommand(rc.get(), "DEL %s", wlock.c_str()))) freeReplyObject(r);
        DBG(std::cerr << std::endl);
    }

    void test_blocking_writer() {
        DBG(std::cerr << __func__ << std::endl);
        RedisFileCache c(cache_dir, host, port, db, 60000, ns, 0);

        const std::string key = "blk-" + rand_hex(6) + ".bin";
        const std::string data = "0123456789";

        // Simulate transient writer lock
        const std::string wlock = ns + ":lock:write:" + key;
        {
            const auto r = static_cast<redisReply *>(redisCommand(rc.get(), "SET %s x PX %d NX", wlock.c_str(), 1000));
            if (r) freeReplyObject(r);
        }

        // This attempt should fail because the write lock (set above) TTL is 1000ms but the time this call will
        // try to get a write lock on the same file is only 500ms. jhrg 10/425
        bool w_ok = c.write_bytes_create_blocking(key, data, std::chrono::milliseconds(500), std::chrono::milliseconds(20));
        CPPUNIT_ASSERT(!w_ok);
        CPPUNIT_ASSERT(!file_exists(cache_dir + "/" + key));

        // Writer: should block then succeed. By now 500ms+ have elapsed and this call will try for another 1500ms.
        // The simulated write lock above should expire within that time interval. jhrg 10/4/25
        w_ok = c.write_bytes_create_blocking(key, data, std::chrono::milliseconds(1500), std::chrono::milliseconds(20));
        CPPUNIT_ASSERT(w_ok);
        CPPUNIT_ASSERT(file_exists(cache_dir + "/" + key));

        if (const auto r = static_cast<redisReply *>(redisCommand(rc.get(), "DEL %s", wlock.c_str()))) freeReplyObject(r);
        DBG(std::cerr << std::endl);
    }

    void test_blocking_reader() {
        DBG(std::cerr << __func__ << std::endl);
        RedisFileCache c(cache_dir, host, port, db, 60000, ns, 0);

        const std::string key = "blk-" + rand_hex(6) + ".bin";
        const std::string data = "0123456789";

        // Seed the test by adding a file to the cache. This should work.
        bool w_ok = c.write_bytes_create_blocking(key, data, std::chrono::milliseconds(500), std::chrono::milliseconds(20));
        CPPUNIT_ASSERT_MESSAGE("The file should have been written.", w_ok);
        CPPUNIT_ASSERT_MESSAGE("The file '" + key + "' should exist.", file_exists(cache_dir + "/" + key));

        // Simulate transient writer lock
        const std::string wlock = ns + ":lock:write:" + key;

        // As above for the writer tests. jhrg 10/4/25
        {
            const auto r = static_cast<redisReply *>(redisCommand(rc.get(), "SET %s y PX %d NX", wlock.c_str(), 1000));
            if (r) freeReplyObject(r);
        }

        std::string out;
        bool r_ok = c.read_bytes_blocking(key, out, std::chrono::milliseconds(500), std::chrono::milliseconds(20));
        CPPUNIT_ASSERT_MESSAGE("This attempt to read a write locked file should fail.", !r_ok);
        CPPUNIT_ASSERT_MESSAGE("The file '" + key + "' should exist.", file_exists(cache_dir + "/" + key));

        r_ok = c.read_bytes_blocking(key, out, std::chrono::milliseconds(1500), std::chrono::milliseconds(20));
        CPPUNIT_ASSERT_MESSAGE("The write lock should have expired and this attempt to read should work.", r_ok);
        CPPUNIT_ASSERT_EQUAL(data, out);

        if (const auto r = static_cast<redisReply *>(redisCommand(rc.get(), "DEL %s", wlock.c_str()))) freeReplyObject(r);
        DBG(std::cerr << std::endl);
    }

    void test_lru_eviction() {
        DBG(std::cerr << __func__ << std::endl);
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
        const std::string total_k = ns + ":idx:total";
        long long total = 0;
        if (auto* r = static_cast<redisReply *>(redisCommand(rc.get(), "GET %s", total_k.c_str()))) {
            if (r->type == REDIS_REPLY_STRING) total = std::stoll(std::string(r->str, r->len));
            else if (r->type == REDIS_REPLY_INTEGER) total = r->integer;
            freeReplyObject(r);
        }
        DBG(std::cerr << "total: " << total << ", cap: " << cap << '\n');
        CPPUNIT_ASSERT(total <= cap);

        // At least one of the earliest files should be gone on disk
        int gone = 0;
        for (const auto& k : keys) {
            if (!file_exists(cache_dir + "/" + k)) ++gone;
        }
        DBG(std::cerr << "gone: " << gone << '\n');
        CPPUNIT_ASSERT(gone >= 1);

        // Eviction log should have entries (best-effort)
        const std::string evlog = ns + ":evict:log";
        long long evcount = 0;
        if (auto* r = (redisReply*)redisCommand(rc.get(), "LLEN %s", evlog.c_str())) {
            if (r->type == REDIS_REPLY_INTEGER) evcount = r->integer;
            freeReplyObject(r);
        }
        CPPUNIT_ASSERT(evcount >= 1);
        DBG(std::cerr << std::endl);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(RedisFileCacheLRUTest);

int main(int argc, char *argv[]) { return run_tests<RedisFileCacheLRUTest>(argc, argv) ? 0 : 1; }
