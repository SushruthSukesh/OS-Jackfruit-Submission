OS Jackfruit Container Runtime
👥 Team Information
Name	SRN
Sushruth Sukesh	PES1UG24CS486
Sujay Hegde	PES1UG24CS478
⚙️ Build, Load, and Run Instructions
Prerequisites
Ubuntu 22.04 / 24.04 VM (Secure Boot OFF)
Not supported on WSL
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
🔨 Build
cd boilerplate
make

For CI-safe build (user-space only):

make -C boilerplate ci
🧩 Load Kernel Module
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor
📁 Prepare Root Filesystem
cd boilerplate
mkdir rootfs-base

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

cp memory_hog cpu_hog io_pulse rootfs-alpha/
cp memory_hog cpu_hog io_pulse rootfs-beta/
🚀 Running the System
Start Supervisor
cd boilerplate
sudo ./engine supervisor ./rootfs-base
Launch Containers (new terminal)
cd boilerplate

sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
Useful Commands
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine run gamma ./rootfs-alpha "/cpu_hog 10" --nice 5
sudo ./engine stop alpha
sudo ./engine stop beta
🧪 Experiments
⚡ Scheduling
sudo ./engine run cpu-high ./rootfs-alpha "/cpu_hog 15" --nice -10 &
sudo ./engine run cpu-low ./rootfs-beta "/cpu_hog 15" --nice 10 &
wait
💾 Memory Limits
sudo ./engine run mem-soft ./rootfs-alpha "/memory_hog 8 1000" --soft-mib 40 --hard-mib 80

sudo ./engine run mem-hard ./rootfs-alpha "/memory_hog 8 500" --soft-mib 20 --hard-mib 48
dmesg | grep container_monitor
🧹 Cleanup
sudo kill $(pgrep -f "engine supervisor")
sudo rmmod monitor
ps aux | grep -E "engine|defunct"
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

🧠 Key Concepts
Namespaces → Process and system isolation
Supervisor → Lifecycle management + zombie prevention
IPC → UNIX sockets (control), pipes (logging)
Kernel Module → Memory monitoring and enforcement
CFS Scheduler → CPU allocation based on nice values
📌 Conclusion

This project demonstrates core OS concepts including process management, scheduling, memory monitoring, and inter-process communication through a practical container runtime implementation.
