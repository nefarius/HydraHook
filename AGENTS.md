# AGENTS.md

Guidance for AI coding agents working in this repository.

## Branch and pull request workflow

Do not implement or commit work on `master`. Before editing, confirm the current branch is the requested work. If it is not, create a branch from up-to-date `master` (`feat/…`, `fix/…`, or `chore/…`) and do the work there. Open a pull request. Do not push commits directly to `master`.

When a reviewer (human or bot) leaves inline findings, verify each claim against the current code before changing it. Fix findings that are still valid. Leave pre-existing or out-of-scope items alone and say why.

Do not merge on a green `build` check alone. A passing build does not mean review threads were addressed. Before merging, query review threads for the pull request's current `headRefOid`:

```powershell
gh api graphql -f query='
  query($owner:String!,$repo:String!,$pr:Int!,$reviewCursor:String,$threadCursor:String) {
    repository(owner:$owner, name:$repo) {
      pullRequest(number:$pr) {
        headRefOid
        reviews(first:100, after:$reviewCursor) {
          pageInfo { hasNextPage endCursor }
          nodes { author { login } state commit { oid } }
        }
        reviewThreads(first:100, after:$threadCursor) {
          pageInfo { hasNextPage endCursor }
          nodes {
            isResolved
            comments(first:1) { nodes { author { login } body path line } }
          }
        }
      }
    }
  }' -f owner=nefarius -f repo=HydraHook -F pr=<num>
```

Page `reviews` with `$reviewCursor` and `reviewThreads` with `$threadCursor` while each connection's `hasNextPage` is true. Merge only when, for that `headRefOid`, every `reviewThreads` node has `isResolved: true` and no human review is `CHANGES_REQUESTED` without a later approval.

## Documentation

Keep these in sync with user-facing or architectural changes:

| Change | Update |
|---|---|
| Public C API, engine behavior, hook flow | `README.md`, `src/HydraHook/README.md`, Doxygen comments on `include/HydraHook/**` |
| Sample behavior | `samples/README.md` and that sample's `README.md` |
| Dependencies or supported platforms | `README.md`, `vcpkg.json` |
| CI, tests, or release layout | `README.md`, `src/HydraHook/README.md` Testing section |

There is no HydraHook section on docs.nefarius.at. Do not invent one.

## Build, tests, and dependencies

- Default branch is `master`. Solution is `HydraHook.sln`. Toolset is v143. Configurations: `Debug`, `Debug_LIB`, `Release`, `Release_LIB` for `Win32` and `x64`.
- `Debug` / `Release` produce `HydraHook.dll`. `Debug_LIB` / `Release_LIB` produce the static library and build the samples and `tests/cpp` projects against it.
- vcpkg is a submodule. The manifest baseline is pinned in `vcpkg-configuration.json`. Triplets are `x86-windows-static` and `x64-windows-static`. `Directory.Build.targets` keeps the install and consume paths on the same triplet directory.
- Install dependencies with `prepare-deps.bat` (both triplets), `prepare-deps.bat x64`, or `prepare-deps.bat Win32`.
- Build:

```text
msbuild HydraHook.sln /m /p:Configuration=Release_LIB /p:Platform=x64
msbuild HydraHook.sln /m /p:Configuration=Release /p:Platform=x64 /t:HydraHook
```

Use `Platform=Win32` for x86. `Release_LIB` writes test executables to `bin\x64` or `bin\x86`.

- Run `bin\<arch>\HydraHookTests.exe` and `bin\<arch>\HydraHookWarpSmokeTests.exe`. The smoke test needs `HydraHookTestHost.dll` beside it and a D3D11 WARP device.

## Releases

Tag `vX.Y.Z` or `vX.Y.Z-preNNN`. `.github/workflows/build.yml` builds, tests, signs, and publishes `HydraHook-x86.zip` and `HydraHook-x64.zip`.

Before the first tagged release, the `nefarius/HydraHook` repository needs:

- Secret `SIGN_RELAY_CI_TOKEN`
- Variable `SIGN_RELAY_SERVER` if it should not use `https://signrelay.api.nefarius.systems/`

## Source constraints

- Keep the public C API stable. Error codes live in `include/HydraHook/Engine/HydraHookCore.h`.
- Shutdown and loader-lock paths use `Hook::remove_nothrow` and `HookActivityTracker::Guard`. Do not replace those with throwing `remove()` on the detach path.
- New and edited source and config files are UTF-8 without BOM.
- Behavior changes in the engine, the public headers, or `tests/cpp` need a matching update under `tests/cpp`.
