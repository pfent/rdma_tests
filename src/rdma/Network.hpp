//---------------------------------------------------------------------------
// (c) 2015 Wolf Roediger <roediger@in.tum.de>
// Technische Universitaet Muenchen
// Institut fuer Informatik, Lehrstuhl III
// Boltzmannstr. 3
// 85748 Garching
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//---------------------------------------------------------------------------
#pragma once
//---------------------------------------------------------------------------
#include <mutex>
#include <stdexcept>
#include <vector>
//---------------------------------------------------------------------------
struct ibv_comp_channel;
struct ibv_context;
struct ibv_cq;
struct ibv_device;
struct ibv_mr;
struct ibv_pd;
struct ibv_qp;
struct ibv_srq;
//---------------------------------------------------------------------------
namespace rdma {
//---------------------------------------------------------------------------
class WorkRequest;
class MemoryRegion;
//---------------------------------------------------------------------------
/// A network exception
class NetworkException : public std::runtime_error
{
public:
   NetworkException(const std::string& reason) : std::runtime_error(reason) {}
};
//---------------------------------------------------------------------------
struct RemoteMemoryRegion {
   uintptr_t address;
   uint32_t key;
};
//---------------------------------------------------------------------------
/// The LID and QPN uniquely address a queue pair
struct Address {
   uint16_t lid;
   uint32_t qpn;
};
std::ostream &operator<<(std::ostream &os, const Address &address);
//---------------------------------------------------------------------------
/// A network of nodes connected via RDMA
class Network
{
protected:
   /// The minimal number of entries for the completion queue
   static const int CQ_SIZE = 100;

   /// The number of queue pairs
   unsigned queuePairCount;
   /// The port of the Infiniband device
   uint8_t ibport;

   /// The Infiniband devices
   ibv_device **devices;
   /// The verbs context
   ibv_context *context;
   /// The global protection domain
   ibv_pd *protectionDomain;
   /// The shared send completion queue
   ibv_cq *completionQueueSend;
   /// The shared receive completion queue
   ibv_cq *completionQueueRecv;
   /// The shared completion channel
   ibv_comp_channel *completionChannel;
   /// The shared receive queue
   ibv_srq* srq;
   /// The queue pairs
   std::vector<ibv_qp*> queuePairs;

   /// The cached work completions
   std::vector<std::pair<bool, uint64_t>> cachedCompletions;
   std::mutex completionMutex;

   /// Create queue pair
   ibv_qp *createQueuePair(ibv_cq *cqSend, ibv_cq *cqRecv);
   /// Poll a completion queue
   uint64_t pollCompletionQueue(ibv_cq *completionQueue, int type);

   /// Wait for a work request completion
   std::pair<bool, uint64_t> waitForCompletion(bool restrict, bool onlySend);

public:
   /// Constructor
   Network(unsigned queuePairCount);
   /// Destructor
   ~Network();

   /// Get the LID
   uint16_t getLID();
   /// Get the queue pair number for a queue pair
   uint32_t getQPN(unsigned index);

   /// Connect the network
   void connect(std::vector<Address> addresses, unsigned retryCount = 0);

   /// Post a send work request
   void postSend(unsigned target, const MemoryRegion& mr, bool completion, uint64_t context, int flags = 0);
   /// Post a write work request
   void postWrite(unsigned target, const RemoteMemoryRegion& t_mr, const MemoryRegion& s_mr, bool completion, uint64_t context, int flags = 0);
   /// Post a receive request
   void postRecv(const MemoryRegion& mr, uint64_t context);
   /// Post a read work request
   void postRead(unsigned target, const MemoryRegion& t_mr, const RemoteMemoryRegion& s_mr, bool completion, uint64_t context, int flags = 0);
   /// Post an atomic fetch/add request
   void postFetchAdd(unsigned target, const MemoryRegion& beforeValue, const RemoteMemoryRegion& remoteAddress, uint64_t add, bool completion, uint64_t context, int flags = 0);
   /// Post an atomic compare/swap request
   void postCompareSwap(unsigned target, const MemoryRegion& beforeValue, const RemoteMemoryRegion& remoteAddress, uint64_t compare, uint64_t swap, bool completion, uint64_t context, int flags = 0);
   /// Post a generic work request
   void postWorkRequest(unsigned target, const WorkRequest& workRequest);

   /// Poll the send completion queue
   uint64_t pollSendCompletionQueue();
   /// Poll the receive completion queue
   uint64_t pollRecvCompletionQueue();

   // Poll a completion queue blocking
   uint64_t pollCompletionQueueBlocking(ibv_cq *completionQueue, int type);
   /// Poll the send completion queue blocking
   uint64_t pollSendCompletionQueueBlocking();
   /// Poll the receive completion queue blocking
   uint64_t pollRecvCompletionQueueBlocking();

   /// Wait for a work request completion
   std::pair<bool, uint64_t> waitForCompletion();
   uint64_t waitForCompletionSend();
   uint64_t waitForCompletionReceive();

   /// Get the protection domain
   ibv_pd *getProtectionDomain() {
      return protectionDomain;
   }

   /// Print the capabilities of the RDMA host channel adapter
   void printCapabilities();
   /// Print detailed information about the specified queue pair
   void printQueuePairDetails(unsigned qpid);
};
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
