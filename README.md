# custom-k8s-scheduler

A Kubernetes scheduler written in C++17. Replaces the default `kube-scheduler` for pods that opt in via `schedulerName: custom-scheduler`.

It connects directly to the API server, watches for unscheduled pods, runs them through a filter/score pipeline, and binds them to the best node — no controller-runtime, no Go, no operator SDK.

---

## How it works

```
API Server
    │
    │  watch /api/v1/pods?fieldSelector=status.phase=Pending
    ▼
┌─────────────────────────────────────────────────────┐
│                   Watch Loop                        │
│                                                     │
│  1. Drain already-pending pods on startup           │
│  2. Stream new ADDED / MODIFIED events              │
│  3. Reconnect with last resourceVersion on timeout  │
└──────────────────────┬──────────────────────────────┘
                       │  pod event
                       ▼
              ┌────────────────┐
              │  schedulerName │  ──── not ours? skip
              │  == ours?      │
              └───────┬────────┘
                      │
                      ▼
             list_nodes()  (fresh every decision)
                      │
                      ▼
          ┌───────────────────────┐
          │     Filter Phase      │
          │                       │
          │  ① node Ready?        │
          │  ② not cordoned?      │
          │  ③ nodeSelector match │
          │  ④ NoSchedule taints  │
          │  ⑤ enough CPU + RAM   │
          └──────────┬────────────┘
                     │ feasible nodes
                     ▼
          ┌───────────────────────┐
          │     Score Phase       │
          │                       │
          │  least-allocated      │
          │  (free CPU% + RAM%)/2 │
          └──────────┬────────────┘
                     │ winner
                     ▼
          POST /pods/{name}/binding
                     │
                     ▼
          emit Event → pod scheduled
```

---

## Filter pipeline

Each filter is a pure function `(Pod, Node) → bool`. All must pass for a node to be considered.

| # | Filter | What it checks |
|---|--------|----------------|
| 1 | `filter_node_ready` | `conditions[Ready].status == True` |
| 2 | `filter_node_schedulable` | `spec.unschedulable != true` |
| 3 | `filter_node_selector` | every `pod.nodeSelector` label exists on the node |
| 4 | `filter_taints` | all `NoSchedule` taints have a matching toleration |
| 5 | `filter_resources` | `pod.requests ≤ node.allocatable` for CPU and memory |

The score is **least-allocated**: `(free_cpu% + free_mem%) / 2` normalised to `[0, 100]`. Highest score wins.

---

## Project layout

```
.
├── CMakeLists.txt
├── Dockerfile
├── deploy/
│   ├── rbac.yaml                  # ServiceAccount + ClusterRole + binding
│   └── scheduler-deployment.yaml
└── src/
    ├── http_client.hpp / .cpp     # libcurl wrapper (GET, POST, streaming watch)
    ├── k8s_types.hpp              # Pod, Node, ResourceList, WatchEvent
    ├── k8s_client.hpp / .cpp      # API calls: list, watch, bind, events
    ├── scheduler.hpp / .cpp       # filter + score pipeline
    └── main.cpp                   # watch loop, signal handling, credential loading
```

Dependencies: `libcurl`, [`nlohmann/json`](https://github.com/nlohmann/json) (fetched automatically by CMake).

---

## Build

```bash
# configure (downloads nlohmann/json via FetchContent)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# compile
cmake --build build --parallel
```

Requires: `cmake ≥ 3.16`, `g++ / clang++ with C++17`, `libcurl-dev`.

On Debian/Ubuntu:
```bash
apt install cmake g++ libcurl4-openssl-dev
```

---

## Run

### Against a remote cluster (dev)

```bash
export KUBE_API_SERVER=https://your-cluster:6443
export KUBE_TOKEN=$(kubectl create token custom-scheduler -n kube-system)
export KUBE_CA_CERT=/path/to/ca.crt

./build/scheduler --name custom-scheduler
```

### In-cluster

The binary reads `/var/run/secrets/kubernetes.io/serviceaccount/` automatically when those env vars aren't set.

```bash
./build/scheduler --name custom-scheduler --namespace default
```

---

## Deploy to Kubernetes

```bash
# 1. build and push the image
docker build -t your-registry/custom-scheduler:latest .
docker push your-registry/custom-scheduler:latest

# 2. apply RBAC first
kubectl apply -f deploy/rbac.yaml

# 3. deploy the scheduler
kubectl apply -f deploy/scheduler-deployment.yaml
```

Edit `deploy/scheduler-deployment.yaml` and replace `your-registry/custom-scheduler:latest` with your actual image.

---

## Opt a pod into this scheduler

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: my-pod
spec:
  schedulerName: custom-scheduler   # <-- this is all you need
  containers:
    - name: app
      image: nginx
      resources:
        requests:
          cpu: "100m"
          memory: "128Mi"
```

Pods without this field continue to use `default-scheduler` unchanged.

---

## Watch it work

```
[scheduler] starting 'custom-scheduler'
[scheduler] using in-cluster credentials
[scheduler] found 3 node(s)
[scheduler] watching for new pods (rv=)…
[scheduler] scheduling pod default/my-pod
[scheduler]   node worker-1 score=74
[scheduler]   node worker-2 score=61
[scheduler]   node worker-3 score=88
[scheduler] binding my-pod -> worker-3
```

---

## Credentials lookup order

```
KUBE_API_SERVER + KUBE_TOKEN set?
        │
       yes ──► use env vars (+ KUBE_CA_CERT if set)
        │
       no  ──► read in-cluster service account token
                /var/run/secrets/kubernetes.io/serviceaccount/
                  ├── token
                  └── ca.crt
```

---

## Extending it

The two places you'd touch for custom scheduling behaviour:

- **Add a filter** — write a `bool filter_xyz(const Pod&, const Node&)` in `scheduler.cpp` and call it in `schedule()`.
- **Change the score** — replace or supplement `score_least_allocated` in `scheduler.cpp`. The score function signature is `int(const Pod&, const Node&)` returning `[0, 100]`.

The HTTP layer and watch loop don't need to change for either.
