#pragma once
#include "handler/request_handler.hpp"

// ═══════════════════════════════════════════════════════════════════
// HealthHandler — returns {"status":"ok"} for health checks
//
// Used by load balancers and orchestrators (k8s liveness/readiness
// probes).  Always returns 200 with a minimal JSON body.
// ═══════════════════════════════════════════════════════════════════

class HealthHandler : public RequestHandler {
public:
    Response Handle(const Context& ctx) override;
};
