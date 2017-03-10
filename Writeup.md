# Network Sockets over RDMA
A writeup for the [practical course database implementation](http://db.in.tum.de/teaching/ws1617/imlab/)

## Abstract
Modern in-memory databases are more and more bottlenecked in transactional workloads by network latency. One of the 
reasons for this latency is the overhead associated with sending data through kernel interfaces. I.e. a typical way of
sending data over the network is the `write()` system call. This is associated with  
context-switches from kernel- to user-mode and back, which is rather costly. <!-- Maybe cite: https://www.cs.cmu.edu/~chensm/Big_Data_reading_group/papers/flexsc-osdi10.pdf -->  
In this project we implemented a high-performance, low-latency TCP alternative using Remote Direct Memory Access (RDMA)
without system calls. RDMA is a technique mainly supported by the InfiniBand standard, but can also be implemented over 
converged Ethernet and other network types.

## Previous work
In 2015 Peter Goldsborough et al. implemented [TSSX](https://github.com/goldsborough/tssx), a *transparent shared-memory
socket exchange*, i.e. a with `LD_PRELOAD` pre-loadable shared library, that emulates Unix domain sockets over 
shared-memory. In his work, he demonstrated that eliminating system calls can result in  ~70% more transactions per second (tps)
in the TPC-B benchmark.  
In our work, we adapted TSSX's concept for RDMA, while reusing significant parts of the code needed to support the 
socket interface.
 
There are some similar implementations of this concept, mainly Message Passing Interfaces like OpenMPI or MPICH that 
also support sending messages over RDMA. However, we decided to not use these libraries as basis to avoid another level 
of indirection (wrapping sockets in MPI in RDMA). Instead we directly base our work on [libibverbs](https://git.kernel.org/cgit/libs/infiniband/libibverbs.git)
as described in the InfiniBand architecture specification and the RDMA protocol verbs specification.

## Basic messaging
Basic messaging in our implementation is handled by the `RDMAMessageBuffer`. At first, it registers 
all buffers and queues needed in the RDMA communication with libibverbs, then exchanges all necessary information with the remote side
of the connection. To communicate with the remote side and exchange network and buffer IDs, we reuse the TCP socket we are
replacing. So we bootstrap our connection setup with the preexisting TCP connection. This also means, we can always fall 
back to the TCP connection, if anything goes wrong.

In the `RDMAMessageBuffer::send()` method, we can now post WriteWorkRequests to the RDMA QueuePair to write messages into 
the receivers memory. Here it is especially important to eliminate superfluous calls to libibverbs, e.g. registering a 
new MemoryRegion with libibverbs is costly and involves system calls, so we are better off "slicing" the preregistered 
buffer by raw pointer arithmetic. [RDMAMojo](http://www.rdmamojo.com/2013/06/08/tips-and-tricks-to-optimize-your-rdma-code/#Avoid_using_control_operations_in_the_data_path) describes this as "Avoid using control operations in the data path", which are rather expensive in the context of send / receive operations.

We also need a concept to message the remote side that new data has been written and a message has arrived. TSSX
solves this by maintaining separate read and write indices that are atomically updated. We can mimic this 
behaviour with RDMA using `IBV_WR_ATOMIC_FETCH_AND_ADD` work requests.

## Optimizations
### Messages with header / footer
Apart from usual, but nevertheless significant optimizations like utilizing buffer sizes of $2^n$ for buffer 
wraparounds, the most significant performance improvement came from a more sophisticated message signaling.  
With the previous approach, we always send two separate work requests: One for the actual data and another
one to update the message count. Also one of these is an atomic operation, which is generally slower than a normal
operation. We can squash the two operations into one with the help of two assumptions:

1. We always write to zero'ed memory
2. We write ordered first to last byte

The first assumption can be achieved by strictly zeroing out already read messages at the receiver side. The second 
assumption can be guaranteed by setting the RDMA QueuePair type to `IBV_QPT_RC`, i.e. a **R**eliable "TCP-like" 
**C**onnection.

Based on this properties, we can now define a header / footer format that also signals if a message has been completely
transmitted. We precede the message with a 4B length and is followed by a 4B validity footer. A message is then readable
and completely transmitted, when the memory at position `start + sizeof(messageSize) + messageSize` correctly holds the 
validity footer. When the data has not been completely transmitted, we get from 1. that we read zero'ed memory (thus an
incorrect validity). When we read a validity we are guaranteed by 2. that all bytes of the message already have been written
to the buffer.

### Inline sending
Usually when posting a WorkRequest, an asynchronous progress starts where the RDMA capable hardware reads the message
as needed. This is especially worthwhile for large workloads, as the control flow can immediately return to the caller.
However for small payloads, that are especially common in transactional workloads, this process can induce additional latency
until the message is actually sent. When setting the `IBV_SEND_INLINE` flag in the WorkRequest, the data is 
synchronously copied, eliminating this latency. Setting this flag is only really viable for message sizes up to around 
192 Bytes, where we get a ~20% improvement in messages per second. Up to around 432 Bytes, there is still a ~5% 
improvement. Messages longer than 432 Bytes do generally not profit from being inlined.

The inline tests can be reproduced and remeasured for new hardware with the `rdmaInlineComparison` build target.

## Results
To conclude this project, we did some benchmarks measuring the performance improvements of our library. The
first test is a micro-benchmark only measuring the raw message throughput, i.e. a upper limit in how many round trips per
second are possible: 

| 64B Messages over | RoundTrips / second |
| ---- | ------------------: |
| TCP over Ethernet | 19,968 |
| TCP over Infiniband | 42,961 |
| RDMA without Optimizations | 194,872 |
| RDMA + header / footer | 335,000 |
| RDMA + header / footer + inline | 376,393 |

The second benchmark is an adapted version of the `pgbench` TPC-B test. Unfortunately we didn't manage to get `pgbench`
to work with `LD_PRELOAD`ing our library, but instead logged the SQL output of `pgbench -n -T 5`, which we then 
replayed using `psql`. So this benchmark **includes** connection setup and teardown, which should slightly favour TCP.

| PostgreSQL Benchmark |   tps |
| ----                 | ----: |
| over TCP             | 3,499 |
| over RDMA            | 5,441 |

Interesting additional comparisons would include librdmacm, OpenMPI and MPICH. However none of the mentioned libraries 
are easily compatible with PostgreSQL, thus exceed the scope of this project.
