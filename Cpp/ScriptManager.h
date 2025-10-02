//
// Created by James Gallagher on 10/2/25.
//

#ifndef POC_CACHE_HIREDIS_REDISSCRIPTMANAGER_H
#define POC_CACHE_HIREDIS_REDISSCRIPTMANAGER_H

// ---------- ScriptManager.h ----------

#include <hiredis/hiredis.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <stdexcept>

class ScriptManager {
public:
    explicit ScriptManager(redisContext* rc) : rc_(rc) {
        // Force RESP2 for simpler replies (optional but recommended)
        if (auto* r = (redisReply*)redisCommand(rc_, "HELLO 2")) freeReplyObject(r);
    }

    // Register a script body and load it immediately; returns the SHA1.
    const std::string& register_and_load(const std::string& name, const std::string& body) {
        auto sha = script_load(body);
        entries_[name] = {body, sha};
        return entries_[name].sha;
    }

    // Return the current SHA for a known script name
    const std::string& sha(const std::string& name) const {
        return entries_.at(name).sha;
    }

    // EVALSHA returning long long; auto-recovers on NOSCRIPT by reloading the script and retrying once.
    long long evalsha_ll(const std::string& name,
                         int nkeys, const std::vector<std::string>& keys,
                         const std::vector<std::string>& argv) {
        auto it = entries_.find(name);
        if (it == entries_.end()) throw std::runtime_error("Unknown script: " + name);

        try {
            return evalsha_ll_raw(it->second.sha, nkeys, keys, argv);
        } catch (const std::runtime_error& e) {
            std::string msg = e.what();
            if (msg.find("NOSCRIPT") != std::string::npos) {
                // Reload & retry once
                it->second.sha = script_load(it->second.body);
                return evalsha_ll_raw(it->second.sha, nkeys, keys, argv);
            }
            throw;
        }
    }

private:
    struct Entry { std::string body; std::string sha; };
    redisContext* rc_;
    std::unordered_map<std::string, Entry> entries_;

    static void reply_guard(redisReply* r) {
        if (!r) throw std::runtime_error("Redis command failed (NULL reply)");
    }

    std::string script_load(const std::string& body) {
        redisReply* r = (redisReply*)redisCommand(rc_, "SCRIPT LOAD %b", body.data(), (size_t)body.size());
        reply_guard(r);
        std::unique_ptr<redisReply, void(*)(void*)> G(r, freeReplyObject);
        if (r->type != REDIS_REPLY_STRING) throw std::runtime_error("SCRIPT LOAD: unexpected reply type");
        return std::string(r->str, r->len);
    }

    long long evalsha_ll_raw(const std::string& sha,
                             int nkeys, const std::vector<std::string>& keys,
                             const std::vector<std::string>& argv) {
        std::vector<const char*> av; av.reserve(3 + nkeys + (int)argv.size());
        std::vector<size_t> ln; ln.reserve(av.size());
        auto push = [&](const std::string& s){ av.push_back(s.data()); ln.push_back(s.size()); };

        std::string nkeys_s = std::to_string(nkeys);
        push("EVALSHA"); push(sha); push(nkeys_s);
        for (const auto& k: keys) push(k);
        for (const auto& a: argv) push(a);

        redisReply* rr = (redisReply*)redisCommandArgv(rc_, (int)av.size(), av.data(), ln.data());
        reply_guard(rr);
        std::unique_ptr<redisReply, void(*)(void*)> G(rr, freeReplyObject);

        // Accept common types
        switch (rr->type) {
            case REDIS_REPLY_INTEGER: return rr->integer;
#if defined(REDIS_REPLY_BOOL)
            case REDIS_REPLY_BOOL:    return rr->integer ? 1 : 0; // RESP3
#endif
            case REDIS_REPLY_STATUS:  return 1; // e.g., "OK"
            case REDIS_REPLY_NIL:     return 0;
            case REDIS_REPLY_STRING: {
                try { return std::stoll(std::string(rr->str, rr->len)); }
                catch (...) { throw std::runtime_error("EVALSHA string->int parse error"); }
            }
            case REDIS_REPLY_ERROR: {
                std::string msg(rr->str ? rr->str : "", rr->len ? rr->len : 0);
                throw std::runtime_error("EVALSHA error: " + msg);
            }
            default:
                throw std::runtime_error("EVALSHA: unexpected reply type");
        }
    }
};

#endif //POC_CACHE_HIREDIS_REDISSCRIPTMANAGER_H