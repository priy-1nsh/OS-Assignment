#ifndef WORKLOAD_IO_H
#define WORKLOAD_IO_H

#include <stdio.h> // snprintf
#include <string.h> // memset
#include <fcntl.h> // open and O_* flags
#include <unistd.h> // write, fsync, close, unlink

// Each child attempts 2,000 writes of 4,096 bytes:
#define IO_OPERATIONS 2000
#define IO_BLOCK_SIZE 4096

// a function that keeps writing data to a temporary file, asks the operating system to save it to storage, and finally deletes the file.
// static gives the function an internal linkage
static void io_task(int id) // id is the worker number used in the filename
{
char path[64]; // stores the filename and it's path
char block[IO_BLOCK_SIZE];
int fd, i;

snprintf(path, sizeof(path),
"/tmp/io_worker_%d.dat", id); // snprintf works like printf, except it writes formatted text into a character array instead of printing it on the screen.
memset(block, 'x', sizeof(block)); // it fills all 4,096 positions in block with 'x'.

fd = open(path, O_CREAT | O_WRONLY | O_TRUNC,
0644);
// This asks the operating system to open the file named by path.

// The flags specify what should happen:
// Flag	Meaning -
// O_CREAT	Create the file if it does not exist
// O_WRONLY	Open it for writing only
// O_TRUNC	If it already exists, discard its existing contents


// The | operator combines these flags so all three apply.

// 0644
// is an octal permission value, used when creating the file:
// - Owner: read and write.
// - Group: read only.
// - Others: read only.
if (fd < 0)
return;

for (i = 0; i < IO_OPERATIONS; i++) {

    if (write(fd, block, sizeof(block)) != sizeof(block))
        break;
        // "Write 4,096 bytes from block into the file identified by fd.”

        // write() returns the number of bytes actually written, or -1 on error.

    fsync(fd);
    // fsync() asks the operating system to synchronize the file’s data and required metadata to storage, waiting for completion.

    // Calling it after every block makes this workload much more demanding than simply buffering all the writes.
}

close(fd);
unlink(path); // Removes the file’s name from the filesystem
}

#endif

// One practical detail: /tmp can be backed by RAM on some systems, so this workload does not always measure physical disk performance.