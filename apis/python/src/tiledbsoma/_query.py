# Copyright (c) 2021-2023 The Chan Zuckerberg Initiative Foundation
# Copyright (c) 2021-2023 TileDB, Inc.
#
# Licensed under the MIT License.

"""Implementation of a SOMA Experiment.
"""
import enum
from concurrent.futures import ThreadPoolExecutor
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    Literal,
    Mapping,
    Optional,
    Protocol,
    Sequence,
    Tuple,
    TypeVar,
    cast,
    overload,
)

import attrs
import numpy as np
import numpy.typing as npt
import pandas as pd
import pyarrow as pa
import pyarrow.compute as pacomp
import scipy.sparse as sp
from anndata import AnnData
from somacore import (
    AxisQuery,
    DataFrame,
    ReadIter,
    SparseRead,
    query,
)
from somacore.data import _RO_AUTO
from somacore.options import (
    BatchSize,
    PlatformConfig,
    ReadPartitions,
    ResultOrder,
    ResultOrderStr,
)
from somacore.query.query import (
    AxisColumnNames,
    Numpyable,
)
from somacore.query.types import IndexFactory, IndexLike
from typing_extensions import Self

if TYPE_CHECKING:
    from ._experiment import Experiment
from ._fast_csr import read_csr
from ._measurement import Measurement
from ._sparse_nd_array import SparseNDArray

_T = TypeVar("_T")
_T_co = TypeVar("_T_co", covariant=True)


class _HasObsVar(Protocol[_T_co]):
    """Something which has an ``obs`` and ``var`` field.

    Used to give nicer type inference in :meth:`Axis.getattr_from`.
    """

    @property
    def obs(self) -> _T_co: ...

    @property
    def var(self) -> _T_co: ...


class Axis(enum.Enum):
    OBS = "obs"
    VAR = "var"

    @property
    def value(self) -> Literal["obs", "var"]:
        return super().value  # type: ignore[no-any-return]

    @overload
    def getattr_from(self, __source: _HasObsVar[_T]) -> _T: ...

    @overload
    def getattr_from(
        self, __source: Any, *, pre: Literal[""], suf: Literal[""]
    ) -> object: ...

    @overload
    def getattr_from(
        self, __source: Any, *, pre: str = ..., suf: str = ...
    ) -> object: ...

    def getattr_from(self, __source: Any, *, pre: str = "", suf: str = "") -> object:
        """Equivalent to ``something.<pre><obs/var><suf>``."""
        return getattr(__source, pre + self.value + suf)

    def getitem_from(
        self, __source: Mapping[str, "_T"], *, pre: str = "", suf: str = ""
    ) -> _T:
        """Equivalent to ``something[pre + "obs"/"var" + suf]``."""
        return __source[pre + self.value + suf]


@attrs.define
class AxisIndexer(query.AxisIndexer):
    """
    Given a query, provides index-building services for obs/var axis.

    Lifecycle: maturing
    """

    query: "ExperimentAxisQuery"
    _index_factory: IndexFactory
    _cached_obs: Optional[IndexLike] = None
    _cached_var: Optional[IndexLike] = None

    @property
    def _obs_index(self) -> IndexLike:
        """Private. Return an index for the ``obs`` axis."""
        if self._cached_obs is None:
            self._cached_obs = self._index_factory(self.query.obs_joinids().to_numpy())
        return self._cached_obs

    @property
    def _var_index(self) -> IndexLike:
        """Private. Return an index for the ``var`` axis."""
        if self._cached_var is None:
            self._cached_var = self._index_factory(self.query.var_joinids().to_numpy())
        return self._cached_var

    def by_obs(self, coords: Numpyable) -> npt.NDArray[np.intp]:
        """Reindex the coords (soma_joinids) over the ``obs`` axis."""
        return self._obs_index.get_indexer(_to_numpy(coords))

    def by_var(self, coords: Numpyable) -> npt.NDArray[np.intp]:
        """Reindex for the coords (soma_joinids) over the ``var`` axis."""
        return self._var_index.get_indexer(_to_numpy(coords))


def _to_numpy(it: Numpyable) -> npt.NDArray[np.int64]:
    if isinstance(it, np.ndarray):
        return it
    return it.to_numpy()


