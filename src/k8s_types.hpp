#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Resource quantities (CPU in millicores, memory in bytes)
// ---------------------------------------------------------------------------

struct ResourceList {
    int64_t cpu_millicores = 0;
    int64_t memory_bytes   = 0;
};

// Parse "100m" → 100,  "2" → 2000,  "500Mi" → 524288000
inline int64_t parse_cpu(const std::string& s) {
    if (s.empty()) return 0;
    if (s.back() == 'm')
        return std::stoll(s.substr(0, s.size() - 1));
    return std::stoll(s) * 1000;
}

inline int64_t parse_memory(const std::string& s) {
    if (s.empty()) return 0;
    if (s.size() > 2 && s.substr(s.size() - 2) == "Ki")
        return std::stoll(s.substr(0, s.size() - 2)) * 1024LL;
    if (s.size() > 2 && s.substr(s.size() - 2) == "Mi")
        return std::stoll(s.substr(0, s.size() - 2)) * 1024LL * 1024;
    if (s.size() > 2 && s.substr(s.size() - 2) == "Gi")
        return std::stoll(s.substr(0, s.size() - 2)) * 1024LL * 1024 * 1024;
    return std::stoll(s);
}

inline ResourceList parse_resource_list(const nlohmann::json& j) {
    ResourceList r;
    if (j.contains("cpu"))    r.cpu_millicores = parse_cpu(j["cpu"].get<std::string>());
    if (j.contains("memory")) r.memory_bytes   = parse_memory(j["memory"].get<std::string>());
    return r;
}

// ---------------------------------------------------------------------------
// ObjectMeta
// ---------------------------------------------------------------------------

struct ObjectMeta {
    std::string name;
    std::string namespc;   // "namespace" is a reserved word in C++
    std::string uid;
    std::string resource_version;
    std::unordered_map<std::string, std::string> labels;
    std::unordered_map<std::string, std::string> annotations;
};

inline ObjectMeta parse_meta(const nlohmann::json& j) {
    ObjectMeta m;
    m.name             = j.value("name", "");
    m.namespc          = j.value("namespace", "");
    m.uid              = j.value("uid", "");
    m.resource_version = j.value("resourceVersion", "");
    if (j.contains("labels"))
        m.labels = j["labels"].get<std::unordered_map<std::string, std::string>>();
    if (j.contains("annotations"))
        m.annotations = j["annotations"].get<std::unordered_map<std::string, std::string>>();
    return m;
}

// ---------------------------------------------------------------------------
// Taint / Toleration
// ---------------------------------------------------------------------------

struct Taint {
    std::string key;
    std::string value;
    std::string effect;  // NoSchedule | PreferNoSchedule | NoExecute
};

struct Toleration {
    std::string key;
    std::string value;
    std::string effect;
    std::string op;  // Equal | Exists
};

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

struct NodeCondition {
    std::string type;    // Ready, MemoryPressure, DiskPressure, …
    std::string status;  // True | False | Unknown
};

struct Node {
    ObjectMeta  meta;
    bool        unschedulable = false;
    std::vector<Taint>         taints;
    std::unordered_map<std::string, std::string> labels;
    ResourceList allocatable;
    std::vector<NodeCondition> conditions;

    bool is_ready() const {
        for (const auto& c : conditions)
            if (c.type == "Ready") return c.status == "True";
        return false;
    }
};

inline Node parse_node(const nlohmann::json& j) {
    Node n;
    n.meta   = parse_meta(j["metadata"]);
    n.labels = n.meta.labels;

    const auto& spec = j.value("spec", nlohmann::json::object());
    n.unschedulable  = spec.value("unschedulable", false);

    if (spec.contains("taints")) {
        for (const auto& t : spec["taints"]) {
            Taint taint;
            taint.key    = t.value("key", "");
            taint.value  = t.value("value", "");
            taint.effect = t.value("effect", "");
            n.taints.push_back(taint);
        }
    }

    const auto& status = j.value("status", nlohmann::json::object());
    if (status.contains("allocatable"))
        n.allocatable = parse_resource_list(status["allocatable"]);

    if (status.contains("conditions")) {
        for (const auto& c : status["conditions"]) {
            NodeCondition cond;
            cond.type   = c.value("type", "");
            cond.status = c.value("status", "");
            n.conditions.push_back(cond);
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Pod
// ---------------------------------------------------------------------------

struct ContainerResources {
    ResourceList requests;
    ResourceList limits;
};

struct Container {
    std::string        name;
    ContainerResources resources;
};

struct Pod {
    ObjectMeta          meta;
    std::string         scheduler_name;
    std::string         node_name;        // empty if unscheduled
    std::string         phase;            // Pending | Running | …
    std::vector<Container>   containers;
    std::vector<Toleration>  tolerations;
    std::unordered_map<std::string, std::string> node_selector;

    ResourceList total_requests() const {
        ResourceList total;
        for (const auto& c : containers) {
            total.cpu_millicores += c.resources.requests.cpu_millicores;
            total.memory_bytes   += c.resources.requests.memory_bytes;
        }
        return total;
    }
};

inline Pod parse_pod(const nlohmann::json& j) {
    Pod p;
    p.meta = parse_meta(j["metadata"]);

    const auto& spec = j.value("spec", nlohmann::json::object());
    p.scheduler_name = spec.value("schedulerName", "default-scheduler");
    p.node_name      = spec.value("nodeName", "");

    if (spec.contains("nodeSelector"))
        p.node_selector = spec["nodeSelector"]
                              .get<std::unordered_map<std::string, std::string>>();

    if (spec.contains("tolerations")) {
        for (const auto& t : spec["tolerations"]) {
            Toleration tol;
            tol.key    = t.value("key", "");
            tol.value  = t.value("value", "");
            tol.effect = t.value("effect", "");
            tol.op     = t.value("operator", "Equal");
            p.tolerations.push_back(tol);
        }
    }

    if (spec.contains("containers")) {
        for (const auto& c : spec["containers"]) {
            Container cont;
            cont.name = c.value("name", "");
            if (c.contains("resources")) {
                const auto& res = c["resources"];
                if (res.contains("requests"))
                    cont.resources.requests = parse_resource_list(res["requests"]);
                if (res.contains("limits"))
                    cont.resources.limits = parse_resource_list(res["limits"]);
            }
            p.containers.push_back(cont);
        }
    }

    p.phase = j.value("status", nlohmann::json::object()).value("phase", "");
    return p;
}

// ---------------------------------------------------------------------------
// Watch event
// ---------------------------------------------------------------------------

enum class WatchEventType { ADDED, MODIFIED, DELETED, UNKNOWN };

struct WatchEvent {
    WatchEventType type = WatchEventType::UNKNOWN;
    nlohmann::json object;
};

inline WatchEvent parse_watch_event(const std::string& line) {
    WatchEvent ev;
    try {
        auto j = nlohmann::json::parse(line);
        std::string t = j.value("type", "");
        if      (t == "ADDED")    ev.type = WatchEventType::ADDED;
        else if (t == "MODIFIED") ev.type = WatchEventType::MODIFIED;
        else if (t == "DELETED")  ev.type = WatchEventType::DELETED;
        ev.object = j.value("object", nlohmann::json::object());
    } catch (...) {}
    return ev;
}
