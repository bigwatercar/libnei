#!/bin/bash
# Fix bench/example TaskRunner types for new runner hierarchy
SRC="/mnt/c/Personal/Projects/LibNei/libnei-src"

# Category A: IoThread::runner() return types → SingleThreadTaskRunner
for f in tcp_conn_stress_bench tcp_cross_bench tcp_loopback_bench tcp_rtt_bench tcp_throughput_bench tls_throughput_bench; do
    file="${SRC}/bench/${f}.cpp"
    sed -i 's/nei::scoped_refptr<nei::TaskRunner> runner()/nei::scoped_refptr<nei::SingleThreadTaskRunner> runner()/g' "$file"
    sed -i 's/nei::scoped_refptr<nei::TaskRunner> runner_;/nei::scoped_refptr<nei::SingleThreadTaskRunner> runner_;/g' "$file"
    echo "Fixed bench/${f}.cpp"
done

# Fix resolve_connect_example
sed -i 's/nei::scoped_refptr<nei::TaskRunner> runner()/nei::scoped_refptr<nei::SingleThreadTaskRunner> runner()/g' "${SRC}/examples/resolve_connect_example.cpp"
sed -i 's/nei::scoped_refptr<nei::TaskRunner> runner_;/nei::scoped_refptr<nei::SingleThreadTaskRunner> runner_;/g' "${SRC}/examples/resolve_connect_example.cpp"
echo "Fixed examples/resolve_connect_example.cpp"

# Category B: async_file background_runner → SequencedTaskRunner
sed -i 's/const nei::scoped_refptr<nei::TaskRunner> &bg,/const nei::scoped_refptr<nei::SequencedTaskRunner> \&bg,/g' "${SRC}/bench/async_file_bench.cpp" "${SRC}/examples/async_file_example.cpp"
sed -i 's/const nei::scoped_refptr<nei::TaskRunner>& bg/const nei::scoped_refptr<nei::SequencedTaskRunner>\& bg/g' "${SRC}/bench/async_file_bench.cpp" "${SRC}/examples/async_file_example.cpp"
sed -i 's/const nei::scoped_refptr<nei::TaskRunner> &background_runner/const nei::scoped_refptr<nei::SequencedTaskRunner> \&background_runner/g' "${SRC}/examples/async_file_example.cpp" "${SRC}/bench/async_file_bench.cpp"
sed -i 's/const nei::scoped_refptr<nei::TaskRunner>& background_runner/const nei::scoped_refptr<nei::SequencedTaskRunner>\& background_runner/g' "${SRC}/examples/async_file_example.cpp"
sed -i 's/scoped_refptr<nei::TaskRunner> background_runner/scoped_refptr<nei::SequencedTaskRunner> background_runner/g' "${SRC}/examples/async_file_example.cpp"
echo "Fixed async_file bench/example"

# Category D: pipe stream examples io_runner → SingleThreadTaskRunner
sed -i 's/nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner()/nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner = io_thread.GetTaskRunner()/g' "${SRC}/examples/pipe_stream_cross_process_posix_example.cpp" "${SRC}/examples/pipe_stream_cross_process_win_example.cpp"
echo "Fixed pipe stream examples"

# Category E: task_priority_perf_demo — internal::Task no longer exists
# The observer API changed; comment it out for now
echo "WARNING: task_priority_perf_demo.cpp needs manual fix (internal::Task removed)"

echo "=== All fixes applied ==="
