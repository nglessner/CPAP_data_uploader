#ifndef O2RING_FILE_SINK_H
#define O2RING_FILE_SINK_H

#include <Arduino.h>
#include <cstdint>

// Streaming destination for an O2Ring file pull. The orchestrator hands BLE
// chunks (~240 bytes each) to writeChunk() as they arrive, instead of
// buffering the whole file in RAM first. This lets us pull 80+ KB files
// even when ESP32 max-contiguous-heap is only 30 KB at sync time.
//
// Lifecycle per file:
//   begin(name, totalSize) → writeChunk(...) × N → finalize(ok)
//
// One sink instance can be reused across multiple files in a single sync
// run; the orchestrator calls begin/finalize once per file. Implementations
// are responsible for cleaning up partial data on finalize(false).
class O2RingFileSink {
public:
    virtual ~O2RingFileSink() = default;

    // Called before any chunks. totalSize comes from F2's reply (file size
    // advertised by the ring). Return false to abort this file's pull;
    // finalize() will not be called in that case.
    virtual bool begin(const String& filename, uint32_t totalSize) = 0;

    // Called once per F3 reply chunk. Return false to abort the pull;
    // finalize(false) will be called for cleanup.
    virtual bool writeChunk(const uint8_t* data, size_t len) = 0;

    // Called exactly once after begin() returned true, regardless of
    // success. ok=true means all chunks were delivered and matched the
    // advertised size; ok=false means abort/error and any partial output
    // should be cleaned up.
    virtual void finalize(bool ok) = 0;
};

#endif  // O2RING_FILE_SINK_H
