# Copyright (c) 2021-2023 The Chan Zuckerberg Initiative Foundation
# Copyright (c) 2021-2023 TileDB, Inc.
#
# Licensed under the MIT License.

"""Conversion utility methods.
"""

from __future__ import annotations

from typing import TypeVar, cast

import numpy as np
import pandas as pd
import pandas._typing as pdt
import pyarrow as pa
import scipy.sparse as sp

from .._fastercsx import CompressedMatrix
from .._funcs import typeguard_ignore
from .._types import NPNDArray, PDSeries
from ..options._soma_tiledb_context import SOMATileDBContext

_DT = TypeVar("_DT", bound=pdt.Dtype)
_MT = TypeVar("_MT", NPNDArray, sp.spmatrix, PDSeries)
_str_to_type = {"boolean": bool, "string": str, "bytes": bytes}


def decategoricalize_obs_or_var(obs_or_var: pd.DataFrame) -> pd.DataFrame:
    """Performs a typecast into types that TileDB can persist."""
    if len(obs_or_var.columns) > 0:
        return pd.DataFrame.from_dict(
            {
                str(k): to_tiledb_supported_array_type(str(k), v)
                for k, v in obs_or_var.items()
            },
        )
    else:
        return obs_or_var.copy()


@typeguard_ignore
def _to_tiledb_supported_dtype(dtype: _DT) -> _DT:
    """A handful of types are cast into the TileDB type system."""
    # TileDB has no float16 -- cast up to float32
    return cast(_DT, np.dtype("float32")) if dtype == np.dtype("float16") else dtype


def to_tiledb_supported_array_type(name: str, x: _MT) -> _MT:
    """Converts datatypes unrepresentable by TileDB into datatypes it can represent.
    E.g., float16 -> float32
    """
    if isinstance(x, (np.ndarray, sp.spmatrix)) or not isinstance(
        x.dtype, pd.CategoricalDtype
    ):
        target_dtype = _to_tiledb_supported_dtype(x.dtype)
        return x if target_dtype == x.dtype else x.astype(target_dtype)

    # categories = x.cat.categories
    # cat_dtype = categories.dtype
    # if cat_dtype.kind in ("f", "u", "i"):
    #     if x.hasnans and cat_dtype.kind == "i":
    #         raise ValueError(
    #             f"Categorical column {name!r} contains NaN -- unable to convert to TileDB array."
    #         )
    #     # More mysterious spurious mypy errors.
    #     target_dtype = _to_tiledb_supported_dtype(cat_dtype)  # type: ignore[arg-type]
    # else:
    #     # Into the weirdness. See if Pandas can help with edge cases.
    #     inferred = infer_dtype(categories)
    #     if x.hasnans and inferred in ("boolean", "bytes"):
    #         raise ValueError(
    #             "Categorical array contains NaN -- unable to convert to TileDB array."
    #         )
    #     target_dtype = np.dtype(  # type: ignore[assignment]
    #         _str_to_type.get(inferred, object)
    #     )

    # return x.astype(target_dtype)
    return x


def csr_from_coo_table(
    tbl: pa.Table, num_rows: int, num_cols: int, context: SOMATileDBContext
) -> sp.csr_matrix:
    """Given an Arrow Table containing COO data, return a ``scipy.sparse.csr_matrix``."""
    s = CompressedMatrix.from_soma(
        tbl, (num_rows, num_cols), "csr", True, context
    ).to_scipy()
    return s
