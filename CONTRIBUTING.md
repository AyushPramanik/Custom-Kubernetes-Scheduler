# Contributing

## Before you start

Check the open issues first. If you're adding something non-trivial, open an issue before writing code — avoids wasted effort if the direction isn't right.

---

## Setup

```bash
# clone
git clone https://github.com/your-username/custom-k8s-scheduler
cd custom-k8s-scheduler

# configure + build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

You'll need `cmake ≥ 3.16`, `g++/clang++ with C++17`, and `libcurl-dev`. nlohmann/json is fetched automatically.

---

## Making changes

**Adding a filter**

Write it in `src/scheduler.cpp` as a free function:

```cpp
bool filter_xyz(const Pod& pod, const Node& node) {
    // return false to reject the node
}
```

Then call it inside `schedule()` in the filter phase block. Add the declaration to `scheduler.hpp`.

**Adding a scoring function**

Same file, same pattern. Return an `int` in `[0, 100]` and combine it with the existing score however makes sense for your use case.

**Touching the API layer**

The HTTP client is in `http_client.hpp/cpp`. The K8s-specific calls (list, watch, bind, events) are in `k8s_client.hpp/cpp`. Keep them separate — the HTTP client knows nothing about Kubernetes.

---

## Commits

One logical change per commit. Follow the existing style:

```
feat: <what you added>
fix: <what you corrected>
chore: <build, deps, config>
docs: <readme, comments>
```

No "WIP" commits in PRs. Squash before opening if needed.

---

## Pull requests

- Target `main`
- Keep the diff small — big PRs take longer to review and are harder to reason about
- If you changed scheduling behaviour, describe what you tested against (minikube, kind, real cluster)
- Update the README filter table if you added a filter

---

## What's in scope

- New filter predicates (pod affinity, topology spread, custom labels)
- Better scoring strategies
- Config file support instead of env vars
- Metrics endpoint (Prometheus)
- Proper resource accounting (subtracting already-running pod requests from node allocatable)

## What's out of scope

- Replacing libcurl with a full HTTP/2 client
- Reimplementing as a controller using controller-runtime or kubebuilder
- Windows support
