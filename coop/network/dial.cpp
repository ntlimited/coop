#include <cerrno>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dial.h"

#include "event_mask.h"
#include "handle.h"
#include "router.h"

#include "coop/context.h"
#include "coop/coordinator.h"
#include "coop/multi_coordinator.h"

namespace coop
{

namespace network
{

int Dial(
    const char* hostname,
    int port)
{
    // Set up a non-blocking socket and initiate the connect
    //

    int fd = socket(AF_INET , SOCK_STREAM | SOCK_NONBLOCK, 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (!inet_pton(AF_INET, hostname, &addr.sin_addr))
    {
        printf("failed to inet_pton\n");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)))
    {
        if (errno != EINPROGRESS)
        {
            printf("failed to connect: %d (%s)\n", errno, strerror(errno));
            close(fd);
            return -1;
        }
    }

    return fd;
}

} // end namespace coop::network
} // end namespace coop
