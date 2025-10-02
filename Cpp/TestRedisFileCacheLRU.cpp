// test_poc_cache_mproc_hiredis_lru.cpp

#include "RedisFileCacheLRU.h"
#include <hiredis/hiredis.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <getopt.h>

#include <random>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>

// ------------------ small hiredis helpers ------------------
static redisContext* rc_connect(const std::string& host, int port, int db) {
    redisContext* rc = redisConnect(host.c_str(), port);
    if (!rc || rc->err) {
        std::cerr << "redis connect error: " << (rc ? rc->errstr : "NULL") << "\n";
        if (rc) redisFree(rc);
        return nullptr;
    }
    if (db != 0) {
        if (auto* r = (redisReply*)redisCommand(rc, "SELECT %d", db)) freeReplyObject(r);
    }
    return rc;
}

static std::string srandmember(redisContext* rc, const std::string& key) {
    redisReply* r = (redisReply*)redisCommand(rc, "SRANDMEMBER %s", key.c_str());
    if (!r) return {};
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);
    if (r->type == REDIS_REPLY_STRING) return std::string(r->str, r->len);
    return {};
}

static void sadd(redisContext* rc, const std::string& key, const std::string& member) {
    if (auto* r = (redisReply*)redisCommand(rc, "SADD %s %b", key.c_str(), member.data(), (size_t)member.size()))
        freeReplyObject(r);
}

static void srem(redisContext* rc, const std::string& key, const std::string& member) {
    if (auto* r = (redisReply*)redisCommand(rc, "SREM %s %b", key.c_str(), member.data(), (size_t)member.size()))
        freeReplyObject(r);
}

static void del(redisContext* rc, const std::string& key) {
    if (auto* r = (redisReply*)redisCommand(rc, "DEL %s", key.c_str()))
        freeReplyObject(r);
}

static long long get_ll(redisContext* rc, const std::string& cmd_fmt, const std::string& key) {
    redisReply* r = (redisReply*)redisCommand(rc, cmd_fmt.c_str(), key.c_str());
    if (!r) return 0;
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);
    if (r->type == REDIS_REPLY_INTEGER) return r->integer;
    if (r->type == REDIS_REPLY_STRING) try { return std::stoll(std::string(r->str, r->len)); } catch (...) {}
    return 0;
}

static std::string short_hex(std::mt19937_64& gen, int n) {
    static const char* hexd = "0123456789abcdef";
    std::uniform_int_distribution<int> d(0,15);
    std::string s; s.reserve(n);
    for (int i=0;i<n;++i) s.push_back(hexd[d(gen)]);
    return s;
}

