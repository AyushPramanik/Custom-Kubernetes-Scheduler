#pragma once

#include "http_client.hpp"
#include "k8s_types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Callback type for the pod watch stream
using PodWatchCallback = std::function<void(WatchEvent)>;

class K8sClient {
public:
    // Build from explicit config (e.g. kubeconfig token)
    K8sClient(const std::string& api_server,
              const std::string& token,
              const std::string& ca_cert_path = "",
              bool verify_tls = true);

    // Build from in-cluster service-account credentials
    static K8sClient from_in_cluster();

    // --- Nodes ---
    std::vector<Node> list_nodes() const;

    // --- Pods ---
    // Returns unscheduled pods whose schedulerName matches scheduler_name
    std::vector<Pod> list_pending_pods(const std::string& scheduler_name,
                                       const std::string& namespc = "") const;

    // Watch for pod events; calls cb for each event.
    // Blocks until the stream ends or is interrupted.
    void watch_pods(const std::string& scheduler_name,
                    const std::string& namespc,
                    const std::string& resource_version,
                    PodWatchCallback   cb) const;

    // --- Binding ---
    // Bind pod to node, returns true on success
    bool bind_pod(const std::string& namespc,
                  const std::string& pod_name,
                  const std::string& pod_uid,
                  const std::string& node_name) const;

    // --- Events ---
    // Emit a normal/warning event for a pod (for observability)
    void emit_event(const Pod&         pod,
                    const std::string& reason,
                    const std::string& message,
                    const std::string& type = "Normal") const;

private:
    std::unique_ptr<HttpClient> http_;
    std::string                 api_server_;
};
