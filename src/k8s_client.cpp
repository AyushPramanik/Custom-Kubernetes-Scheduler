#include "k8s_client.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

static constexpr const char* IN_CLUSTER_TOKEN_PATH =
    "/var/run/secrets/kubernetes.io/serviceaccount/token";
static constexpr const char* IN_CLUSTER_CA_PATH =
    "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";
static constexpr const char* IN_CLUSTER_API =
    "https://kubernetes.default.svc";

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read: " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Remove trailing newline from token file
static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

K8sClient::K8sClient(const std::string& api_server,
                     const std::string& token,
                     const std::string& ca_cert_path,
                     bool verify_tls)
    : api_server_(api_server)
{
    HttpConfig cfg;
    cfg.base_url      = api_server;
    cfg.bearer_token  = token;
    cfg.ca_cert_path  = ca_cert_path;
    cfg.verify_tls    = verify_tls;
    http_ = std::make_unique<HttpClient>(std::move(cfg));
}

K8sClient K8sClient::from_in_cluster() {
    std::string token = trim(read_file(IN_CLUSTER_TOKEN_PATH));
    return K8sClient(IN_CLUSTER_API, token, IN_CLUSTER_CA_PATH, true);
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

std::vector<Node> K8sClient::list_nodes() const {
    auto resp = http_->get("/api/v1/nodes");
    if (!resp.ok()) {
        std::cerr << "[k8s] list_nodes failed: " << resp.status_code
                  << " " << resp.body << "\n";
        return {};
    }

    std::vector<Node> nodes;
    auto j = nlohmann::json::parse(resp.body);
    for (const auto& item : j["items"])
        nodes.push_back(parse_node(item));
    return nodes;
}

// ---------------------------------------------------------------------------
// Pods
// ---------------------------------------------------------------------------

std::vector<Pod> K8sClient::list_pending_pods(const std::string& scheduler_name,
                                               const std::string& namespc) const {
    std::string path = namespc.empty()
        ? "/api/v1/pods?fieldSelector=status.phase%3DPending"
        : "/api/v1/namespaces/" + namespc + "/pods?fieldSelector=status.phase%3DPending";

    auto resp = http_->get(path);
    if (!resp.ok()) {
        std::cerr << "[k8s] list_pending_pods failed: " << resp.status_code << "\n";
        return {};
    }

    std::vector<Pod> pods;
    auto j = nlohmann::json::parse(resp.body);
    for (const auto& item : j["items"]) {
        Pod p = parse_pod(item);
        // Only claim pods that belong to this scheduler and are truly unscheduled
        if (p.scheduler_name == scheduler_name && p.node_name.empty())
            pods.push_back(std::move(p));
    }
    return pods;
}

void K8sClient::watch_pods(const std::string& scheduler_name,
                            const std::string& namespc,
                            const std::string& resource_version,
                            PodWatchCallback   cb) const {
    std::string rv_param = resource_version.empty()
        ? "" : "&resourceVersion=" + resource_version;

    std::string path = namespc.empty()
        ? "/api/v1/pods?watch=true&fieldSelector=status.phase%3DPending" + rv_param
        : "/api/v1/namespaces/" + namespc +
          "/pods?watch=true&fieldSelector=status.phase%3DPending" + rv_param;

    http_->watch(path, [&](const std::string& line) -> bool {
        WatchEvent ev = parse_watch_event(line);
        if (ev.type == WatchEventType::UNKNOWN) return true;  // skip malformed

        // Filter: only unscheduled pods for our scheduler
        if (ev.object.contains("spec")) {
            std::string sched = ev.object["spec"].value("schedulerName", "");
            std::string node  = ev.object["spec"].value("nodeName", "");
            if (sched != scheduler_name || !node.empty()) return true;
        }
        cb(std::move(ev));
        return true;
    });
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

bool K8sClient::bind_pod(const std::string& namespc,
                          const std::string& pod_name,
                          const std::string& pod_uid,
                          const std::string& node_name) const {
    nlohmann::json binding = {
        {"apiVersion", "v1"},
        {"kind", "Binding"},
        {"metadata", {
            {"name", pod_name},
            {"namespace", namespc}
        }},
        {"target", {
            {"apiVersion", "v1"},
            {"kind", "Node"},
            {"name", node_name}
        }}
    };

    std::string path = "/api/v1/namespaces/" + namespc +
                       "/pods/" + pod_name + "/binding";
    (void)pod_uid;  // included for caller clarity; K8s derives it from pod name

    auto resp = http_->post(path, binding.dump());
    if (!resp.ok()) {
        std::cerr << "[k8s] bind_pod " << pod_name << " -> " << node_name
                  << " failed: " << resp.status_code << " " << resp.body << "\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void K8sClient::emit_event(const Pod&         pod,
                            const std::string& reason,
                            const std::string& message,
                            const std::string& type) const {
    using namespace std::chrono;
    auto now  = system_clock::now();
    auto secs = duration_cast<seconds>(now.time_since_epoch()).count();

    // RFC3339 timestamp (simplified, UTC only)
    std::time_t t = static_cast<std::time_t>(secs);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));

    nlohmann::json ev = {
        {"apiVersion", "v1"},
        {"kind", "Event"},
        {"metadata", {
            {"name", pod.meta.name + "." + reason},
            {"namespace", pod.meta.namespc}
        }},
        {"involvedObject", {
            {"apiVersion", "v1"},
            {"kind", "Pod"},
            {"name", pod.meta.name},
            {"namespace", pod.meta.namespc},
            {"uid", pod.meta.uid}
        }},
        {"reason",         reason},
        {"message",        message},
        {"type",           type},
        {"firstTimestamp", ts},
        {"lastTimestamp",  ts},
        {"count",          1},
        {"reportingComponent", "custom-scheduler"},
        {"reportingInstance",  "custom-scheduler"}
    };

    std::string path = "/api/v1/namespaces/" + pod.meta.namespc + "/events";
    http_->post(path, ev.dump());  // best-effort; ignore errors
}
