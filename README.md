Multi-Container Runtime
Team Information
Name	SRN
Sushruth Sukesh	PES1UG24CS486
Sujay Hegde	PES1UG24CS478
Build, Load, and Run Instructions
Prerequisites
Ubuntu 22.04 or 24.04 VM (Secure Boot OFF)
No WSL
Install Dependencies
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
Build
cd boilerplate
make          # builds engine, workloads, and kernel module
For CI-safe build (user-space only):

make -C boilerplate ci
Load Kernel Module
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor   # verify device exists
Prepare Root Filesystem
cd boilerplate
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Copy workload binaries into rootfs
cp memory_hog cpu_hog io_pulse rootfs-alpha/
cp memory_hog cpu_hog io_pulse rootfs-beta/
Start Supervisor
cd boilerplate
sudo ./engine supervisor ./rootfs-base
Launch Containers (from another terminal)
cd boilerplate

# Start two containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# List tracked containers
sudo ./engine ps

# View container logs
sudo ./engine logs alpha

# Run a workload in foreground mode
sudo ./engine run gamma ./rootfs-alpha "/cpu_hog 10" --nice 5

# Stop containers
sudo ./engine stop alpha
sudo ./engine stop beta
Scheduler Experiments
# Experiment 1: CPU-bound with different nice values
sudo ./engine run cpu-high ./rootfs-alpha "/cpu_hog 15" --nice -10 &
sudo ./engine run cpu-low ./rootfs-beta "/cpu_hog 15" --nice 10 &
wait

# Experiment 2: CPU-bound vs I/O-bound concurrently
sudo ./engine run cpu-test ./rootfs-alpha "/cpu_hog 15" --nice 0 &
sudo ./engine run io-test ./rootfs-beta "/io_pulse 30 200" --nice 0 &
wait
Memory Limit Testing
# Test soft limit warning (allocates 8 MiB/sec, soft at 40 MiB)
sudo ./engine run mem-soft ./rootfs-alpha "/memory_hog 8 1000" --soft-mib 40 --hard-mib 80

# Test hard limit kill (allocates 8 MiB/sec, hard at 48 MiB)
sudo ./engine run mem-hard ./rootfs-alpha "/memory_hog 8 500" --soft-mib 20 --hard-mib 48

# Check kernel logs for limit events
dmesg | grep container_monitor
Cleanup
# Stop supervisor (Ctrl+C or from another terminal):
sudo kill $(pgrep -f "engine supervisor")

# Unload kernel module
sudo rmmod monitor

# Verify no zombies
ps aux | grep -E "engine|defunct"
Demo with Screenshots
1. Multi-Container Supervision
Multi-container supervision
<img width="1566" height="467" alt="screenshot 1" src="https://github.com/user-attachments/assets/027f5315-c741-4600-8467-07d3ff484413" />


Caption. A single engine supervisor process (PID 17763) parents two concurrent container children (PIDs 17780, 17788) as shown by pstree. The supervisor log confirms Started container alpha and Started container beta under the same supervisor.

2. Metadata Tracking
Engine ps output
<img width="727" height="88" alt="screenshot 2" src="https://github.com/user-attachments/assets/1d2919dc-3d0e-4713-a038-30a6ecd2a227" />


Caption. sudo ./engine ps prints the supervisor's per-container metadata table: container ID, host PID, state, soft/hard memory limits (MiB), exit code, and terminating signal. Both alpha (soft=48, hard=80) and beta (soft=64, hard=96) are running with no exit code yet.

3. Bounded-Buffer Logging
Captured container logs
<img width="998" height="581" alt="screenshot 3" src="https://github.com/user-attachments/assets/3faf8f86-f14e-4737-b08c-897885f26d16" />


Caption. Container output produced by sh -c "for i in 1..5; do echo log-line-$i; sleep 1; done" flows through the pipe → producer thread → bounded buffer → consumer thread → per-container log file. engine logs logger retrieves the five captured lines intact, demonstrating the pipeline operates end-to-end with no drops.

