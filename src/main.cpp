#include "k8s_client.hpp"
#include "k8s_types.hpp"
#include "scheduler.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

static volatile bool g_running = true;

static void handle_signal(int) { g_running = false; }

// ---------------------------------------------------------------------------
// Schedule a single pod: pick node, bind, emit event
// ---------------------------------------------------------------------------

static void handle_pod(const K8sClient& client,
                        const Pod&       pod,
                        const std::vector<Node>& nodes) {
    std::cout << "[scheduler] scheduling pod "
              << pod.meta.namespc << "/" << pod.meta.name << "\n";

    auto chosen = schedule(pod, nodes);
    if (!chosen) {
        client.emit_event(pod, "FailedScheduling",
                          "No feasible node found", "Warning");
        return;
    }

    std::cout << "[scheduler] binding " << pod.meta.name
              << " -> " << *chosen << "\n";

    if (client.bind_pod(pod.meta.namespc, pod.meta.name, pod.meta.uid, *chosen)) {
        client.emit_event(pod, "Scheduled",
                          "Successfully assigned to node " + *chosen);
    } else {
        client.emit_event(pod, "FailedScheduling",
                          "Bind to " + *chosen + " failed", "Warning");
    }
}

// ---------------------------------------------------------------------------
// Main scheduling loop
// ---------------------------------------------------------------------------

int run_scheduler(const std::string& scheduler_name,
                  const std::string& namespace_filter) {
    // Build client — prefer in-cluster, fall back to KUBECONFIG token env var
    std::unique_ptr<K8sClient> client;

    const char* token_env = std::getenv("KUBE_TOKEN");
    const char* api_env   = std::getenv("KUBE_API_SERVER");

    if (token_env && api_env) {
        std::string ca = std::getenv("KUBE_CA_CERT") ? std::getenv("KUBE_CA_CERT") : "";
        client = std::make_unique<K8sClient>(api_env, token_env, ca, ca.empty() ? false : true);
        std::cout << "[scheduler] using env-var credentials\n";
    } else {
        try {
            auto c = K8sClient::from_in_cluster();
            client = std::make_unique<K8sClient>(std::move(c));
            std::cout << "[scheduler] using in-cluster credentials\n";
        } catch (const std::exception& e) {
            std::cerr << "[scheduler] could not load credentials: " << e.what() << "\n"
                      << "  Set KUBE_API_SERVER, KUBE_TOKEN (and optionally KUBE_CA_CERT)\n";
            return 1;
        }
    }

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::string resource_version;

    while (g_running) {
        // Refresh node list every iteration (cheap; nodes rarely change)
        auto nodes = client->list_nodes();
        std::cout << "[scheduler] found " << nodes.size() << " node(s)\n";

        // Drain any already-pending pods first
        auto pending = client->list_pending_pods(scheduler_name, namespace_filter);
        for (const auto& pod : pending) {
            if (!g_running) break;
            handle_pod(*client, pod, nodes);
        }

        // Then watch for new pods until the stream ends (K8s times out ~5 min)
        std::cout << "[scheduler] watching for new pods (rv=" << resource_version << ")…\n";

        client->watch_pods(
            scheduler_name, namespace_filter, resource_version,
            [&](WatchEvent ev) {
                if (!g_running) return;
                if (ev.type == WatchEventType::ADDED ||
                    ev.type == WatchEventType::MODIFIED) {
                    Pod pod = parse_pod(ev.object);
                    if (!pod.node_name.empty()) return;  // already scheduled elsewhere
                    // Refresh nodes before each decision
                    nodes = client->list_nodes();
                    handle_pod(*client, pod, nodes);
                }
                // Track resource version to resume watch after reconnect
                if (ev.object.contains("metadata"))
                    resource_version = ev.object["metadata"].value("resourceVersion", "");
            }
        );

        if (!g_running) break;

        // Brief pause before reconnecting to the watch stream
        std::cout << "[scheduler] watch stream ended, reconnecting in 2s…\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    std::cout << "[scheduler] shutting down\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string scheduler_name   = "custom-scheduler";
    std::string namespace_filter;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--name" || arg == "-n") && i + 1 < argc)
            scheduler_name = argv[++i];
        else if ((arg == "--namespace" || arg == "-ns") && i + 1 < argc)
            namespace_filter = argv[++i];
        else {
            std::cerr << "Usage: " << argv[0]
                      << " [--name <scheduler-name>] [--namespace <ns>]\n";
            return 1;
        }
    }

    std::cout << "[scheduler] starting '" << scheduler_name << "'"
              << (namespace_filter.empty() ? "" : " in ns=" + namespace_filter)
              << "\n";

    return run_scheduler(scheduler_name, namespace_filter);
}
