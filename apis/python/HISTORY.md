# TileDB-SOMA Python Changelog

All notable changes to the Python TileDB-SOMA project will be documented in this file (related: [TileDB-SOMA R API changelog](../r/NEWS.md)).

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

### Changed

### Deprecated

### Removed

### Fixed

- \[[#4071](https://github.com/single-cell-data/TileDB-SOMA/pull/4071)\] [python] A `tiledb_timestamp` with value of zero is now equivalent to an unspecified timestamp (or `None`), and will be a synonym for "current time". Prior to this fix, a zero-valued timestamp would generate errors or unpredictable results.

### Security

## [Release 1.17.0]

The primary change in 1.17.0 is the upgrade to TileDB 2.28.

### Added

- \[[#3740](https://github.com/single-cell-data/TileDB-SOMA/pull/3740)\] [python] Add experimental Dask-backed `to_anndata` functionality to `SparseNDArray` and `ExperimentAxisQuery`.

### Changed

- \[\[[#4057](https://github.com/single-cell-data/TileDB-SOMA/pull/4057)\] [c++] Update [TileDB core to 2.28.0](https://github.com/TileDB-Inc/TileDB/blob/main/HISTORY.md#tiledb-v2280-release-notes)
- \[[#4023](https://github.com/single-cell-data/TileDB-SOMA/pull/4023)\] [c++] Use nanoarrow ArrowSchemaSetTypeDateTime for datetime values. Dictionary type with timestamp value type will raise error on read.

### Fixed

- \[[#4040](https://github.com/single-cell-data/TileDB-SOMA/pull/4040)\] [python] suppress insignificant overflow warning from numpy.

- \[[#4050](https://github.com/single-cell-data/TileDB-SOMA/pull/4050)\] DataFrame `count` and SparseNDArray `nnz` fix - report correct number of cells in array in the case where a delete query had been previously applied.

- \[[#4055](https://github.com/single-cell-data/TileDB-SOMA/pull/4055)\] Various `open()` code paths failed to check the SOMA encoding version number, and would fail with cryptic errors.
- \[[#4066](https://github.com/single-cell-data/TileDB-SOMA/pull/4066)\] Fix various memory leaks related to releasing Arrow structures when transfering ownership between C++ and Python and vise versa.

- \[[#4031](https://github.com/single-cell-data/TileDB-SOMA/pull/4031)\] [python] Storage paths generated from collection keys are now URL-escaped if they contain characters outside the safe set (`a-zA-Z0-9-_.()^!@+={}~'`). Additionally, the special names `..` and `.` are now prohibited.

## [Release prior to 1.17.0]

TileDB-SOMA Python releases prior to 1.17.0 are documented in the [TileDB-SOMA Github Releases](https://github.com/single-cell-data/TileDB-SOMA/releases).
