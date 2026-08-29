# Releasing

Every part of Motion Master ships one version number. This document covers where that number lives, how it reaches each artefact, how to publish a release, and the CI workflows that build and check one.

## Versioning

All components — C++ backend, React UI, OpenAPI spec, and npm packages — share a single semver. `VERSION` (repo root) is the **canonical source**: a one-line plain-text file (e.g. `6.0.0-alpha.31`). There is no auto-increment — a human picks the next version. Everything else is either *derived* from `VERSION` at build time or *kept in sync* with it by the bump script.

### Bumping

Never edit `VERSION` by hand. Run the bump script with the new version:

```bash
./tools/bump-version.sh 6.1.0
./tools/bump-version.sh 6.1.0-alpha.0
```

It writes `VERSION`, then propagates the value to every location that *isn't* auto-derived: `vcpkg.json`, the `package.json` manifests (root workspace + `motion-master`, `motion-master-client`, `hil/api`), `swagger.yml` (`info.version`), the `version_test.cc` assertion, and the UI sidebar badge in `RootLayout.tsx`.

### How it reaches the C++ binary

CMake does the propagation into native code at configure time — `version.h` is **generated, never edited by hand**:

1. `CMakeLists.txt` reads the file into a variable: `file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" MM_VERSION)`.
2. `libs/core/CMakeLists.txt` runs `configure_file(version.h.in …)`, substituting `@MM_VERSION@` in the template to produce the build-dir `version.h`:

   ```cpp
   constexpr std::string_view kVersion = "6.0.0-alpha.31";
   static_assert(semver::valid(kVersion));  // build fails on a malformed version
   ```

The `static_assert` is a compile-time guard: a malformed version in `VERSION` breaks the build rather than shipping a bad string. The `Doxyfile` version is propagated the same way.

## Publishing a release

After bumping, commit the changed files, then push a `v<version>` tag to trigger the release workflow:

```bash
git add -A
git commit -m "chore: bump version to 6.1.0"
git tag v6.1.0
git push && git push --tags
```

The `v*` tag builds the platform binaries **and** publishes `@synapticon/motion-master-client@<version>` to npm (prereleases under the `next` dist-tag). Two drift nets back the sync: `version_test.cc` fails if its hard-coded string falls out of step, and the `api-client-drift` CI job fails if the committed API client is stale against `swagger.yml`.

## CI

| Workflow | Trigger | Purpose |
| --- | --- | --- |
| `build-linux-x64.yml` | push / PR to `main` | Build & test (Linux x64); vcpkg packages cached |
| `build-linux-arm64.yml` | push / PR to `main` | Build & test (Linux ARM64) |
| `build-macos-arm64.yml` | push / PR to `main` | Build & test (macOS Apple Silicon) |
| `build-windows-x64.yml` | push / PR to `main` | Build & test (Windows x64) |
| `lint.yml` | push / PR to `main` | Five gates: clang-format, cpplint, cppcheck, `api-client-drift` (the committed TS client must match `swagger.yml`), and `python-client-example` (every `operationId` the Python examples call must still resolve against the spec) |
| `api-tests.yml` | push to `main` | Run the [`hil/api`](DEVELOPMENT.md#api-integration-tests) HTTP + WebSocket integration tests against a containerised server |
| `cert-renewal.yml` | 1st of every month | Renew Let's Encrypt cert via acme-dns; publish it to the rolling `tls-cert` release |
| `deploy-pages.yml` | push to `main`, `v*` tag | Publish `motion-master.synapticon.com` — landing page, `/docs` Doxygen, the `swagger.yml` spec, and each PWA under `/apps/<name>/`. Docs and landing track `main`; the **apps are pinned to the latest `v*` tag** so the hosted console always matches a released binary |
| `jitter.yml` | manual dispatch | Run `jitter_bench` on a `PREEMPT_RT` CI machine (duration / period / workload as inputs) |
| `release.yml` | `v*` tag push | Build all platforms, bundle cert + key from the rolling `tls-cert` release, publish GitHub Release with `.tar.gz`, `.deb`, `.rpm` (Linux x64 and aarch64), `.zip` (Windows), and `.tar.gz` (macOS arm64); then code-sign the Windows exe on a self-hosted runner and replace the zip asset |
| `release-stats.yml` | every day at 04:00 UTC | Count the downloads of every release asset and publish `STATS.md` to the rolling `stats` release |

The vcpkg cache key is OS + `vcpkg.json` hash, extended with the architecture where two legs share an OS (`build-linux-arm64.yml`) and with the build container where the toolchain differs too (the release workflow's Debian 13 aarch64 leg). The first run after a dependency change rebuilds from source; subsequent runs restore from cache.
