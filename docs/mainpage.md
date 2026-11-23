# HydraHook

Reference for the public C API and the engine implementation.

## Public API

- [Core lifecycle](@ref HydraHookCore.h) — create and destroy the engine, custom context, and logging.
- Direct3D callback families — [Direct3D 9](@ref HydraHookDirect3D9.h), [Direct3D 10](@ref HydraHookDirect3D10.h), [Direct3D 11](@ref HydraHookDirect3D11.h), and [Direct3D 12](@ref HydraHookDirect3D12.h).
- [Core Audio](@ref HydraHookCoreAudio.h) — `IAudioRenderClient` buffer callbacks.

## Engine internals

- [Implementation](@ref Engine.h) — engine state, the hook worker, and the Detours wrappers. C++ types stay in the class index, and both audiences have source links.
- [Architecture](@ref md_docs_2architecture) — process model, shutdown, and extension points.
