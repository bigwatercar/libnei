#!/bin/bash
SRC="/mnt/c/Personal/Projects/LibNei/libnei-src"

# Fix pipe_stream_bench.cpp
sed -i 's|nei::scoped_refptr<nei::TaskRunner> io_runner;|nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner;|g' "${SRC}/bench/pipe_stream_bench.cpp"
sed -i 's|PipeBenchState(nei::scoped_refptr<nei::TaskRunner> runner,|PipeBenchState(nei::scoped_refptr<nei::SingleThreadTaskRunner> runner,|g' "${SRC}/bench/pipe_stream_bench.cpp"
sed -i 's|const nei::scoped_refptr<nei::TaskRunner> &io_runner, std::size_t chunk_size|const nei::scoped_refptr<nei::SingleThreadTaskRunner> \&io_runner, std::size_t chunk_size|g' "${SRC}/bench/pipe_stream_bench.cpp"
sed -i 's|const nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner()|const nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner = io_thread.GetTaskRunner()|g' "${SRC}/bench/pipe_stream_bench.cpp"
echo "Fixed bench/pipe_stream_bench.cpp"

# Fix pipe_stream_cross_process_bench.cpp if it has similar issues
if [ -f "${SRC}/bench/pipe_stream_cross_process_bench.cpp" ]; then
    sed -i 's|nei::scoped_refptr<nei::TaskRunner> io_runner|nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner|g' "${SRC}/bench/pipe_stream_cross_process_bench.cpp"
    echo "Fixed bench/pipe_stream_cross_process_bench.cpp"
fi

echo "=== Done ==="