// ------------------ UPDATED WORKER ------------------
int worker(const std::string& cache_dir,
           const std::string& ns,
           const std::string& redis_host,
           int redis_port,
           int redis_db,
           double write_prob,
           int duration_sec,
           int read_sleep_ms,
           int write_sleep_ms,
           int key_suffix_chars,
           bool use_blocking,
           long long max_bytes)
{
    pid_t pid = getpid();
    // hiredis control for discovery set ops
    redisContext* rc = rc_connect(redis_host, redis_port, redis_db);
    if (!rc) return 1;

    // cache instance (bounded if max_bytes > 0)
    RedisFileCache cache(cache_dir, redis_host, redis_port, redis_db, 60000, ns, max_bytes);
    const std::string keyset = ns + ":keys:set";

    std::mt19937_64 gen((uint64_t)pid ^ (uint64_t)time(nullptr));
    std::uniform_real_distribution<double> u01(0.0,1.0);
    std::uniform_int_distribution<int> payload_len(200, 4000);

    auto ms_sleep = [](int ms){ if (ms>0) usleep(ms*1000); };
    auto now = [](){ return time(nullptr); };
    time_t t0 = now();

    long it=0, ro=0, rb=0, rm=0, rbytes=0;
    long wo=0, wb=0, we=0, wbytes=0, other=0;

    auto new_key = [&](){
        return std::to_string(pid) + "-" + short_hex(gen, key_suffix_chars) + ".bin";
    };

    while (now() - t0 < duration_sec) {
        ++it;
        bool do_write = (u01(gen) < write_prob);
        if (do_write) {
            auto key = new_key();
            int n = payload_len(gen);
            std::string hdr = "pid=" + std::to_string(pid) + ";key=" + key + ";rand=" + short_hex(gen, 8) + "\n";
            std::string data = hdr;
            data.resize(hdr.size() + n);
            for (size_t i=hdr.size(); i<data.size(); ++i) data[i] = char(gen() & 0xFF);

            try {
                if (use_blocking) {
                    if (cache.write_bytes_create_blocking(key, data, std::chrono::milliseconds(1500))) {
                        sadd(rc, keyset, key);
                        ++wo; wbytes += (long)data.size();
                    } else {
                        ++wb; // timed out waiting for lock
                    }
                } else {
                    cache.write_bytes_create(key, data);
                    sadd(rc, keyset, key);
                    ++wo; wbytes += (long)data.size();
                }
            } catch (const CacheBusyError&) {
                ++wb;
            } catch (const std::system_error& se) {
                if (se.code().value() == EEXIST) ++we;
                else {
                    ++other;
                    std::cerr << "Work write_bytes_create error: " << se.code().value() << '\n';
                }
            } catch (const std::runtime_error& re) {
                ++other;
                std::cerr << "Work write_bytes_create runtime error: " << re.what() << '\n';
            } catch (const std::exception& e) {
                ++other;
                std::cerr << "Work write_bytes_create exception: " << e.what() << '\n';
            } catch (...) {
                ++other;
                std::cerr << "Work write_bytes_create error but who knows why...\n";
            }
            ms_sleep(write_sleep_ms);
        } else {
            auto key = srandmember(rc, keyset);
            if (key.empty()) { ++rm; ms_sleep(read_sleep_ms); continue; }
            try {
                if (use_blocking) {
                    std::string s;
                    if (cache.read_bytes_blocking(key, s, std::chrono::milliseconds(1000))) {
                        ++ro; rbytes += (long)s.size();
                    } else {
                        ++rb; // timed out due to writer/evict fence
                    }
                } else {
                    auto s = cache.read_bytes(key);
                    ++ro; rbytes += (long)s.size();
                }
            } catch (const CacheBusyError&) {
                ++rb;
            } catch (const std::system_error& se) {
                if (se.code().value() == ENOENT) { ++rm; srem(rc, keyset, key); }
                else ++other;
            } catch (...) {
                ++other;
            }
            ms_sleep(read_sleep_ms);
        }
    }

    std::cout << "PID " << pid
              << " it=" << it
              << " R(ok/busy/miss)=" << ro << "/" << rb << "/" << rm
              << " Rbytes=" << rbytes
              << " W(ok/busy/exist)=" << wo << "/" << wb << "/" << we
              << " Wbytes=" << wbytes
              << " other=" << other
              << std::endl;

    redisFree(rc);
    return 0;
}

