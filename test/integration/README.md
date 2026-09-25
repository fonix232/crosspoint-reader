# BookOrbit server integration tests

This suite runs the production C++ wire codec, streaming response parser and
progress-conflict policy through `BookOrbitProtocolProbe`, with real HTTP
requests to an isolated BookOrbit server. Native storage tests exercise the
production queue and annotation ledger using a fault-injecting SD adapter.
It does not simulate the ESP32 Wi-Fi stack, e-ink UI or physical power loss.

Requirements: Docker Compose, CMake, a C++20 compiler and Python 3.10+.
The test server listens only on localhost:3998. `test.env` contains test-only
credentials and pins the BookOrbit image. The Compose project and its volumes
are named separately from any development server.

```sh
cmake -S test -B build/test
cmake --build build/test --target BookOrbitProtocolProbe BookOrbitDataTest BookOrbitDataStoreTest -j4
ctest --test-dir build/test -R 'BookOrbitData' --output-on-failure
docker compose --env-file test/integration/test.env -f test/integration/docker-compose.yml up -d --wait
python3 test/integration/seed/seed.py
python3 test/integration/run.py
docker compose --env-file test/integration/test.env -f test/integration/docker-compose.yml down -v
```

The seeder creates 100 distinct EPUBs, two collections (one empty), progress,
bookmarks and highlights. The test runner creates unique device identities.
Tests cover progress conflicts including backward movement, lost-response
reading-event retries, annotation add/edit/delete and acknowledgement retries,
missing-history deletion guards, bookmark identities, exact sideload matching,
book pagination, search and collection filtering. One test explicitly sets a
metadata title because dock imports can precede metadata extraction.

The native storage adapter deliberately rejects MD5 calls: identity hashing is
provided by the ESP32 SDK, not a substitute test implementation. Storage cases
supply explicit keys and exercise production recovery logic.

Seeder attribution is in `seed/NOTICE.md` and `seed/LICENSE.crossink`.