4. CLI and IPC
CLI client and supervisor response over UNIX socket
<img width="808" height="343" alt="screenshot 4" src="https://github.com/user-attachments/assets/e1cb653c-8d80-445e-baec-519a55798a80" />


Caption. The short-lived CLI process (engine start demo-cli ...) opens a SOCK_STREAM UNIX-domain connection to /tmp/mini_runtime.sock, sends a control_request_t, and prints Container demo-cli started. On the supervisor side, the corresponding log line [supervisor] Started container demo-cli (pid 17425) confirms the request was received, parsed, and acted upon — demonstrating the second IPC path distinct from the logging pipes.

5. Soft-Limit Warning
Kernel soft-limit warnings in dmesg
<img width="1085" height="237" alt="screenshot 5" src="https://github.com/user-attachments/assets/32b75de5-2111-4783-bcd8-a7caeb5c0f5c" />


Caption. The kernel module [container_monitor] emits a SOFT LIMIT event the first time a tracked process exceeds its soft threshold. Each line records the container ID, host PID, current RSS bytes, and the configured soft limit (e.g., rss=42541056 limit=41943040 ≈ 40.5 MiB over a 40 MiB soft cap).

6. Hard-Limit Enforcement
Hard-limit kill in dmesg and ps reflects killed state
<img width="1082" height="43" alt="screenshot 6" src="https://github.com/user-attachments/assets/eed545e0-f167-40e8-88cd-59bd19ff2bef" />


Caption. When RSS crosses the hard limit the module calls send_sig(SIGKILL, task, 1) and removes the entry; dmesg records HARD LIMIT container=mem-hard pid=17511 rss=59318272 limit=50331648 (≈56 MiB over a 48 MiB hard cap). engine ps then reflects the kill with state=killed exit=137 sig=9 for both mem-hard and mem-soft, demonstrating the required attribution path (SIGKILL without stop_requested → hard_limit_killed).

7. Scheduling Experiment
Nice value comparison timings
<img width="1260" height="263" alt="screenshot 7" src="https://github.com/user-attachments/assets/f63d6787-6340-4cdf-bab5-9eb8b40c83af" />


Caption. Two identical cpu_hog workloads run concurrently: cpu-high at --nice -10 finishes in 9.810 s real while cpu-low at --nice +10 takes 19.811 s real — a ~2:1 wall-clock ratio. This is exactly what CFS's weight-based proportional share predicts: lower nice → higher weight → larger slice of the contended CPU, so the high-priority container finishes while the low-priority one continues.

8. Clean Teardown
Clean shutdown — no zombies, supervisor exit logs
<img width="860" height="202" alt="screenshot 8" src="https://github.com/user-attachments/assets/99c6cae4-d248-4c12-a901-9057559b78d4" />


Caption. After engine stop alpha and engine stop beta, engine ps shows the correct attribution: alpha/beta/demo-cli are stopped (supervisor-initiated) while mem-hard/mem-soft remain killed (kernel SIGKILL). ps -ef | awk '$3 == 1 && /defunct/' returns no rows, confirming no zombie children of init/supervisor. The supervisor's own log ends with every container's final state recorded.

Engineering Analysis
1. Isolation Mechanisms
Our runtime achieves process and filesystem isolation through Linux namespaces and chroot:

PID Namespace (CLONE_NEWPID): Each container gets its own PID namespace via clone(). Inside the container, the process sees itself as PID 1. The host kernel maintains a mapping between the container's PID namespace and the global PID namespace, so the supervisor can track and signal child processes using host PIDs. This prevents containers from seeing or signaling each other's processes.

UTS Namespace (CLONE_NEWUTS): Each container has its own hostname, set to the container ID via sethostname(). This isolates the system identity so containers cannot interfere with each other's hostname settings.