@attrs.define(frozen=True)
class AxisQueryResult:
    """The result of running :meth:`ExperimentAxisQuery.read`. Private."""

    obs: pd.DataFrame
    """Experiment.obs query slice, as a pandas DataFrame"""
    var: pd.DataFrame
    """Experiment.ms[...].var query slice, as a pandas DataFrame"""
    X: sp.csr_matrix
    """Experiment.ms[...].X[...] query slice, as a SciPy sparse.csr_matrix """
    X_layers: Dict[str, sp.csr_matrix] = attrs.field(factory=dict)
    """Any additional X layers requested, as SciPy sparse.csr_matrix(s)"""
    obsm: Dict[str, npt.NDArray[Any]] = attrs.field(factory=dict)
    """Experiment.obsm query slice, as a numpy ndarray"""
    obsp: Dict[str, npt.NDArray[Any]] = attrs.field(factory=dict)
    """Experiment.obsp query slice, as a numpy ndarray"""
    varm: Dict[str, npt.NDArray[Any]] = attrs.field(factory=dict)
    """Experiment.varm query slice, as a numpy ndarray"""
    varp: Dict[str, npt.NDArray[Any]] = attrs.field(factory=dict)
    """Experiment.varp query slice, as a numpy ndarray"""

    def to_anndata(self) -> AnnData:
        return AnnData(
            X=self.X,
            obs=self.obs,
            var=self.var,
            obsm=(self.obsm or None),
            obsp=(self.obsp or None),
            varm=(self.varm or None),
            varp=(self.varp or None),
            layers=(self.X_layers or None),
        )


