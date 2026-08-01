#!/bin/bash
SRC="/mnt/c/Personal/Projects/LibNei/libnei-src"
set -e

# Fix lambda return types: TaskRunner → SingleThreadTaskRunner
for f in tcp_cross_bench tcp_conn_stress_bench tcp_rtt_bench tcp_loopback_bench tcp_throughput_bench; do
    file="${SRC}/bench/${f}.cpp"
    sed -i 's|mutable -> nei::scoped_refptr<nei::TaskRunner>|mutable -> nei::scoped_refptr<nei::SingleThreadTaskRunner>|g' "$file"
    echo "Fixed lambda in bench/${f}.cpp"
done

# Fix async_file_example.cpp line 177
sed -i 's|nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner()|nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner = io_thread.GetTaskRunner()|g' "${SRC}/examples/async_file_example.cpp"
echo "Fixed async_file_example.cpp"

# Fix pipe stream examples
sed -i 's|nei::scoped_refptr<nei::TaskRunner> io_runner|nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner|g' "${SRC}/examples/pipe_stream_cross_process_posix_example.cpp" "${SRC}/examples/pipe_stream_cross_process_win_example.cpp"
echo "Fixed pipe stream examples"

echo "=== All fixes applied ==="
