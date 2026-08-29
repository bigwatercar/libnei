#!/bin/bash
# fix_runner_types.sh — narrow scoped_refptr<TaskRunner> to precise types
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
set -e

fix_header() {
    local f="$1"
    local io_type="$2"
    local include_needed="$3"
    
    if [ "$include_needed" = "1" ]; then
        sed -i '/^namespace nei {$/i #include <neixx/task/task_runner.h>' "$f"
        sed -i '/^class TaskRunner;$/d' "$f"
    fi
    sed -i "s/scoped_refptr<TaskRunner>/scoped_refptr<${io_type}>/g" "$f"
}

# === Core ===
fix_header "$SRC/include/neixx/task/thread_task_runner_handle.h" SingleThreadTaskRunner 1
fix_header "$SRC/include/neixx/task/timer.h" SequencedTaskRunner 1

# === IO ===
fix_header "$SRC/include/neixx/io/async_file.h" SingleThreadTaskRunner 1
fix_header "$SRC/include/neixx/io/pipe_stream.h" SingleThreadTaskRunner 1
fix_header "$SRC/include/neixx/io/file_stream_adapters.h" SequencedTaskRunner 1
sed -i "s/scoped_refptr<TaskRunner> target_task_runner_/scoped_refptr<SequencedTaskRunner> target_task_runner_/g" "$SRC/include/neixx/io/stream_reader.h"
sed -i "s/scoped_refptr<TaskRunner> target_task_runner_/scoped_refptr<SequencedTaskRunner> target_task_runner_/g" "$SRC/include/neixx/io/stream_writer.h"

# === Net ===
for f in "$SRC/include/neixx/net/tcp_client_socket.h" \
         "$SRC/include/neixx/net/tcp_server_socket.h" \
         "$SRC/include/neixx/net/tls_client_socket.h" \
         "$SRC/include/neixx/net/tls_server_socket.h" \
         "$SRC/include/neixx/net/udp_socket.h"; do
    fix_header "$f" SingleThreadTaskRunner 1
done
sed -i 's/std::function<scoped_refptr<TaskRunner>()>/std::function<scoped_refptr<SingleThreadTaskRunner>()>/g' "$SRC/include/neixx/net/tcp_server_socket.h"

# === Files ===
fix_header "$SRC/include/neixx/files/file_path_watcher.h" SingleThreadTaskRunner 1

# === IPC ===
fix_header "$SRC/include/neixx/ipc/message_channel.h" SingleThreadTaskRunner 1
sed -i 's/scoped_refptr<TaskRunner> io_task_runner,/scoped_refptr<SingleThreadTaskRunner> io_task_runner,/g' "$SRC/include/neixx/ipc/message_channel.h"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner,/scoped_refptr<SequencedTaskRunner> client_task_runner,/g' "$SRC/include/neixx/ipc/message_channel.h"

fix_header "$SRC/include/neixx/ipc/rpc_endpoint.h" SingleThreadTaskRunner 1
sed -i 's/scoped_refptr<TaskRunner> io_task_runner,/scoped_refptr<SingleThreadTaskRunner> io_task_runner,/g' "$SRC/include/neixx/ipc/rpc_endpoint.h"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner,/scoped_refptr<SequencedTaskRunner> client_task_runner,/g' "$SRC/include/neixx/ipc/rpc_endpoint.h"

# === Process ===
fix_header "$SRC/src/neixx/process/child_process_stream_proxy.h" SingleThreadTaskRunner 1
sed -i 's/scoped_refptr<TaskRunner> target_task_runner_/scoped_refptr<SequencedTaskRunner> target_task_runner_/g' "$SRC/src/neixx/process/child_process_stream_proxy.h"

# === BindPostTask ===
sed -i 's/BindPostTaskTrampoline(scoped_refptr<TaskRunner> task_runner,/BindPostTaskTrampoline(scoped_refptr<SequencedTaskRunner> task_runner,/g' "$SRC/include/neixx/task/bind_post_task.h"
sed -i 's/scoped_refptr<TaskRunner> task_runner_;/scoped_refptr<SequencedTaskRunner> task_runner_;/' "$SRC/include/neixx/task/bind_post_task.h"
sed -i 's/const scoped_refptr<TaskRunner> current = ThreadTaskRunnerHandle::Get()/const scoped_refptr<SingleThreadTaskRunner> current = ThreadTaskRunnerHandle::Get()/' "$SRC/include/neixx/task/bind_post_task.h"
sed -i 's/BindPostTask(scoped_refptr<TaskRunner> task_runner,/BindPostTask(scoped_refptr<SequencedTaskRunner> task_runner,/g' "$SRC/include/neixx/task/bind_post_task.h"

echo "=== Headers done ==="

# === .cpp implementations ===
for f in "$SRC/src/neixx/task/thread_task_runner_handle.cpp" \
         "$SRC/src/neixx/task/timer.cpp" \
         "$SRC/src/neixx/io/async_file.cpp" \
         "$SRC/src/neixx/io/pipe_stream.cpp" \
         "$SRC/src/neixx/io/file_stream_adapters.cpp" \
         "$SRC/src/neixx/io/stream_reader.cpp" \
         "$SRC/src/neixx/io/stream_writer.cpp" \
         "$SRC/src/neixx/files/file_path_watcher.cpp" \
         "$SRC/src/neixx/process/process_service.cpp" \
         "$SRC/src/neixx/process/child_process_stream_proxy.cpp" \
         "$SRC/src/neixx/threading/thread.cpp"; do
    sed -i 's/scoped_refptr<TaskRunner>/scoped_refptr<SingleThreadTaskRunner>/g' "$f"
done

# IPC — mixed types
sed -i 's/scoped_refptr<TaskRunner> io_task_runner_/scoped_refptr<SingleThreadTaskRunner> io_task_runner_/g' "$SRC/src/neixx/ipc/message_channel.cpp"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner_/scoped_refptr<SequencedTaskRunner> client_task_runner_/g' "$SRC/src/neixx/ipc/message_channel.cpp"
sed -i 's/scoped_refptr<TaskRunner> io_task_runner_/scoped_refptr<SingleThreadTaskRunner> io_task_runner_/g' "$SRC/src/neixx/ipc/rpc_endpoint.cpp"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner_/scoped_refptr<SequencedTaskRunner> client_task_runner_/g' "$SRC/src/neixx/ipc/rpc_endpoint.cpp"

# Net .cpp
for f in "$SRC/src/neixx/net"/*.cpp; do
    sed -i 's/scoped_refptr<TaskRunner>/scoped_refptr<SingleThreadTaskRunner>/g' "$f"
done
sed -i 's/std::function<scoped_refptr<TaskRunner>()>/std::function<scoped_refptr<SingleThreadTaskRunner>()>/g' "$SRC/src/neixx/net/tcp_server_socket"*.cpp

echo "=== All done ==="
