# UE_QueueType_DevEnv

Related to Unreal PR https://github.com/EpicGames/UnrealEngine/pull/8859

Bounded MPMC Queue type(s) re-write, designed to be better than the AtomicQueue presently used in the Unreal Engine.

Currently a work in progress. Everything is contained within one type for easy benchmaring.

Queue type is in Queue.h. All code temporarily inside FBoundedQueueBase.

CURRENT VERSIONS
  Using cursors stored in their own cache lines.
  Using cursors stored in their own cache lines, and a shared cached copy of the cursors.
  Using cursors stored in their own cache lines, and individual cached copies of the curors.
TODO VERSIONS:
  Using cursors stored in the same cache line as each other.
  Using cursors stored in the same cache line as each other, and a shared cached copy of the cursors.
  Using cursors stored in the same cache line as each other, and individual cached copies of the curors.
