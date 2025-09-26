//
// Created by James Gallagher on 9/26/25.
//

#include "redis_poc_cache.h"

#include <sw/redis++/redis++.h>
#include <sys/wait.h>
#include <unistd.h>
#include <getopt.h>
#include <random>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <sstream>

struct Stats {
    pid_t pid{};
    long iterations{};
    long reads_ok{};
    long reads_busy{};
    long reads_missing{};     // includes "no key yet" + file not found
    long read_bytes{};

    long writes_ok{};
    long writes_busy{};
    long writes_exists{};
    long write_bytes{};

    long other_errors{};

    void add(const Stats& o) {
        iterations += o.iterations;
        reads_ok += o.reads_ok;
        reads_busy += o.reads_busy;
        reads_missing += o.reads_missing;
        read_bytes += o.read_bytes;
        writes_ok += o.writes_ok;
        writes_busy += o.writes_busy;
        writes_exists += o.writes_exists;
        write_bytes += o.write_bytes;
        other_errors += o.other_errors;
    }
};

static std::string short_hex(std::mt19937_64& gen, int nchars) {
    static const char* hexd = "0123456789abcdef";
    std::uniform_int_distribution<int> dist(0,15);
    std::string s; s.reserve(nchars);
    for (int i=0;i<nchars;++i) s.push_back(hexd[dist(gen)]);
    return s;
}

int worker(const std::string& cache_dir,
           const std::string& redis_url,
           const std::string& ns,
           double write_prob,
           int duration_sec,
           int read_sleep_ms,
           int write_sleep_ms,
           int key_suffix_chars)
{
    pid_t pid = getpid();
    sw::redis::Redis r(redis_url);
    RedisFileCache cache(cache_dir, redis_url, 60000, ns);

    const std::string keyset = ns + ":keys:set";
    std::mt19937_64 gen(static_cast<uint64_t>(pid) ^ static_cast<uint64_t>(::time(nullptr)));
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int> payload_len(200, 4000);

    auto t_start = std::chrono::steady_clock::now();
    Stats st{}; st.pid = pid;

    auto now_secs = [&]{ return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t_start).count(); };

    auto ms_sleep = [](int ms){
        if (ms>0) usleep(ms*1000);
    };

    auto new_key = [&](){
        std::ostringstream oss;
        oss << pid << "-" << short_hex(gen, key_suffix_chars) << ".bin";
        return oss.str();
    };

    auto pick_key = [&]() -> std::string {
        auto key = r.srandmember<std::string>(keyset);
        if (key) return *key;
        return {};
    };

    while (now_secs() < duration_sec) {
        st.iterations++;
        bool do_write = (u01(gen) < write_prob);

        if (do_write) {
            auto key = new_key();
            int n = payload_len(gen);
            std::ostringstream hdr;
            hdr << "pid=" << pid << ";key=" << key << ";rand=" << short_hex(gen, 8) << "\n";
            std::string data = hdr.str();
            data.resize(data.size() + n);
            // fill tail with random bytes
            for (int i = hdr.str().size(); i < (int)data.size(); ++i) {
                data[i] = static_cast<char>(gen() & 0xFF);
            }

            try {
                cache.write_bytes_create(key, data);
                r.sadd(keyset, key);
                st.writes_ok++;
                st.write_bytes += static_cast<long>(data.size());
            } catch (const CacheBusyError&) {
                st.writes_busy++;
            } catch (const std::system_error& se) {
                if (se.code().value() == EEXIST) st.writes_exists++;
                else st.other_errors++;
            } catch (...) {
                st.other_errors++;
            }
            ms_sleep(write_sleep_ms);
        } else {
            auto key = pick_key();
            if (key.empty()) {
                st.reads_missing++;
                ms_sleep(read_sleep_ms);
                continue;
            }
            try {
                auto s = cache.read_bytes(key);
                st.reads_ok++;
                st.read_bytes += static_cast<long>(s.size());
            } catch (const CacheBusyError&) {
                st.reads_busy++;
            } catch (const std::system_error& se) {
                if (se.code().value() == ENOENT) {
                    // stale listing or external deletion
                    st.reads_missing++;
                    r.srem(keyset, key);
                } else {
                    st.other_errors++;
                }
            } catch (...) {
                st.other_errors++;
            }
            ms_sleep(read_sleep_ms);
        }
    }

    // Print stats as a single line for parent to parse
    std::cout << "PID " << pid
              << " it=" << st.iterations
              << " R(ok/busy/miss)=" << st.reads_ok << "/" << st.reads_busy << "/" << st.reads_missing
              << " Rbytes=" << st.read_bytes
              << " W(ok/busy/exist)=" << st.writes_ok << "/" << st.writes_busy << "/" << st.writes_exists
              << " Wbytes=" << st.write_bytes
              << " other=" << st.other_errors
              << std::endl;

    // Exit code unused; parent just reads lines
    return 0;
}

int main(int argc, char** argv) {
    int processes = 4;
    int duration = 20;
    std::string cache_dir = "/tmp/poc-cache";
    std::string redis_url = "tcp://127.0.0.1:6379";
    std::string ns = "poc-cache";
    double write_prob = 0.15;
    int read_sleep_ms = 5;
    int write_sleep_ms = 20;
    int key_suffix_chars = 4; // shorter => more collisions => EEXIST shows up

    // crude arg parsing
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--processes") && i+1<argc) processes = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i+1<argc) duration = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache-dir") && i+1<argc) cache_dir = argv[++i];
        else if (!strcmp(argv[i], "--redis-url") && i+1<argc) redis_url = argv[++i];
        else if (!strcmp(argv[i], "--namespace") && i+1<argc) ns = argv[++i];
        else if (!strcmp(argv[i], "--write-prob") && i+1<argc) write_prob = std::atof(argv[++i]);
        else if (!strcmp(argv[i], "--read-sleep") && i+1<argc) read_sleep_ms = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--write-sleep") && i+1<argc) write_sleep_ms = std::atoi(argv[++i]);
        else if (!strcmp(argv[i], "--key-suffix-chars") && i+1<argc) key_suffix_chars = std::atoi(argv[++i]);
    }

    ::mkdir(cache_dir.c_str(), 0777);

    // Clean previous set (optional but recommended for clean runs)
    {
        sw::redis::Redis r(redis_url);
        r.del(ns + ":keys:set");
    }

    std::vector<pid_t> pids;
    pids.reserve(processes);
    for (int i=0;i<processes;++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // child
            return worker(cache_dir, redis_url, ns, write_prob, duration,
                          read_sleep_ms, write_sleep_ms, key_suffix_chars);
        } else if (pid > 0) {
            pids.push_back(pid);
        } else {
            std::perror("fork");
            return 1;
        }
    }

    // Collect children
    int status = 0;
    for (pid_t pid : pids) {
        int s = 0;
        waitpid(pid, &s, 0);
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0) status = 1;
    }
    return status;
}
