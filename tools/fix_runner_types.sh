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
fix_header "$SRC/modules/neixx/task/include/neixx/task/thread_task_runner_handle.h" SingleThreadTaskRunner 1
fix_header "$SRC/modules/neixx/task/include/neixx/task/timer.h" SequencedTaskRunner 1

# === IO ===
fix_header "$SRC/modules/neixx/io/include/neixx/io/async_file.h" SingleThreadTaskRunner 1
fix_header "$SRC/modules/neixx/io/include/neixx/io/pipe_stream.h" SingleThreadTaskRunner 1
fix_header "$SRC/modules/neixx/io/include/neixx/io/file_stream_adapters.h" SequencedTaskRunner 1
sed -i "s/scoped_refptr<TaskRunner> target_task_runner_/scoped_refptr<SequencedTaskRunner> target_task_runner_/g" "$SRC/modules/neixx/io/include/neixx/io/stream_reader.h"
sed -i "s/scoped_refptr<TaskRunner> target_task_runner_/scoped_refptr<SequencedTaskRunner> target_task_runner_/g" "$SRC/modules/neixx/io/include/neixx/io/stream_writer.h"

# === Net ===
for f in "$SRC/modules/neixx/net/include/neixx/net/tcp_client_socket.h" \
         "$SRC/modules/neixx/net/include/neixx/net/tcp_server_socket.h" \
         "$SRC/modules/neixx/net/include/neixx/net/tls_client_socket.h" \
         "$SRC/modules/neixx/net/include/neixx/net/tls_server_socket.h" \
         "$SRC/modules/neixx/net/include/neixx/net/udp_socket.h"; do
    fix_header "$f" SingleThreadTaskRunner 1
done
sed -i 's/std::function<scoped_refptr<TaskRunner>()>/std::function<scoped_refptr<SingleThreadTaskRunner>()>/g' "$SRC/modules/neixx/net/include/neixx/net/tcp_server_socket.h"

# === Files ===
fix_header "$SRC/modules/neixx/files/include/neixx/files/file_path_watcher.h" SingleThreadTaskRunner 1

# === IPC ===
fix_header "$SRC/modules/neixx/ipc/include/neixx/ipc/message_channel.h" SingleThreadTaskRunner 1
sed -i 's/scoped_refptr<TaskRunner> io_task_runner,/scoped_refptr<SingleThreadTaskRunner> io_task_runner,/g' "$SRC/modules/neixx/ipc/include/neixx/ipc/message_channel.h"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner,/scoped_refptr<SequencedTaskRunner> client_task_runner,/g' "$SRC/modules/neixx/ipc/include/neixx/ipc/message_channel.h"

fix_header "$SRC/modules/neixx/ipc/include/neixx/ipc/rpc_endpoint.h" SingleThreadTaskRunner 1
sed -i 's/scoped_refptr<TaskRunner> io_task_runner,/scoped_refptr<SingleThreadTaskRunner> io_task_runner,/g' "$SRC/modules/neixx/ipc/include/neixx/ipc/rpc_endpoint.h"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner,/scoped_refptr<SequencedTaskRunner> client_task_runner,/g' "$SRC/modules/neixx/ipc/include/neixx/ipc/rpc_endpoint.h"

# === Process ===
fix_header "$SRC/modules/neixx/process/src/child_process_stream_proxy.h" SingleThreadTaskRunner 1
sed -i 's/scoped_refptr<TaskRunner> target_task_runner_/scoped_refptr<SequencedTaskRunner> target_task_runner_/g' "$SRC/modules/neixx/process/src/child_process_stream_proxy.h"

# === BindPostTask ===
sed -i 's/BindPostTaskTrampoline(scoped_refptr<TaskRunner> task_runner,/BindPostTaskTrampoline(scoped_refptr<SequencedTaskRunner> task_runner,/g' "$SRC/modules/neixx/task/include/neixx/task/bind_post_task.h"
sed -i 's/scoped_refptr<TaskRunner> task_runner_;/scoped_refptr<SequencedTaskRunner> task_runner_;/' "$SRC/modules/neixx/task/include/neixx/task/bind_post_task.h"
sed -i 's/const scoped_refptr<TaskRunner> current = ThreadTaskRunnerHandle::Get()/const scoped_refptr<SingleThreadTaskRunner> current = ThreadTaskRunnerHandle::Get()/' "$SRC/modules/neixx/task/include/neixx/task/bind_post_task.h"
sed -i 's/BindPostTask(scoped_refptr<TaskRunner> task_runner,/BindPostTask(scoped_refptr<SequencedTaskRunner> task_runner,/g' "$SRC/modules/neixx/task/include/neixx/task/bind_post_task.h"

echo "=== Headers done ==="

# === .cpp implementations ===
for f in "$SRC/modules/neixx/task/src/thread_task_runner_handle.cpp" \
         "$SRC/modules/neixx/task/src/timer.cpp" \
         "$SRC/modules/neixx/io/src/async_file.cpp" \
         "$SRC/modules/neixx/io/src/pipe_stream.cpp" \
         "$SRC/modules/neixx/io/src/file_stream_adapters.cpp" \
         "$SRC/modules/neixx/io/src/stream_reader.cpp" \
         "$SRC/modules/neixx/io/src/stream_writer.cpp" \
         "$SRC/modules/neixx/files/src/file_path_watcher.cpp" \
         "$SRC/modules/neixx/process/src/process_service.cpp" \
         "$SRC/modules/neixx/process/src/child_process_stream_proxy.cpp" \
         "$SRC/modules/neixx/threading/src/thread.cpp"; do
    sed -i 's/scoped_refptr<TaskRunner>/scoped_refptr<SingleThreadTaskRunner>/g' "$f"
done

# IPC — mixed types
sed -i 's/scoped_refptr<TaskRunner> io_task_runner_/scoped_refptr<SingleThreadTaskRunner> io_task_runner_/g' "$SRC/modules/neixx/ipc/src/message_channel.cpp"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner_/scoped_refptr<SequencedTaskRunner> client_task_runner_/g' "$SRC/modules/neixx/ipc/src/message_channel.cpp"
sed -i 's/scoped_refptr<TaskRunner> io_task_runner_/scoped_refptr<SingleThreadTaskRunner> io_task_runner_/g' "$SRC/modules/neixx/ipc/src/rpc_endpoint.cpp"
sed -i 's/scoped_refptr<TaskRunner> client_task_runner_/scoped_refptr<SequencedTaskRunner> client_task_runner_/g' "$SRC/modules/neixx/ipc/src/rpc_endpoint.cpp"

# Net .cpp
for f in "$SRC/modules/neixx/net/src"/*.cpp; do
    sed -i 's/scoped_refptr<TaskRunner>/scoped_refptr<SingleThreadTaskRunner>/g' "$f"
done
sed -i 's/std::function<scoped_refptr<TaskRunner>()>/std::function<scoped_refptr<SingleThreadTaskRunner>()>/g' "$SRC/modules/neixx/net/src/tcp_server_socket"*.cpp

echo "=== All done ==="
