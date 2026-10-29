// UniSSO v2 — Vue 3 SPA + C++ backend.
// The HTTP framework is vendored from Sakjijdidji55/C-backend-server-framework
// (MIT). Application code lives under src/app.

#include <iostream>
#include <string>

#include "src/app/core.h"
#include "src/app/logger.h"

int main() {
    try {
        applog::init();
        auto& cfg = AppConfig::get();

        std::cout << "UniSSO v2 starting on port " << cfg.port
                  << " (issuer " << cfg.issuer << ")" << std::endl;
        std::cout << "[config] mysql=" << cfg.mysqlHost << ":" << cfg.mysqlPort
                  << "/" << cfg.mysqlDb << " redis=" << cfg.redisHost << ":"
                  << cfg.redisPort << " user=" << cfg.redisUser
                  << " prefix=" << cfg.redisPrefix << " web=" << cfg.webRoot
                  << std::endl;

        if (!Db::init()) {
            std::cerr << "FATAL: MySQL unavailable" << std::endl;
            return 1;
        }
        if (!Cache::init()) {
            std::cerr << "FATAL: Redis unavailable" << std::endl;
            return 1;
        }
        if (!RsJwt::init()) {
            std::cerr << "FATAL: RS256 signing keys unavailable" << std::endl;
            return 1;
        }
        sse::selfTest();

        Server app(cfg.port);

        // One global request guard: host allow-list, security headers, CSRF.
        app.use([](const Request& req, Response& res) {
            http::securityHeaders(res, req);
            if (!authx::allowHost(req)) {
                http::error(res, 400, "invalid_host", "请求的主机名不被允许");
                return false;
            }
            if (!authx::validateCsrf(req, res)) {
                return false;
            }
            return true;
        });

        handlers::registerAll(app);
        app.setNotFoundHandler(spaFallback);

        services::startHeartbeat();
        app.run();
        // Order matters: join every background thread while static globals
        // (logger queue, Db, Cache) are still alive. Returning from main
        // runs exit() -> static destructors; a joinable std::thread
        // destroyed there calls std::terminate().
        services::stopHeartbeat();
        applog::shutdown();
        std::cout << "UniSSO v2 exited" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << std::endl;
        services::stopHeartbeat();
        return 1;
    }
}
