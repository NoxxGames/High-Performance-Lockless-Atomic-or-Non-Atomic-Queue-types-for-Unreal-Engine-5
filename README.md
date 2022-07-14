# High Performance Atomic/Non-Atomic Lockless MPMC Queue types for Unreal Engine 5.

Related to Unreal PR https://github.com/EpicGames/UnrealEngine/pull/8859

Bounded MPMC Queue type(s) re-write, designed to be better than the AtomicQueue presently used in the Unreal Engine.

Currently a work in progress. Queue types are in Queue.h

## Types to be created:

- [x] TBoundedQueueComon (C)
1. [x] Regular Type Versions:
   - [x] TBoundedCircularQueueBase (C)
     - [x] TBoundedCircularQueue (C)
     - [x] TBoundedCircularQueueHeap (C)
2. [ ] Atomic Versions:
   - [x] TBoundedCircularAtomicQueueBase (C)
     - [x] TBoundedCircularAtomicQueue (C)
     - [ ] FBoundedCircularAtomicQueueHeap (WIP)
