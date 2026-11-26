# Repository-owned package builds

`packages.json` uses schema 2. `find_package(AISuite CONFIG REQUIRED)` has no version bound, so the declaration uses `>=0.0.0`. Direct `find_package(snodec 3.0.0 CONFIG REQUIRED ...)` uses SNode.C's SameMajorVersion contract, declared as `>=3.0.0,<4`. This conversion preserves source requirements, targets, prerequisite lists and the existing package test policy.

The chain checks out tagged source and submodules into `SOURCE_DIR` and independent CI assets into `CI_DIR`. The native hook selects `$CI_DIR/package/config.cmake`, shared with local CPack. It records dependency version/revision lower bounds from `DEPENDENCIES_JSON` and preserves the SNode.C major-version upper bound and existing Sonnet plugin dependency. No product source is changed.