// ------------------ UPDATED MAIN ------------------
int main(int argc, char** argv) {
    int processes = 4;
    int duration = 20;
    std::string cache_dir = "/tmp/poc-cache";
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    int redis_db = 0;
    std::string ns = "poc-cache";
    double write_prob = 0.15;
    int read_sleep_ms = 5;
    int write_sleep_ms = 20;
    int key_suffix_chars = 4;
    bool blocking = false;
    long long max_bytes = 0; // 0 => unbounded
    int monitor_every_ms = 1000; // parent monitor tick

    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--processes") && i+1<argc) processes = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i+1<argc) duration = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache-dir") && i+1<argc) cache_dir = argv[++i];
        else if (!strcmp(argv[i], "--redis-host") && i+1<argc) redis_host = argv[++i];
        else if (!strcmp(argv[i], "--redis-port") && i+1<argc) redis_port = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--redis-db") && i+1<argc) redis_db = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--namespace") && i+1<argc) ns = argv[++i];
        else if (!strcmp(argv[i], "--write-prob") && i+1<argc) write_prob = std::atof(argv[++i]);
        else if (!strcmp(argv[i], "--read-sleep") && i+1<argc) read_sleep_ms = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--write-sleep") && i+1<argc) write_sleep_ms = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--key-suffix-chars") && i+1<argc) key_suffix_chars = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--blocking")) blocking = true;
        else if (!strcmp(argv[i], "--max-bytes") && i+1<argc) max_bytes = std::atoll(argv[++i]);
        else if (!strcmp(argv[i], "--monitor-ms") && i+1<argc) monitor_every_ms = std::atoi(argv[++i]);
    }

    ::mkdir(cache_dir.c_str(), 0777);

    // Parent hiredis connection for prep & monitoring
    redisContext* rc = rc_connect(redis_host, redis_port, redis_db);
    if (!rc) return 1;

    // Clean discovery set (so runs are independent)
    const std::string keyset = ns + ":keys:set";
    del(rc, keyset);
    const std::string z_lru = ns + ":idx:lru";
    del(rc, z_lru);
    const std::string h_sizes = ns + ":idx:size";
    del(rc, h_sizes);
    const std::string total_key = ns + ":idx:total";
    del(rc, total_key);

    // Single process version, else ... [rest of main]
    if (processes == 0) {
        long long total_bytes = get_ll(rc, "GET %s", total_key);
        long long nkeys = get_ll(rc, "SCARD %s", keyset);

        std::cout << "total_bytes=" << total_bytes
                  << " keys=" << nkeys
                  << (max_bytes>0 ? (" cap=" + std::to_string(max_bytes)) : "")
                  << "\n";

        redisFree(rc);

        worker(cache_dir, ns, redis_host, redis_port, redis_db,
                          write_prob, duration, read_sleep_ms, write_sleep_ms,
                          key_suffix_chars, blocking, max_bytes);

        redisContext* rc = rc_connect(redis_host, redis_port, redis_db);
        if (!rc) return 1;

        total_bytes = get_ll(rc, "GET %s", total_key);
        nkeys = get_ll(rc, "SCARD %s", keyset);

        std::cout << "total_bytes=" << total_bytes
                  << " keys=" << nkeys
                  << (max_bytes>0 ? (" cap=" + std::to_string(max_bytes)) : "")
                  << "\n";

        redisFree(rc);
        return 0;
    }

    // Monitor keys for reporting

    // Spawn workers (fork)
    std::vector<pid_t> pids; pids.reserve(processes);
    for (int i=0; i<processes; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // child
            return worker(cache_dir, ns, redis_host, redis_port, redis_db,
                          write_prob, duration, read_sleep_ms, write_sleep_ms,
                          key_suffix_chars, blocking, max_bytes);
        } else if (pid > 0) {
            pids.push_back(pid);
        } else {
            perror("fork");
            redisFree(rc);
            return 1;
        }
    }

    // Simple parent-side monitor loop
    auto t_start = std::chrono::steady_clock::now();
    while (true) {
        // Check if all children have exited (non-blocking)
        int live = 0;
        for (auto pid : pids) {
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == 0) ++live;
        }

        // Print totals
        long long total_bytes = get_ll(rc, "GET %s", total_key);
        long long nkeys = get_ll(rc, "SCARD %s", keyset);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t_start).count();

        std::cout << "[monitor t=" << elapsed
                  << "s] total_bytes=" << total_bytes
                  << " keys=" << nkeys
                  << (max_bytes>0 ? (" cap=" + std::to_string(max_bytes)) : "")
                  << "\n";

        if (live == 0) break; // all done
        usleep(monitor_every_ms * 1000);
    }

    redisFree(rc);
    return 0;
}