Mount Namespace (CLONE_NEWNS): Each container gets a private mount table. We mount /proc inside each container so that tools like ps work correctly. Mount changes inside one container (e.g., mounting filesystems) do not affect other containers or the host.

Filesystem Isolation (chroot): Each container uses chroot() to change its root directory to a dedicated rootfs copy. This prevents the container from accessing host filesystem paths. We chose chroot over pivot_root for simplicity — chroot is sufficient when combined with mount namespace isolation, though pivot_root would provide stronger isolation by completely detaching the old root.

What the host kernel still shares: All containers share the same kernel, network stack (we don't use CLONE_NEWNET), user namespace, and IPC namespace. The kernel's scheduling, memory management, and device drivers are shared across all containers. This is fundamentally different from a VM, where each guest has its own kernel.

2. Supervisor and Process Lifecycle
A long-running parent supervisor is essential because it acts as the central coordination point for multiple container lifecycles:

Process Creation: The supervisor uses clone() with namespace flags to create each container as a child process. The clone() system call creates a new process that shares the parent's address space (like fork) but enters new namespaces. The child then calls chroot, mounts /proc, and execs the target command.

Parent-Child Relationships: The supervisor (parent) maintains a linked list of container_record_t entries, each tracking the container's host PID, state, memory limits, and exit status. This metadata is protected by a pthread mutex because multiple threads (the main event loop, producer threads) may access it concurrently.

Reaping: When a container exits, the kernel sends SIGCHLD to the supervisor. Our SIGCHLD handler simply sets a flag; actual reaping happens in the main event loop via waitpid(-1, &status, WNOHANG) in a loop. This non-blocking approach ensures we reap all exited children without blocking the supervisor. Failing to reap would create zombie processes that consume kernel process table entries.

Signal Delivery: The supervisor distinguishes three exit paths:

Normal exit: Container's command completed — WIFEXITED is true, state becomes CONTAINER_EXITED
Manual stop: Supervisor set stop_requested before sending SIGTERM/SIGKILL — state becomes CONTAINER_STOPPED
Hard-limit kill: Kernel module sent SIGKILL without stop_requested — state becomes CONTAINER_KILLED
This three-way classification is critical for the ps command to report accurate termination reasons.

3. IPC, Threads, and Synchronization
Our project uses two distinct IPC mechanisms:

Path A — Logging Pipes (container → supervisor): Each container's stdout/stderr is connected to a pipe. The write end goes to the child (via dup2); the read end stays with the supervisor. A dedicated producer thread per container reads from this pipe and pushes log_item_t chunks into a shared bounded buffer.

Path B — Unix Domain Socket (CLI → supervisor): The CLI client connects to a Unix domain socket at /tmp/mini_runtime.sock, sends a control_request_t struct, and receives a control_response_t. This is a different mechanism from the logging pipes, providing a clean separation between data-plane (logging) and control-plane (CLI) communication.

Bounded Buffer: The log buffer is a circular array of 16 log_item_t entries with head, tail, and count fields.

Race conditions without synchronization:

Multiple producer threads could simultaneously increment tail and count, causing lost log entries or buffer corruption
A producer writing to a slot while a consumer reads from the same slot would produce garbled log output
Shutdown signaling without proper broadcast could leave threads blocked forever (deadlock)
Synchronization choice: We use a pthread mutex with two condition variables (not_empty, not_full):

Mutex protects all buffer state (head, tail, count, shutting_down)
not_full condition: Producers wait here when the buffer is full; consumers signal after removing an item
not_empty condition: The consumer waits here when the buffer is empty; producers signal after inserting
We chose mutex + condition variables over semaphores because condition variables integrate naturally with the shutting_down flag — we can check the flag inside the critical section and use pthread_cond_broadcast to wake all waiting threads during shutdown. With semaphores, shutdown signaling would require additional coordination.

Metadata lock: A separate pthread_mutex_t metadata_lock protects the container linked list, preventing races between the main event loop (adding/reading containers) and the reaping logic (updating state).

4. Memory Management and Enforcement
RSS (Resident Set Size) measures the amount of physical memory currently mapped into a process's address space. It includes:

Code pages (text segment)
Data pages (heap, stack, globals)
Shared library pages mapped into the process
RSS does not measure:

Swapped-out pages (they're not resident)
Shared memory that's mapped but not yet accessed (demand-paged)
Kernel memory used on behalf of the process (slab allocations, page tables)
File-backed pages that have been evicted from the page cache
Why soft and hard limits are different policies: A soft limit serves as an early warning — the container is using more memory than expected, but the workload might be bursty and could naturally reduce usage. Killing the process at the soft limit would cause unnecessary disruption. The hard limit is a safety boundary — if a process exceeds it, the system is at risk of memory pressure affecting other containers or the host. Immediate termination protects system stability.

Why enforcement belongs in kernel space: User-space monitoring via /proc/<pid>/statm would require periodic polling with inherent latency — a process could allocate and use large amounts of memory between polls. The kernel module runs a timer callback every second and has direct access to get_mm_rss() without parsing procfs, making it more efficient. More importantly, the kernel module can send signals directly via send_sig(SIGKILL, task, 1) without the race conditions inherent in user-space kill() calls where the PID could be reused. The kernel module also naturally handles the case where the user-space supervisor crashes — the timer continues running and enforcing limits independently.

5. Scheduling Behavior
Our experiments use cpu_hog (CPU-bound) and io_pulse (I/O-bound) workloads to observe Linux's Completely Fair Scheduler (CFS) behavior.

Experiment 1: CPU-bound with different nice values

Two cpu_hog instances run concurrently: one at nice -10 (higher priority) and one at nice +10 (lower priority). CFS assigns CPU time proportional to weight, which is derived from nice value. Nice -10 corresponds to a weight of ~9548, while nice +10 corresponds to ~110 — roughly an 87:1 ratio. The high-priority container completes significantly faster because CFS gives it a proportionally larger share of CPU time on each scheduling epoch.

Experiment 2: CPU-bound vs I/O-bound

CFS naturally provides good responsiveness to I/O-bound processes. When io_pulse blocks on write()/fsync(), it voluntarily yields the CPU. When it wakes up, CFS gives it priority because it has accumulated less "virtual runtime" than the CPU-bound cpu_hog. This demonstrates CFS's design goal: I/O-bound interactive processes get low latency while CPU-bound batch processes still make forward progress.

The key scheduling insight: CFS tracks virtual runtime (vruntime) per task, which advances proportionally to wall-clock time but inversely to the task's weight. Lower nice values → higher weight → slower vruntime advancement → more CPU time. I/O-bound tasks accumulate less vruntime during blocking, so they're favored when they become runnable again.

Design Decisions and Tradeoffs
Namespace Isolation
Choice: chroot for filesystem isolation instead of pivot_root.

Tradeoff: chroot is simpler to implement (single syscall, no bind-mount setup) but less secure — a privileged process inside the container could potentially escape via .. traversal or by creating a new root. pivot_root would completely detach the old root filesystem, providing stronger isolation.

Justification: For this project's scope, chroot combined with PID/UTS/mount namespaces provides sufficient isolation. The containers run test workloads, not untrusted code. The simplicity of chroot reduces implementation complexity and potential bugs.

Supervisor Architecture
Choice: Single-threaded event loop with select() for the control plane, plus per-container producer threads for logging.

Tradeoff: The single-threaded control plane means that a long-running CMD_RUN blocks the supervisor from accepting other CLI requests until the container exits. A multi-threaded or event-driven control plane would allow concurrent request handling.

Justification: The select() approach with 1-second timeouts keeps the implementation simple while still allowing periodic child reaping. The CMD_RUN blocking behavior matches the specified semantics (block until container exits). For the expected workload (a few containers), this is adequate.

IPC / Logging
Choice: Unix domain socket for control plane; pipes for logging with a bounded circular buffer (16 slots, mutex + condition variables).

Tradeoff: Unix domain sockets require connection setup per CLI command (connect/send/recv/close). A FIFO or shared memory approach could avoid this overhead, but Unix sockets provide reliable, bidirectional, connection-oriented communication with built-in framing.

Justification: Unix domain sockets are the most natural fit for request-response CLI interaction. The bounded buffer with 16 slots balances memory usage against throughput — a larger buffer would reduce the chance of producer blocking but consume more memory. The mutex + condition variable approach is the standard, well-understood solution for bounded-buffer producer-consumer synchronization.

Kernel Monitor
Choice: Mutex (not spinlock) for protecting the monitored entry list. Timer callback uses mutex_trylock to avoid blocking.

Tradeoff: Using a mutex means the timer callback might skip a monitoring cycle if the lock is held by an ioctl handler. A spinlock would guarantee the timer always runs but would prevent the ioctl handler from sleeping (needed for kmalloc(GFP_KERNEL)).

Justification: The 1-second timer interval means missing one cycle has negligible impact. The ioctl path benefits from GFP_KERNEL allocation (which can sleep and reclaim memory), which is not possible under a spinlock. The mutex approach is simpler and less prone to bugs than a spinlock + GFP_ATOMIC combination.

Scheduling Experiments
Choice: Compare cpu_hog instances at different nice values, and compare cpu_hog vs io_pulse at the same nice value.

Tradeoff: Nice values provide coarse-grained control. sched_setaffinity would allow CPU pinning for more controlled experiments, but adds complexity.

Justification: Nice values are the simplest mechanism to demonstrate CFS scheduling behavior. The two experiments cover the key scheduling concepts: priority-based CPU time allocation and I/O-bound vs CPU-bound responsiveness.

Scheduler Experiment Results
Experiment 1: CPU-Bound Workloads at Different Priorities
Two identical cpu_hog instances are launched concurrently under the same supervisor, differing only in their --nice value:

Container	Workload	Nice Value	Wall-Clock Real Time
cpu-high	cpu_hog	-10	9.810 s
cpu-low	cpu_hog	+10	19.811 s
Measurements captured in screenshot/7_scheduling.png.

What the results show. The high-priority container finishes in roughly half the wall-clock time of the low-priority container (~2.02× ratio). CFS assigns each task a weight derived from its nice value; nice −10 → weight ≈ 9548, nice +10 → weight ≈ 110, so when the two workloads compete for a single CPU the high-priority task gets almost all of the CPU until its own work is done, after which the low-priority task runs essentially alone and takes a further ~10 s to finish its remaining work. This illustrates CFS's proportional-share fairness: it is not strictly priority-preemptive (the low-priority task still makes some progress), but weight drives the ratio of CPU time each task receives while both are runnable.

Experiment 2: CPU-Bound vs I/O-Bound Concurrently
Container	Workload	Nice Value	Configuration
cpu-test	cpu_hog	0	10 s of tight-loop burn
io-test	io_pulse	0	20 iterations, 200 ms
What the results show. With equal nice values, io_pulse maintains a very close-to-200 ms cadence between its write/fsync calls even though cpu_hog is burning a full core in parallel. This happens because io_pulse voluntarily sleeps during I/O, accumulating virtual runtime (vruntime) far more slowly than the CPU-bound task. When it wakes up it has the lowest vruntime of any runnable task and CFS schedules it immediately, giving it near-zero wake-up latency. The CPU-bound task consumes essentially all of the CPU time that the I/O task isn't using. This is exactly the scheduling goal CFS is designed for: responsiveness for interactive/I/O-bound tasks without starving CPU-bound throughput work.

