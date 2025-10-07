//
// Created by James Gallagher on 9/29/25.
//

#ifndef POC_REDIS_CACHE_REDIS_POC_CACHE_HIREDIS_H
#define POC_REDIS_CACHE_REDIS_POC_CACHE_HIREDIS_H

#include <string>
#include <stdexcept>
#include <memory>
#include <chrono>

#include "ScriptManager.h"

struct redisContext;
struct redisReply;

/**
 * Exception thrown when a non-blocking read or write operation fails
 * because the item n question is locked in a way the prevents the read
 * or write from succeeding.
 */
class CacheBusyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * A disk file cache designed to be multiprocess and multi-host safe.
 * The cache uses a Redis server as a cache lock manager. The cache
 * also uses the Redis server to hold information used to implement
 * an LRU cache eviction scheme.
 *
 * @note This is not also multithread safe.
 */
class RedisFileCache {
public:
    // bounded-cache ctor (max_bytes <= 0 => unbounded)
    explicit RedisFileCache(std::string cache_dir,
                   const std::string &redis_host = "127.0.0.1",
                   int redis_port = 6379,
                   int redis_db = 0,
                   long long lock_ttl_ms = 60000,
                   std::string ns = "poc-cache",
                   long long max_bytes = 0);

    // Non-copyable
    RedisFileCache(const RedisFileCache&) = delete;
    RedisFileCache& operator=(const RedisFileCache&) = delete;

    std::string read_bytes(const std::string& key) const;
    void write_bytes_create(const std::string& key, const std::string& data);
    bool exists(const std::string& key) const;


    bool read_bytes_blocking(const std::string& key,
                             std::string& out,
                             std::chrono::milliseconds timeout,
                             std::chrono::milliseconds backoff = std::chrono::milliseconds(10)) const;

    bool write_bytes_create_blocking(const std::string& key,
                                     const std::string& data,
                                     std::chrono::milliseconds timeout,
                                     std::chrono::milliseconds backoff = std::chrono::milliseconds(10));

    const std::string& namespace_prefix() const { return ns_; }

private:
    std::string cache_dir_; /// Where the files are stored
    std::string ns_;    /// Redis key Namespace
    long long ttl_ms_;  /// File lock max lifetime; prevent stale locks
    long long max_bytes_ = 0; /// How big a cache? 0 == unbounded.

    // This controls how often the cache purge actually happens, regardless of how often
    // new files are added. Make this small for certain tests, etc. (see TestRedisFileCacheLRU.cpp)
    // jhrg 10/4//25
    long long purge_mtx_ttl_ms_ = 2000; /// Minimum purge frequency
    double purge_factor_ = 0.2; /// Purge below max_bytes_ by this factor; between 0.0 and 1.0

    std::unique_ptr<redisContext, void(*)(redisContext*)> rc_;  /// The Redis connection
    std::unique_ptr<ScriptManager> scripts_{nullptr};   /// Manages the LUA scripts

    // index keys

    std::string z_lru_ = ns_ + ":idx:lru";    // ZSET: key -> last access ms
    std::string h_sizes_ = ns_ + ":idx:size";  // HASH: key -> size
    std::string s_keys_ = ns_ + ":keys:set";   // SET: all keys (kept for tests / discovery)
    std::string k_total_ = ns_ + ":idx:total";  // STRING: total bytes
    std::string k_purge_mtx_ = ns_ + ":purge:mutex"; // STRING: purger mutex
    std::string k_evict_fence_ = ns_ + ":lock:evict:";  // STRING: eviction fence prefix

    // hiredis helpers
    long long cmd_ll(const char* fmt, ...) const;
    std::string cmd_s(const char* fmt, ...) const;

    void acquire_read(const std::string& key) const;
    void release_read(const std::string& key) const noexcept;
    std::string acquire_write(const std::string& key) const;
    void release_write(const std::string& key, const std::string& token) const noexcept;
    bool can_evict_now(const std::string& key) const;

    std::string k_evict_fence(const std::string& key) const { return k_evict_fence_ + key; };

    static long long now_ms();
    void touch_lru(const std::string& key, long long ts_ms) const;
    void index_add_on_publish(const std::string& key, long long size, long long ts_ms) const;
    void index_remove_on_delete(const std::string& key, long long size);
    long long get_total_bytes() const;
    static long long file_size_bytes(const std::string& path) ;

    void ensure_capacity();                       // loop until total<=max
    bool try_evict_one(std::string& victim, long long& freed);

    // file helpers
    static void validate_key(const std::string& key);
    std::string path_for(const std::string& key) const;
    std::string k_write(const std::string& key) const;
    std::string k_readers(const std::string& key) const;
    static bool file_exists_(const std::string& p);
    static void fsync_fd(int fd);

    friend class RedisFileCacheLRUTest;

public:
    // setters/getters
    long long get_purge_mtx_ttl() const { return purge_mtx_ttl_ms_; }
    void set_purge_mtx_ttl(const long long ttl) { purge_mtx_ttl_ms_ = ttl; }

    double get_purge_factor() const { return purge_factor_; }
    void set_purge_factor(const double pf) { if (pf < 0.0 || pf > 1.0) return; purge_factor_ = pf; }
};

#endif //POC_REDIS_CACHE_REDIS_POC_CACHE_HIREDIS_H