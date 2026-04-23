#pragma once

#include "k8s_types.hpp"

#include <optional>
#include <string>
#include <vector>

// Returns the name of the best node for the pod, or nullopt if none fits.
std::optional<std::string> schedule(const Pod&               pod,
                                     const std::vector<Node>& nodes);

// ---------------------------------------------------------------------------
// Filter predicates (each returns true if the node is acceptable)
// ---------------------------------------------------------------------------

bool filter_node_ready(const Node& node);
bool filter_node_schedulable(const Node& node);
bool filter_node_selector(const Pod& pod, const Node& node);
bool filter_taints(const Pod& pod, const Node& node);
bool filter_resources(const Pod& pod, const Node& node);
