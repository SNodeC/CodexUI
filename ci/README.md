# Repository-owned package builds

`packages.json` uses schema 2. `find_package(AISuite CONFIG REQUIRED)` has no version bound, so the declaration uses `>=0.0.0`. Tagged source `v0.2.0` uses `find_package(snodec 2.0 CONFIG REQUIRED ...)` with SNode.C's SameMajorVersion contract, declared as `>=2.0,<3`. Master's unreleased 3.0 change does not change that tagged source requirement. This conversion preserves source requirements, targets, prerequisite lists and the existing package test policy.

The chain checks out tagged source and submodules into `SOURCE_DIR` and independent CI assets into `CI_DIR`. The native hook selects `$CI_DIR/package/config.cmake`, shared with local CPack. It records exact dependency versions/revisions from `DEPENDENCIES_JSON` and preserves the existing Sonnet plugin dependency. Core-ci checks the declaration's compatibility ranges while planning. No product source is changed.