class ExperimentAxisQuery:
    """Axis-based query against a SOMA Experiment.

    ExperimentAxisQuery allows easy selection and extraction of data from a
    single :class:`Measurement` in an :class:`Experiment`, by obs/var (axis) coordinates
    and/or value filter.

    The primary use for this class is slicing :class:`Experiment` ``X`` layers by obs or
    var value and/or coordinates. Slicing on :class:`SparseNDArray` ``X`` matrices is
    supported; :class:`DenseNDArray` is not supported at this time.

    IMPORTANT: this class is not thread-safe.

    IMPORTANT: this query class assumes it can store the full result of both
    axis dataframe queries in memory, and only provides incremental access to
    the underlying X NDArray. API features such as ``n_obs`` and ``n_vars``
    codify this in the API.

    IMPORTANT: you must call ``close()`` on any instance of this class to
    release underlying resources. The ExperimentAxisQuery is a context manager,
    and it is recommended that you use the following pattern to make this easy
    and safe::

        with ExperimentAxisQuery(...) as query:
            ...

    This base query implementation is designed to work against any SOMA
    implementation that fulfills the basic APIs. A SOMA implementation may
    include a custom query implementation optimized for its own use.

    Lifecycle: maturing
    """

    def __init__(
        self,
        experiment: "Experiment",
        measurement_name: str,
        *,
        obs_query: AxisQuery = AxisQuery(),
        var_query: AxisQuery = AxisQuery(),
        index_factory: IndexFactory = pd.Index,
    ):
        if measurement_name not in experiment.ms:
            raise ValueError("Measurement does not exist in the experiment")

        # Users often like to pass `foo=None` and we should let them
        obs_query = obs_query or AxisQuery()
        var_query = var_query or AxisQuery()

        self.experiment = experiment
        self.measurement_name = measurement_name

        self._matrix_axis_query = MatrixAxisQuery(obs=obs_query, var=var_query)
        self._joinids = JoinIDCache(self)
        self._indexer = AxisIndexer(
            self,
            index_factory=index_factory,
        )
        self._index_factory = index_factory

    def obs(
        self,
        *,
        column_names: Optional[Sequence[str]] = None,
        batch_size: BatchSize = BatchSize(),
        partitions: Optional[ReadPartitions] = None,
        result_order: ResultOrderStr = _RO_AUTO,
        platform_config: Optional[PlatformConfig] = None,
    ) -> ReadIter[pa.Table]:
        """Returns ``obs`` as an `Arrow table
        <https://arrow.apache.org/docs/python/generated/pyarrow.Table.html>`_
        iterator.

        Lifecycle: maturing
        """
        obs_query = self._matrix_axis_query.obs
        return self._obs_df.read(
            obs_query.coords,
            value_filter=obs_query.value_filter,
            column_names=column_names,
            batch_size=batch_size,
            partitions=partitions,
            result_order=result_order,
            platform_config=platform_config,
        )

    def var(
        self,
        *,
        column_names: Optional[Sequence[str]] = None,
        batch_size: BatchSize = BatchSize(),
        partitions: Optional[ReadPartitions] = None,
        result_order: ResultOrderStr = _RO_AUTO,
        platform_config: Optional[PlatformConfig] = None,
    ) -> ReadIter[pa.Table]:
        """Returns ``var`` as an `Arrow table
        <https://arrow.apache.org/docs/python/generated/pyarrow.Table.html>`_
        iterator.

        Lifecycle: maturing
        """
        var_query = self._matrix_axis_query.var
        return self._var_df.read(
            var_query.coords,
            value_filter=var_query.value_filter,
            column_names=column_names,
            batch_size=batch_size,
            partitions=partitions,
            result_order=result_order,
            platform_config=platform_config,
        )

    def obs_joinids(self) -> pa.IntegerArray:
        """Returns ``obs`` ``soma_joinids`` as an Arrow array.

        Lifecycle: maturing
        """
        return self._joinids.obs

    def var_joinids(self) -> pa.IntegerArray:
        """Returns ``var`` ``soma_joinids`` as an Arrow array.

        Lifecycle: maturing
        """
        return self._joinids.var

    @property
    def n_obs(self) -> int:
        """The number of ``obs`` axis query results.

        Lifecycle: maturing
        """
        return len(self.obs_joinids())

    @property
    def n_vars(self) -> int:
        """The number of ``var`` axis query results.

        Lifecycle: maturing
        """
        return len(self.var_joinids())

    @property
    def indexer(self) -> AxisIndexer:
        """A ``soma_joinid`` indexer for both ``obs`` and ``var`` axes.

        Lifecycle: maturing
        """
        return self._indexer

    def X(
        self,
        layer_name: str,
        *,
        batch_size: BatchSize = BatchSize(),
        partitions: Optional[ReadPartitions] = None,
        result_order: ResultOrderStr = _RO_AUTO,
        platform_config: Optional[PlatformConfig] = None,
    ) -> SparseRead:
        """Returns an ``X`` layer as a sparse read.

        Args:
            layer_name: The X layer name to return.
            batch_size: The size of batches that should be returned from a read.
                See :class:`BatchSize` for details.
            partitions: Specifies that this is part of a partitioned read,
                and which partition to include, if present.
            result_order: the order to return results, specified as a
                :class:`~ResultOrder` or its string value.

        Lifecycle: maturing
        """
        try:
            x_layer = self._ms.X[layer_name]
        except KeyError as ke:
            raise KeyError(f"{layer_name} is not present in X") from ke
        if not isinstance(x_layer, SparseNDArray):
            raise TypeError("X layers may only be sparse arrays")

        self._joinids.preload(self._threadpool)
        return x_layer.read(
            (self._joinids.obs, self._joinids.var),
            batch_size=batch_size,
            partitions=partitions,
            result_order=result_order,
            platform_config=platform_config,
        )

    def obsp(self, layer: str) -> SparseRead:
        """Returns an ``obsp`` layer as a sparse read.

        Lifecycle: maturing
        """
        return self._axisp_inner(Axis.OBS, layer)

    def varp(self, layer: str) -> SparseRead:
        """Returns a ``varp`` layer as a sparse read.

        Lifecycle: maturing
        """
        return self._axisp_inner(Axis.VAR, layer)

    def obsm(self, layer: str) -> SparseRead:
        """Returns an ``obsm`` layer as a sparse read.
        Lifecycle: maturing
        """
        return self._axism_inner(Axis.OBS, layer)

    def varm(self, layer: str) -> SparseRead:
        """Returns a ``varm`` layer as a sparse read.
        Lifecycle: maturing
        """
        return self._axism_inner(Axis.VAR, layer)

    def obs_scene_ids(self) -> pa.Array:
        """Returns a pyarrow array with scene ids that contain obs from this
        query.

        Lifecycle: experimental
        """
        try:
            obs_scene = self.experiment.obs_spatial_presence
        except KeyError as ke:
            raise KeyError("Missing obs_scene") from ke
        if not isinstance(obs_scene, DataFrame):
            raise TypeError("obs_scene must be a dataframe.")

        full_table = obs_scene.read(
            coords=((Axis.OBS.getattr_from(self._joinids), slice(None))),
            result_order=ResultOrder.COLUMN_MAJOR,
            value_filter="data != 0",
        ).concat()

        return pacomp.unique(full_table["scene_id"])

    def var_scene_ids(self) -> pa.Array:
        """Return a pyarrow array with scene ids that contain var from this
        query.

        Lifecycle: experimental
        """
        try:
            var_scene = self._ms.var_spatial_presence
        except KeyError as ke:
            raise KeyError("Missing var_scene") from ke
        if not isinstance(var_scene, DataFrame):
            raise TypeError("var_scene must be a dataframe.")

        full_table = var_scene.read(
            coords=((Axis.OBS.getattr_from(self._joinids), slice(None))),
            result_order=ResultOrder.COLUMN_MAJOR,
            value_filter="data != 0",
        ).concat()

        return pacomp.unique(full_table["scene_id"])

    def to_anndata(
        self,
        X_name: str,
        *,
        column_names: Optional[AxisColumnNames] = None,
        X_layers: Sequence[str] = (),
        obsm_layers: Sequence[str] = (),
        obsp_layers: Sequence[str] = (),
        varm_layers: Sequence[str] = (),
        varp_layers: Sequence[str] = (),
        drop_levels: bool = False,
    ) -> AnnData:
        ad = self._read(
            X_name,
            column_names=column_names or AxisColumnNames(obs=None, var=None),
            X_layers=X_layers,
            obsm_layers=obsm_layers,
            obsp_layers=obsp_layers,
            varm_layers=varm_layers,
            varp_layers=varp_layers,
        ).to_anndata()

        # Drop unused categories on axis dataframes if requested
        if drop_levels:
            for name in ad.obs:
                if ad.obs[name].dtype.name == "category":
                    ad.obs[name] = ad.obs[name].cat.remove_unused_categories()
            for name in ad.var:
                if ad.var[name].dtype.name == "category":
                    ad.var[name] = ad.var[name].cat.remove_unused_categories()

        return ad

    # Context management

    def close(self) -> None:
        pass

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()

    def __del__(self) -> None:
        """Ensure that we're closed when our last ref disappears."""
        self.close()
        # If any superclass in our MRO has a __del__, call it.
        sdel = getattr(super(), "__del__", lambda: None)
        sdel()

    # Internals

    def _read(
        self,
        X_name: str,
        *,
        column_names: AxisColumnNames,
        X_layers: Sequence[str],
        obsm_layers: Sequence[str] = (),
        obsp_layers: Sequence[str] = (),
        varm_layers: Sequence[str] = (),
        varp_layers: Sequence[str] = (),
    ) -> AxisQueryResult:
        """Reads the entire query result in memory.

        This is a low-level routine intended to be used by loaders for other
        in-core formats, such as AnnData, which can be created from the
        resulting objects.

        Args:
            X_name: The X layer to read and return in the ``X`` slot.
            column_names: The columns in the ``var`` and ``obs`` dataframes
                to read.
            X_layers: Additional X layers to read and return
                in the ``layers`` slot.
            obsm_layers:
                Additional obsm layers to read and return in the obsm slot.
            obsp_layers:
                Additional obsp layers to read and return in the obsp slot.
            varm_layers:
                Additional varm layers to read and return in the varm slot.
            varp_layers:
                Additional varp layers to read and return in the varp slot.
        """
        x_collection = self._ms.X
        all_x_names = [X_name] + list(X_layers)
        all_x_arrays: Dict[str, SparseNDArray] = {}
        for _xname in all_x_names:
            if not isinstance(_xname, str) or not _xname:
                raise ValueError("X layer names must be specified as a string.")
            if _xname not in x_collection:
                raise ValueError("Unknown X layer name")
            x_array = x_collection[_xname]
            if not isinstance(x_array, SparseNDArray):
                raise NotImplementedError("Dense array unsupported")
            all_x_arrays[_xname] = x_array

        def _read_axis_mappings(
            fn: Callable[[Axis, str], npt.NDArray[Any]],
            axis: Axis,
            keys: Sequence[str],
        ) -> Dict[str, npt.NDArray[Any]]:
            return {key: fn(axis, key) for key in keys}

        obsm_ft = self._threadpool.submit(
            _read_axis_mappings, self._axism_inner_ndarray, Axis.OBS, obsm_layers
        )
        obsp_ft = self._threadpool.submit(
            _read_axis_mappings, self._axisp_inner_ndarray, Axis.OBS, obsp_layers
        )
        varm_ft = self._threadpool.submit(
            _read_axis_mappings, self._axism_inner_ndarray, Axis.VAR, varm_layers
        )
        varp_ft = self._threadpool.submit(
            _read_axis_mappings, self._axisp_inner_ndarray, Axis.VAR, varp_layers
        )

        obs_table, var_table = self._read_both_axes(column_names)

        obs_joinids = self.obs_joinids()
        var_joinids = self.var_joinids()

        x_matrices = {
            _xname: (
                read_csr(
                    layer,
                    obs_joinids,
                    var_joinids,
                    index_factory=self._index_factory,
                ).to_scipy()
            )
            for _xname, layer in all_x_arrays.items()
        }

        x = x_matrices.pop(X_name)

        obs = obs_table.to_pandas()
        obs.index = obs.index.astype(str)

        var = var_table.to_pandas()
        var.index = var.index.astype(str)

        return AxisQueryResult(
            obs=obs,
            var=var,
            X=x,
            obsm=obsm_ft.result(),
            obsp=obsp_ft.result(),
            varm=varm_ft.result(),
            varp=varp_ft.result(),
            X_layers=x_matrices,
        )

    def _read_both_axes(
        self,
        column_names: AxisColumnNames,
    ) -> Tuple[pa.Table, pa.Table]:
        """Reads both axes in their entirety, ensuring soma_joinid is retained."""
        obs_ft = self._threadpool.submit(
            self._read_axis_dataframe,
            Axis.OBS,
            column_names,
        )
        var_ft = self._threadpool.submit(
            self._read_axis_dataframe,
            Axis.VAR,
            column_names,
        )
        return obs_ft.result(), var_ft.result()

    def _read_axis_dataframe(
        self,
        axis: Axis,
        axis_column_names: AxisColumnNames,
    ) -> pa.Table:
        """Reads the specified axis. Will cache join IDs if not present."""
        column_names = axis_column_names.get(axis.value)

        axis_df = axis.getattr_from(self, pre="_", suf="_df")
        assert isinstance(axis_df, DataFrame)
        axis_query = axis.getattr_from(self._matrix_axis_query)

        # If we can cache join IDs, prepare to add them to the cache.
        joinids_cached = self._joinids._is_cached(axis)
        query_columns = column_names
        added_soma_joinid_to_columns = False
        if (
            not joinids_cached
            and column_names is not None
            and "soma_joinid" not in column_names
        ):
            # If we want to fill the join ID cache, ensure that we query the
            # soma_joinid column so that it is included in the results.
            # We'll filter it out later.
            query_columns = ["soma_joinid"] + list(column_names)
            added_soma_joinid_to_columns = True

        # Do the actual query.
        arrow_table = axis_df.read(
            coords=axis_query.coords,
            value_filter=axis_query.value_filter,
            column_names=query_columns,
        ).concat()

        # Update the cache if needed. We can do this because no matter what
        # other columns are queried for, the contents of the ``soma_joinid``
        # column will be the same and can be safely stored.
        if not joinids_cached:
            setattr(
                self._joinids,
                axis.value,
                arrow_table.column("soma_joinid").combine_chunks(),
            )

        # Drop soma_joinid column if we added it solely for use in filling
        # the joinid cache.
        if added_soma_joinid_to_columns:
            arrow_table = arrow_table.drop(["soma_joinid"])
        return arrow_table

    def _axisp_inner(
        self,
        axis: Axis,
        layer: str,
    ) -> SparseRead:
        p_name = f"{axis.value}p"
        try:
            ms = self._ms
            axisp = ms.obsp if axis.value == "obs" else ms.varp
        except (AttributeError, KeyError):
            raise ValueError(f"Measurement does not contain {p_name} data")

        try:
            ap_layer = axisp[layer]
        except KeyError:
            raise ValueError(f"layer {layer!r} is not available in {p_name}")
        if not isinstance(ap_layer, SparseNDArray):
            raise TypeError(
                f"Unexpected SOMA type {type(ap_layer).__name__}"
                f" stored in {p_name} layer {layer!r}"
            )

        joinids = axis.getattr_from(self._joinids)
        return ap_layer.read((joinids, joinids))

    def _axism_inner(
        self,
        axis: Axis,
        layer: str,
    ) -> SparseRead:
        m_name = f"{axis.value}m"

        try:
            ms = self._ms
            axism = ms.obsm if axis.value == "obs" else ms.varm
        except (AttributeError, KeyError):
            raise ValueError(f"Measurement does not contain {m_name} data")

        try:
            axism_layer = axism[layer]
        except KeyError:
            raise ValueError(f"layer {layer!r} is not available in {m_name}")

        if not isinstance(axism_layer, SparseNDArray):
            raise TypeError(f"Unexpected SOMA type stored in '{m_name}' layer")

        joinids = axis.getattr_from(self._joinids)
        return axism_layer.read((joinids, slice(None)))

    def _convert_to_ndarray(
        self, axis: Axis, table: pa.Table, n_row: int, n_col: int
    ) -> npt.NDArray[np.float32]:
        indexer = cast(
            Callable[[Numpyable], npt.NDArray[np.intp]],
            axis.getattr_from(self.indexer, pre="by_"),
        )
        idx = indexer(table["soma_dim_0"])
        z: npt.NDArray[np.float32] = np.zeros(n_row * n_col, dtype=np.float32)
        np.put(z, idx * n_col + table["soma_dim_1"], table["soma_data"])
        return z.reshape(n_row, n_col)

    def _axisp_inner_ndarray(
        self,
        axis: Axis,
        layer: str,
    ) -> npt.NDArray[np.float32]:
        n_row = n_col = len(axis.getattr_from(self._joinids))

        table = self._axisp_inner(axis, layer).tables().concat()
        return self._convert_to_ndarray(axis, table, n_row, n_col)

    def _axism_inner_ndarray(
        self,
        axis: Axis,
        layer: str,
    ) -> npt.NDArray[np.float32]:
        table = self._axism_inner(axis, layer).tables().concat()

        n_row = len(axis.getattr_from(self._joinids))
        n_col = len(table["soma_dim_1"].unique())

        return self._convert_to_ndarray(axis, table, n_row, n_col)

    @property
    def _obs_df(self) -> DataFrame:
        return self.experiment.obs

    @property
    def _ms(self) -> Measurement:
        return self.experiment.ms[self.measurement_name]

    @property
    def _var_df(self) -> DataFrame:
        return self._ms.var

    @property
    def _threadpool(self) -> ThreadPoolExecutor:
        """
        Returns the threadpool provided by the experiment's context.
        If not available, creates a thread pool just in time."""
        return self.experiment.context.threadpool


