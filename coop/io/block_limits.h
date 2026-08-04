#pragma once

namespace coop
{

namespace io
{

// Block-layer limits for the device backing a path. io_uring has three silent cliffs
// where a disk op stops completing inline and punts to an io-wq worker with no error
// surfaced: a transfer larger than the device's max_hw_sectors_kb, a full block-layer
// queue (more in-flight requests than nr_requests), and segment-count overflow. Probing
// the limits at startup makes the cliffs visible as configuration instead of latent as
// tail latency. -1 = unknown (not a block device, sysfs unavailable).
//
struct BlockLimits
{
    long maxHwSectorsKb = -1;   // largest single transfer the hardware accepts
    long nrRequests     = -1;   // block-layer queue depth
    long maxSegments    = -1;   // max scatter-gather segments per request
};

// Resolve the block device backing `path` (through partitions to the parent device) and
// read its queue limits from sysfs. Plain syscalls — call at startup/diagnostic time,
// not on the data path.
//
BlockLimits ProbeBlockLimits(const char* path);

// Number of live io-wq worker threads ("iou-wrk-*") in this process, from
// /proc/self/task. Zero is the healthy steady state for coop's socket-driven workload;
// a persistently nonzero count means disk ops are punting (buffered IO on a
// non-async-write filesystem, oversized transfers, queue pressure) and the cooperative
// latency story no longer holds for those ops. Diagnostic-time cost; not for hot paths.
//
int IowqWorkerCount();

} // end namespace coop::io
} // end namespace coop
