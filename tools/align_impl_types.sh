#!/bin/bash
# align_impl_types.sh — narrow all internal scoped_refptr<TaskRunner> to precise types
SRC=/mnt/c/Personal/Projects/LibNei/libnei-src
set -e

echo "=== IO: SingleThreadTaskRunner ==="
# async_file internals (io_task_runner is always STR)
for f in "$SRC/src/neixx/io/async_file_posix.cpp" \
         "$SRC/src/neixx/io/async_file_posix.h" \
         "$SRC/src/neixx/io/async_file_win.cpp" \
         "$SRC/src/neixx/io/async_file_win.h"; do
    sed -i 's/scoped_refptr<TaskRunner> io_task_runner/scoped_refptr<SingleThreadTaskRunner> io_task_runner/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> io_runner_snapshot/scoped_refptr<SingleThreadTaskRunner> io_runner_snapshot/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> io_runner =/scoped_refptr<SingleThreadTaskRunner> io_runner =/g' "$f"
    echo "  $f"
done

# pipe_stream internals
for f in "$SRC/src/neixx/io/pipe_stream_posix.cpp" \
         "$SRC/src/neixx/io/pipe_stream_posix.h" \
         "$SRC/src/neixx/io/pipe_stream_win.cpp" \
         "$SRC/src/neixx/io/pipe_stream_win.h" \
         "$SRC/src/neixx/io/pipe_stream.cpp"; do
    sed -i 's/scoped_refptr<TaskRunner> io_task_runner/scoped_refptr<SingleThreadTaskRunner> io_task_runner/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> runner = impl_->io_task_runner()/scoped_refptr<SingleThreadTaskRunner> runner = impl_->io_task_runner()/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> &runner,/scoped_refptr<SingleThreadTaskRunner> \&runner,/g' "$f"
    echo "  $f"
done

echo "=== IO: SequencedTaskRunner ==="
# stream_reader/writer helpers + local vars
for f in "$SRC/src/neixx/io/stream_reader.cpp" \
         "$SRC/src/neixx/io/stream_writer.cpp"; do
    sed -i 's/scoped_refptr<TaskRunner> target_runner = target_task_runner_/scoped_refptr<SequencedTaskRunner> target_runner = target_task_runner_/g' "$f"
    sed -i 's/void PostEmptyFailure(scoped_refptr<TaskRunner> runner,/void PostEmptyFailure(scoped_refptr<SequencedTaskRunner> runner,/g' "$f"
    sed -i 's/void PostWriteResult(scoped_refptr<TaskRunner> runner,/void PostWriteResult(scoped_refptr<SequencedTaskRunner> runner,/g' "$f"
    echo "  $f"
done

# file_stream_adapters — local vars (member already SeqTR)
sed -i 's/scoped_refptr<TaskRunner> target_runner = target_task_runner_/scoped_refptr<SequencedTaskRunner> target_runner = target_task_runner_/g' \
    "$SRC/src/neixx/io/file_stream_adapters.cpp"
echo "  file_stream_adapters.cpp"

echo "=== Files: SingleThreadTaskRunner ==="
for f in "$SRC/src/neixx/files/file_path_watcher.cpp" \
         "$SRC/src/neixx/files/file_path_watcher_posix.cpp"; do
    sed -i 's/scoped_refptr<TaskRunner> task_runner)/scoped_refptr<SingleThreadTaskRunner> task_runner)/g' "$f"
    echo "  $f"
done

echo "=== Net: SingleThreadTaskRunner ==="
# All net IO runners are STR
for f in "$SRC/src/neixx/net/tcp_client_socket.cpp" \
         "$SRC/src/neixx/net/tcp_client_socket_posix.cpp" \
         "$SRC/src/neixx/net/tcp_client_socket_posix.h" \
         "$SRC/src/neixx/net/tcp_client_socket_win.cpp" \
         "$SRC/src/neixx/net/tcp_client_socket_win.h" \
         "$SRC/src/neixx/net/tcp_server_socket.cpp" \
         "$SRC/src/neixx/net/tcp_server_socket_posix.cpp" \
         "$SRC/src/neixx/net/tcp_server_socket_posix.h" \
         "$SRC/src/neixx/net/tcp_server_socket_win.cpp" \
         "$SRC/src/neixx/net/tcp_server_socket_win.h" \
         "$SRC/src/neixx/net/tls_client_socket.cpp" \
         "$SRC/src/neixx/net/tls_server_socket.cpp" \
         "$SRC/src/neixx/net/udp_socket.cpp" \
         "$SRC/src/neixx/net/udp_socket_posix.cpp" \
         "$SRC/src/neixx/net/udp_socket_posix.h" \
         "$SRC/src/neixx/net/udp_socket_win.cpp" \
         "$SRC/src/neixx/net/udp_socket_win.h"; do
    sed -i 's/scoped_refptr<TaskRunner> io_runner/scoped_refptr<SingleThreadTaskRunner> io_runner/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> acceptor_runner/scoped_refptr<SingleThreadTaskRunner> acceptor_runner/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> runner)/scoped_refptr<SingleThreadTaskRunner> runner)/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> runner,/scoped_refptr<SingleThreadTaskRunner> runner,/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> worker_runner =/scoped_refptr<SingleThreadTaskRunner> worker_runner =/g' "$f"
    sed -i 's/scoped_refptr<TaskRunner> PickWorker()/scoped_refptr<SingleThreadTaskRunner> PickWorker()/g' "$f"
    echo "  $f"
done

echo "=== Done ==="