@attrs.define(frozen=True)
class MatrixAxisQuery:
    """The per-axis user query definition. Private."""

    obs: AxisQuery
    var: AxisQuery


@attrs.define
class JoinIDCache:
    """A cache for per-axis join ids in the query. Private."""

    owner: ExperimentAxisQuery

    _cached_obs: Optional[pa.IntegerArray] = None
    _cached_var: Optional[pa.IntegerArray] = None

    def _is_cached(self, axis: Axis) -> bool:
        field = "_cached_" + axis.value
        return getattr(self, field) is not None

    def preload(self, pool: ThreadPoolExecutor) -> None:
        if self._cached_obs is not None and self._cached_var is not None:
            return
        obs_ft = pool.submit(lambda: self.obs)
        var_ft = pool.submit(lambda: self.var)
        # Wait for them and raise in case of error.
        obs_ft.result()
        var_ft.result()

    @property
    def obs(self) -> pa.IntegerArray:
        """Join IDs for the obs axis. Will load and cache if not already."""
        if not self._cached_obs:
            self._cached_obs = load_joinids(
                self.owner._obs_df, self.owner._matrix_axis_query.obs
            )
        return self._cached_obs

    @obs.setter
    def obs(self, val: pa.IntegerArray) -> None:
        self._cached_obs = val

    @property
    def var(self) -> pa.IntegerArray:
        """Join IDs for the var axis. Will load and cache if not already."""
        if not self._cached_var:
            self._cached_var = load_joinids(
                self.owner._var_df, self.owner._matrix_axis_query.var
            )
        return self._cached_var

    @var.setter
    def var(self, val: pa.IntegerArray) -> None:
        self._cached_var = val


def load_joinids(df: DataFrame, axq: AxisQuery) -> pa.IntegerArray:
    tbl = df.read(
        axq.coords,
        value_filter=axq.value_filter,
        column_names=["soma_joinid"],
    ).concat()
    return tbl.column("soma_joinid").combine_chunks()