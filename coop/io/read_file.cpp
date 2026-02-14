#include "read_file.h"

#include <cerrno>
#include <fcntl.h>

#include <spdlog/spdlog.h>

#include "coop/self.h"

#include "descriptor.h"
#include "open.h"
#include "read.h"

namespace coop
{

namespace io
{

int ReadFile(const char* path, void* buf, size_t bufSize)
{
    int fd = Open(path, O_RDONLY);
    if (fd < 0)
    {
        spdlog::error("readfile open failed path={} err={}", path, fd);
        return fd;
    }

    auto* ring = GetUring();
    Descriptor desc(fd, ring);

    size_t total = 0;
    for (;;)
    {
        int result = Read(
            desc,
            static_cast<char*>(buf) + total,
            bufSize - total,
            total);

        if (result < 0)
        {
            spdlog::error("readfile read failed path={} err={}", path, result);
            desc.Close();
            return result;
        }

        if (result == 0)
        {
            break;
        }

        total += static_cast<size_t>(result);

        // If the buffer is full, probe for one more byte to detect overflow
        //
        if (total == bufSize)
        {
            char probe;
            int probeResult = Read(desc, &probe, 1, total);
            if (probeResult > 0)
            {
                spdlog::error("readfile overflow path={} bufSize={}", path, bufSize);
                desc.Close();
                return -EOVERFLOW;
            }
            break;
        }
    }

    desc.Close();
    spdlog::debug("readfile path={} bytes={}", path, total);
    return static_cast<int>(total);
}

} // end namespace coop::io
} // end namespace coop
