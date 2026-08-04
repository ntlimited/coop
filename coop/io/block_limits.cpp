#include "block_limits.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <spdlog/spdlog.h>

namespace coop
{

namespace io
{

namespace
{

long ReadSysfsLong(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f)
    {
        return -1;
    }
    long value = -1;
    if (fscanf(f, "%ld", &value) != 1)
    {
        value = -1;
    }
    fclose(f);
    return value;
}

} // end anonymous namespace

BlockLimits ProbeBlockLimits(const char* path)
{
    BlockLimits limits;

    struct stat st;
    if (stat(path, &st) != 0)
    {
        return limits;
    }

    // A partition's sysfs dir has no queue/; the parent device's does. Following
    // /sys/dev/block/MAJ:MIN/../queue covers both cases: for a whole device the parent
    // is the block/ class dir, so try the direct queue/ first, then the parent's.
    //
    char base[128];
    snprintf(base, sizeof(base), "/sys/dev/block/%u:%u",
             major(st.st_dev), minor(st.st_dev));

    const char* stems[] = { "%s/queue/%s", "%s/../queue/%s" };
    const char* names[] = { "max_hw_sectors_kb", "nr_requests", "max_segments" };
    long* fields[] = { &limits.maxHwSectorsKb, &limits.nrRequests, &limits.maxSegments };

    for (const char* stem : stems)
    {
        char file[192];
        snprintf(file, sizeof(file), stem, base, names[0]);
        if (ReadSysfsLong(file) < 0)
        {
            continue;
        }
        for (int i = 0; i < 3; i++)
        {
            snprintf(file, sizeof(file), stem, base, names[i]);
            *fields[i] = ReadSysfsLong(file);
        }
        break;
    }

    if (limits.maxHwSectorsKb > 0)
    {
        spdlog::info("block limits for {}: max_hw_sectors_kb={} nr_requests={} "
                     "max_segments={}",
                     path, limits.maxHwSectorsKb, limits.nrRequests, limits.maxSegments);
    }
    else
    {
        spdlog::info("block limits for {}: unavailable (not a block device?)", path);
    }
    return limits;
}

int IowqWorkerCount()
{
    DIR* dir = opendir("/proc/self/task");
    if (!dir)
    {
        return -1;
    }

    int count = 0;
    while (dirent* entry = readdir(dir))
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }

        char commPath[280];
        snprintf(commPath, sizeof(commPath), "/proc/self/task/%s/comm", entry->d_name);
        FILE* f = fopen(commPath, "r");
        if (!f)
        {
            continue;
        }
        char comm[32] = {};
        if (fgets(comm, sizeof(comm), f) && strncmp(comm, "iou-wrk", 7) == 0)
        {
            count++;
        }
        fclose(f);
    }
    closedir(dir);
    return count;
}

} // end namespace coop::io
} // end namespace coop
