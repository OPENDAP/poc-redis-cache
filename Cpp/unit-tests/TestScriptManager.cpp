//
// Created by James Gallagher on 10/2/25.
//

// test_ScriptManager.cpp
// CppUnit tests for ScriptManager (hiredis-based).
// No main() here; integrate with your existing CppUnit runner.

#include "ScriptManager.h"
#include "run_tests_cppunit.h"

#include <hiredis/hiredis.h>

#include <memory>
#include <string>
#include <vector>

namespace {
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
    if (auto* r = (redisReply*)redisCommand(rc, "HELLO 2")) freeReplyObject(r); // prefer RESP2
    return RcPtr(rc);
}
} // namespace

class ScriptManagerTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(ScriptManagerTest);
        CPPUNIT_TEST(testRegisterLoadAndEval);
        CPPUNIT_TEST(testReloadOnNoScript);
        CPPUNIT_TEST(testEvalKeysAndArgs);
    CPPUNIT_TEST_SUITE_END();

  public:
    std::string host   = getenv("REDIS_HOST") ? getenv("REDIS_HOST") : std::string("127.0.0.1");
    int         port   = getenv("REDIS_PORT") ? std::atoi(getenv("REDIS_PORT")) : 6379;
    int         db     = getenv("REDIS_DB")   ? std::atoi(getenv("REDIS_DB"))   : 0;

    RcPtr rc;

    void setUp() override {
        rc = rc_connect(host, port, db);
        CPPUNIT_ASSERT(rc && "redis connect failed in ScriptManager tests");
    }
    void tearDown() override {
        rc.reset();
    }

    void testRegisterLoadAndEval() {
        ScriptManager sm(rc.get());
        const std::string name = "ret42";
        const std::string body = "return 42";
        const auto& sha = sm.register_and_load(name, body);
        CPPUNIT_ASSERT_EQUAL((size_t)40, sha.size()); // SHA1 hex length

        long long v = sm.evalsha_ll(name, /*nkeys*/0, /*keys*/{}, /*argv*/{});
        CPPUNIT_ASSERT_EQUAL(42LL, v);
    }

    void testReloadOnNoScript() {
        ScriptManager sm(rc.get());
        const std::string name = "inc";
        const std::string body = R"(
            local k = KEYS[1]
            local x = tonumber(redis.call('GET', k) or "0")
            x = x + 1
            redis.call('SET', k, x)
            return x
        )";
        sm.register_and_load(name, body);

        // Cause NOSCRIPT by flushing the script cache
        if (auto* r = (redisReply*)redisCommand(rc.get(), "SCRIPT FLUSH")) freeReplyObject(r);

        long long v = sm.evalsha_ll(name, 1, std::vector<std::string>{"sm:test:ctr"}, {});
        // On success, manager should have auto-reloaded and returned 1
        CPPUNIT_ASSERT(v >= 1);
    }

    void testEvalKeysAndArgs() {
        ScriptManager sm(rc.get());
        const std::string name = "sum";
        const std::string body = R"(
            -- sums numeric ARGV (ARVG in older doc typo) and returns total + number of KEYS
            local tot = 0
            for i, a in ipairs(ARGV) do tot = tot + tonumber(a) end
            return tot + #KEYS
        )";
        sm.register_and_load(name, body);

        long long v0 = sm.evalsha_ll(name, 0, {}, std::vector<std::string>{"3","4","5"});
        CPPUNIT_ASSERT_EQUAL(12LL, v0);

        long long v1 = sm.evalsha_ll(name, 2, std::vector<std::string>{"k1","k2"}, std::vector<std::string>{"10"});
        // 10 + #KEYS(=2) == 12
        CPPUNIT_ASSERT_EQUAL(12LL, v1);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(ScriptManagerTest);

int main(int argc, char *argv[]) { return run_tests<ScriptManagerTest>(argc, argv) ? 0 : 1; }
