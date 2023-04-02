# High Performance Atomic/Non-Atomic Lockless MPMC Queue types for Unreal Engine 5.

Related to Unreal PR https://github.com/EpicGames/UnrealEngine/pull/8859

Bounded MPMC Queue type(s) re-write, designed to be better than the AtomicQueue presently used in the Unreal Engine.

Queue types are in Queue.h

## Types:

- [x] TBoundedQueueComon
1. [x] Regular Type Versions:
   - [x] TBoundedCircularQueueBase
     - [x] TBoundedCircularQueue
     - [x] TBoundedCircularQueueHeap
2. [x] Atomic Versions:
   - [x] TBoundedCircularAtomicQueueBase
     - [x] TBoundedCircularAtomicQueue
     - [x] FBoundedCircularAtomicQueueHeap
