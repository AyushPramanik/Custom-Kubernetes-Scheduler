#include "scheduler.hpp"

#include <algorithm>
#include <iostream>

// ---------------------------------------------------------------------------
// Filter: node must be Ready
// ---------------------------------------------------------------------------

bool filter_node_ready(const Node& node) {
    return node.is_ready();
}

// ---------------------------------------------------------------------------
// Filter: node must not be cordoned
// ---------------------------------------------------------------------------

bool filter_node_schedulable(const Node& node) {
    return !node.unschedulable;
}

// ---------------------------------------------------------------------------
// Filter: pod's nodeSelector labels must all exist on the node
// ---------------------------------------------------------------------------

bool filter_node_selector(const Pod& pod, const Node& node) {
    for (const auto& [k, v] : pod.node_selector) {
        auto it = node.labels.find(k);
        if (it == node.labels.end() || it->second != v)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Filter: NoSchedule taints must be tolerated
// ---------------------------------------------------------------------------

static bool toleration_matches(const Toleration& tol, const Taint& taint) {
    // Toleration with empty key tolerates all taints (if op == Exists)
    if (tol.op == "Exists" && tol.key.empty()) return true;
    if (tol.key != taint.key) return false;
    if (!tol.effect.empty() && tol.effect != taint.effect) return false;
    if (tol.op == "Exists") return true;
    return tol.value == taint.value;
}

bool filter_taints(const Pod& pod, const Node& node) {
    for (const auto& taint : node.taints) {
        if (taint.effect != "NoSchedule") continue;
        bool tolerated = false;
        for (const auto& tol : pod.tolerations) {
            if (toleration_matches(tol, taint)) { tolerated = true; break; }
        }
        if (!tolerated) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Filter: node must have enough allocatable resources
// (In a real scheduler you'd subtract already-scheduled pod requests here.
//  For simplicity we check raw allocatable vs. pod requests.)
// ---------------------------------------------------------------------------

bool filter_resources(const Pod& pod, const Node& node) {
    ResourceList req = pod.total_requests();
    return req.cpu_millicores <= node.allocatable.cpu_millicores &&
           req.memory_bytes   <= node.allocatable.memory_bytes;
}

// ---------------------------------------------------------------------------
// Score: least-allocated (higher score = more free resources = preferred)
// Normalised to [0, 100]
// ---------------------------------------------------------------------------

static int score_least_allocated(const Pod& pod, const Node& node) {
    ResourceList req  = pod.total_requests();
    int64_t      alloc_cpu = node.allocatable.cpu_millicores;
    int64_t      alloc_mem = node.allocatable.memory_bytes;

    if (alloc_cpu == 0 || alloc_mem == 0) return 0;

    int64_t cpu_free_pct = (alloc_cpu - req.cpu_millicores) * 100 / alloc_cpu;
    int64_t mem_free_pct = (alloc_mem - req.memory_bytes)   * 100 / alloc_mem;

    // Clamp to [0, 100]
    cpu_free_pct = std::max(0LL, std::min(100LL, cpu_free_pct));
    mem_free_pct = std::max(0LL, std::min(100LL, mem_free_pct));

    return static_cast<int>((cpu_free_pct + mem_free_pct) / 2);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

std::optional<std::string> schedule(const Pod& pod, const std::vector<Node>& nodes) {
    // 1. Filter phase
    std::vector<const Node*> candidates;
    for (const auto& n : nodes) {
        if (!filter_node_ready(n))        { continue; }
        if (!filter_node_schedulable(n))  { continue; }
        if (!filter_node_selector(pod, n)){ continue; }
        if (!filter_taints(pod, n))       { continue; }
        if (!filter_resources(pod, n))    { continue; }
        candidates.push_back(&n);
    }

    if (candidates.empty()) {
        std::cerr << "[scheduler] no feasible node for pod "
                  << pod.meta.namespc << "/" << pod.meta.name << "\n";
        return std::nullopt;
    }

    // 2. Score phase
    const Node* best     = nullptr;
    int         best_score = -1;

    for (const Node* n : candidates) {
        int s = score_least_allocated(pod, *n);
        std::cout << "[scheduler]   node " << n->meta.name << " score=" << s << "\n";
        if (s > best_score) { best_score = s; best = n; }
    }

    return best->meta.name;
}
