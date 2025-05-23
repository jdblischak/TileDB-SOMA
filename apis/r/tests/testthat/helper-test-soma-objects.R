# Returns the object created, populated, and closed (unless otherwise requested)
create_and_populate_soma_dataframe <- function(
  uri,
  nrows = 10L,
  seed = 1,
  index_column_names = "int_column",
  factors = FALSE,
  mode = NULL
) {
  set.seed(seed)

  tbl <- create_arrow_table(nrows = nrows, factors = factors)

  full_domain <- domain_for_arrow_table()
  # Pick out the index-column names actually being used in this case
  domain <- list()
  for (index_column in index_column_names) {
    domain[[index_column]] <- full_domain[[index_column]]
  }

  sdf <- SOMADataFrameCreate(
    uri,
    tbl$schema,
    index_column_names = index_column_names,
    domain = domain
  )

  sdf$write(tbl)

  if (is.null(mode)) {
    sdf$close()
  } else if (mode == "READ") {
    sdf$close()
    sdf <- SOMADataFrameOpen(uri, mode = mode)
  }
  sdf
}

# Returns the object created, populated, and closed (unless otherwise requested)
create_and_populate_obs <- function(
  uri,
  nrows = 10L,
  seed = 1,
  factors = FALSE,
  mode = NULL
) {
  create_and_populate_soma_dataframe(
    uri = uri,
    nrows = nrows,
    seed = seed,
    index_column_names = "soma_joinid",
    factors = factors,
    mode = mode
  )
}

# Returns the object created, populated, and closed (unless otherwise requested)
create_and_populate_var <- function(
  uri,
  nrows = 10L,
  seed = 1,
  factors = FALSE,
  mode = NULL
) {
  tbl <- arrow::arrow_table(
    soma_joinid = seq(bit64::as.integer64(0L), to = nrows - 1L),
    quux = as.character(seq.int(nrows) + 1000L),
    xyzzy = runif(nrows),
    schema = arrow::schema(
      arrow::field("soma_joinid", arrow::int64(), nullable = FALSE),
      arrow::field("quux", arrow::large_utf8(), nullable = FALSE),
      arrow::field("xyzzy", arrow::float64(), nullable = FALSE)
    )
  )
  if (isTRUE(factors)) {
    tbl$grp <- factor(c(
      rep_len("lvl1", length.out = floor(nrows / 2)),
      rep_len("lvl2", length.out = floor(nrows / 2))
    ))
  }
  domain <- list(
    soma_joinid = c(0, nrows - 1L)
  )

  dname <- dirname(uri)
  if (!dir.exists(dname)) dir.create(dname)

  sdf <- SOMADataFrameCreate(uri, tbl$schema, index_column_names = "soma_joinid", domain = domain)
  sdf$write(tbl)

  if (is.null(mode)) {
    sdf$close()
  } else if (mode == "READ") {
    sdf$close()
    sdf <- SOMADataFrameOpen(uri, mode = mode)
  }
  sdf
}

# Creates a SOMAExperiment with a single measurement, "RNA"
# Returns the object created, populated, and closed (unless otherwise requested)
#' @param ... Arguments passed to create_sparse_matrix_with_int_dims
create_and_populate_sparse_nd_array <- function(uri, mode = NULL, ...) {
  smat <- create_sparse_matrix_with_int_dims(...)

  ndarray <- SOMASparseNDArrayCreate(uri, arrow::int32(), shape = dim(smat))
  ndarray$write(smat)

  if (is.null(mode)) {
    ndarray$close()
  } else if (mode == "READ") {
    ndarray$close()
    ndarray <- SOMASparseNDArrayOpen(uri, mode = mode)
  }
  ndarray
}

# Creates a SOMAExperiment with a single measurement, "RNA"; populates it;
# returns it closed (unless otherwise requested).
#
# Example with X_layer_names = c("counts", "logcounts"):
#  soma-experiment-query-all1c20a1d341584 GROUP
#  |-- obs ARRAY
#  |-- ms GROUP
#  |------ RNA GROUP
#  |---------- var ARRAY
#  |---------- X GROUP
#  |-------------- counts ARRAY
#  |-------------- logcounts ARRAY
#' @param obsm_layers A named integer vector of layers to add to `obsm`; the
#' names will be used to create new layers in `obsm` and the value will determine
#' the number of dimensions (columns) to add. Names starting with `dense:` will
#' be created as _dense_ arrays (eg. `dense:X_ica`). Pass `NULL` to prevent
#' creation of `obsm` layers
#' @param varm_layers A named integer vector of layers to add to `varm`; the
#' names will be used to create new layers in `varm` and the value will determine
#' the number of dimensions (columns) to add. Names starting with `dense:` will
#' be created as _dense_ arrays (eg. `dense:ICs`). Pass `NULL` to prevent
#' creation of `varm` layers
#' @param obsp_layers A character vector of `obsp` layers; pass `NULL` to
#' prevent creation of `obsp` layers
#' @param varp_layers A character vector of `varp` layers; pass `NULL` to
#' prevent creation of `varp` layers
#'
create_and_populate_experiment <- function(
  uri,
  n_obs,
  n_var,
  X_layer_names,
  obsm_layers = NULL,
  varm_layers = NULL,
  obsp_layer_names = NULL,
  varp_layer_names = NULL,
  config = NULL,
  factors = FALSE,
  mode = NULL
) {
  stopifnot(
    "'obsm_layers' must be a named integer vector" = is.null(obsm_layers) ||
      (rlang::is_integerish(obsm_layers) && rlang::is_named(obsm_layers) && all(obsm_layers > 0L)),
    "'varm_layers' must be a named integer vector" = is.null(varm_layers) ||
      (rlang::is_integerish(varm_layers) && rlang::is_named(varm_layers) && all(varm_layers > 0L)),
    "'obsp_layer_names' must be a character vector" = is.null(obsp_layer_names) ||
      (is.character(obsp_layer_names) && all(nzchar(obsp_layer_names))),
    "'varp_layer_names' must be a character vector" = is.null(varp_layer_names) ||
      (is.character(varp_layer_names) && all(nzchar(varp_layer_names)))
  )

  experiment <- SOMAExperimentCreate(uri, platform_config = config)

  experiment$obs <- create_and_populate_obs(
    uri = file.path(uri, "obs"),
    nrows = n_obs,
    factors = factors
  )

  experiment$ms <- SOMACollectionCreate(file.path(uri, "ms"))

  ms_rna <- SOMAMeasurementCreate(file.path(uri, "ms", "RNA"))
  ms_rna$var <- create_and_populate_var(
    uri = file.path(ms_rna$uri, "var"),
    nrows = n_var,
    factors = factors
  )
  ms_rna$X <- SOMACollectionCreate(file.path(ms_rna$uri, "X"))

  for (layer_name in X_layer_names) {
    snda <- create_and_populate_sparse_nd_array(
      uri = file.path(ms_rna$X$uri, layer_name),
      nrows = n_obs,
      ncols = n_var
    )
    ms_rna$X$set(snda, name = layer_name)
  }
  ms_rna$X$close()

  # Add obsm layers
  if (rlang::is_integerish(obsm_layers)) {
    obsm <- SOMACollectionCreate(file.path(ms_rna$uri, "obsm"))
    for (layer in names(obsm_layers)) {
      key <- gsub(pattern = "^dense:", replacement = "", x = layer)
      shape <- c(n_obs, obsm_layers[layer])
      if (grepl(pattern = "^dense:", x = layer)) {
        obsm$add_new_dense_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        obsm$get(key)$write(create_dense_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      } else {
        obsm$add_new_sparse_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        obsm$get(key)$write(create_sparse_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      }
    }
    obsm$close()
    ms_rna$add_new_collection(obsm, "obsm")
  }

  # Add varm layers
  if (rlang::is_integerish(varm_layers)) {
    varm <- SOMACollectionCreate(file.path(ms_rna$uri, "varm"))
    for (layer in names(varm_layers)) {
      key <- gsub(pattern = "^dense:", replacement = "", x = layer)
      shape <- c(n_var, varm_layers[layer])
      if (grepl(pattern = "^dense:", x = layer)) {
        varm$add_new_dense_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        varm$get(key)$write(create_dense_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      } else {
        varm$add_new_sparse_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        varm$get(key)$write(create_sparse_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      }
    }
    varm$close()
    ms_rna$add_new_collection(varm, "varm")
  }

  # Add obsp layers
  if (is.character(obsp_layer_names)) {
    obsp <- SOMACollectionCreate(file.path(ms_rna$uri, "obsp"))
    for (layer in obsp_layer_names) {
      obsp$add_new_sparse_ndarray(
        key = layer,
        type = arrow::int32(),
        shape = c(n_obs, n_obs)
      )
      obsp$get(layer)$write(create_sparse_matrix_with_int_dims(
        nrows = n_obs,
        ncols = n_obs
      ))
    }
    obsp$close()
    ms_rna$add_new_collection(obsp, "obsp")
  }

  # Add varp layers
  if (is.character(varp_layer_names)) {
    varp <- SOMACollectionCreate(file.path(ms_rna$uri, "varp"))
    for (layer in varp_layer_names) {
      varp$add_new_sparse_ndarray(
        key = layer,
        type = arrow::int32(),
        shape = c(n_var, n_var)
      )
      varp$get(layer)$write(create_sparse_matrix_with_int_dims(
        nrows = n_var,
        ncols = n_var
      ))
    }
    varp$close()
    ms_rna$add_new_collection(varp, "varp")
  }

  ms_rna$close()

  experiment$ms$set(ms_rna, name = "RNA")
  experiment$ms$close()

  if (is.null(mode)) {
    experiment$close()
  } else if (mode == "READ") {
    experiment$close()
    experiment <- SOMAExperimentOpen(uri, mode = mode)
  }
  experiment
}

create_and_populate_ragged_experiment <- function(
  uri,
  n_obs,
  n_var,
  X_layer_names,
  obsm_layers = NULL,
  varm_layers = NULL,
  obsp_layer_names = NULL,
  varp_layer_names = NULL,
  config = NULL,
  factors = FALSE,
  mode = NULL,
  seed = NA_integer_
) {

  stopifnot(
    "'obsm_layers' must be a named integer vector" = is.null(obsm_layers) ||
      (rlang::is_integerish(obsm_layers) && rlang::is_named(obsm_layers) && all(obsm_layers > 0L)),
    "'varm_layers' must be a named integer vector" = is.null(varm_layers) ||
      (rlang::is_integerish(varm_layers) && rlang::is_named(varm_layers) && all(varm_layers > 0L)),
    "'obsp_layer_names' must be a character vector" = is.null(obsp_layer_names) ||
      (is.character(obsp_layer_names) && all(nzchar(obsp_layer_names))),
    "'varp_layer_names' must be a character vector" = is.null(varp_layer_names) ||
      (is.character(varp_layer_names) && all(nzchar(varp_layer_names))),
    "'mode' must be 'READ' or 'WRITE'" = is.null(mode) ||
      (is.character(mode) && length(mode == 1L) && mode %in% c('READ', 'WRITE')),
    "'seed' must be a single integer value" = is.null(seed) ||
      (is.integer(seed) && length(seed) == 1L)
  )

  experiment <- SOMAExperimentCreate(uri, platform_config = config)

  experiment$obs <- create_and_populate_obs(
    uri = file.path(uri, "obs"),
    nrows = n_obs,
    factors = factors
  )

  experiment$ms <- SOMACollectionCreate(file.path(uri, "ms"))

  ms_rna <- SOMAMeasurementCreate(file.path(uri, "ms", "RNA"))
  ms_rna$set_metadata(.assay_version_hint('v5'))

  ms_rna$var <- create_and_populate_var(
    uri = file.path(ms_rna$uri, "var"),
    nrows = n_var,
    factors = factors
  )
  ms_rna$X <- SOMACollectionCreate(file.path(ms_rna$uri, "X"))

  ragged_density <- seq(from = 0L, to = 1L, by = 0.1)
  ragged_density <- rev(ragged_density[ragged_density > 0L])
  ragged_density <- rep_len(ragged_density, length.out = length(X_layer_names))

  if (!is.na(seed)) {
    set.seed(seed)
  }

  for (i in seq_along(X_layer_names)) {
    layer_name <- X_layer_names[i]

    mat <- Matrix::rsparsematrix(
      nrow = ceiling(n_obs * ragged_density[i]),
      ncol = ceiling(n_var * ragged_density[i]),
      density = 0.6,
      rand.x = function(n) as.integer(runif(n, min = 1, max = 100)),
      repr = 'T'
    )

    ndarray <- SOMASparseNDArrayCreate(
      file.path(ms_rna$X$uri, layer_name),
      arrow::int32(),
      shape = dim(mat)
    )
    ndarray$write(mat)
    if (ragged_density[i] != 1L) {
      ndarray$set_metadata(.ragged_array_hint())
    }
    ndarray$set_metadata(.type_hint(class(mat)))

    ms_rna$X$set(ndarray, name = layer_name)
  }
  ms_rna$X$close()

  # Add obsm layers
  if (rlang::is_integerish(obsm_layers)) {
    obsm <- SOMACollectionCreate(file.path(ms_rna$uri, "obsm"))
    for (layer in names(obsm_layers)) {
      key <- gsub(pattern = '^dense:', replacement = '', x = layer)
      shape <- c(n_obs, obsm_layers[layer])
      if (grepl(pattern = '^dense:', x = layer)) {
        obsm$add_new_dense_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        obsm$get(key)$write(create_dense_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      } else {
        obsm$add_new_sparse_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        obsm$get(key)$write(create_sparse_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      }
    }
    obsm$close()
    ms_rna$add_new_collection(obsm, "obsm")
  }

  # Add varm layers
  if (rlang::is_integerish(varm_layers)) {
    varm <- SOMACollectionCreate(file.path(ms_rna$uri, "varm"))
    for (layer in names(varm_layers)) {
      key <- gsub(pattern = '^dense:', replacement = '', x = layer)
      shape <- c(n_var, varm_layers[layer])
      if (grepl(pattern = '^dense:', x = layer)) {
        varm$add_new_dense_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        varm$get(key)$write(create_dense_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      } else {
        varm$add_new_sparse_ndarray(
          key = key,
          type = arrow::int32(),
          shape = shape
        )
        varm$get(key)$write(create_sparse_matrix_with_int_dims(
          nrows = shape[1L],
          ncols = shape[2L]
        ))
      }
    }
    varm$close()
    ms_rna$add_new_collection(varm, "varm")
  }

  # Add obsp layers
  if (is.character(obsp_layer_names)) {
    obsp <- SOMACollectionCreate(file.path(ms_rna$uri, "obsp"))
    for (layer in obsp_layer_names) {
      obsp$add_new_sparse_ndarray(
        key = layer,
        type = arrow::int32(),
        shape = c(n_obs, n_obs)
      )
      obsp$get(layer)$write(create_sparse_matrix_with_int_dims(
        nrows = n_obs,
        ncols = n_obs
      ))
    }
    obsp$close()
    ms_rna$add_new_collection(obsp, "obsp")
  }

  # Add varp layers
  if (is.character(varp_layer_names)) {
    varp <- SOMACollectionCreate(file.path(ms_rna$uri, "varp"))
    for (layer in varp_layer_names) {
      varp$add_new_sparse_ndarray(
        key = layer,
        type = arrow::int32(),
        shape = c(n_var, n_var)
      )
      varp$get(layer)$write(create_sparse_matrix_with_int_dims(
        nrows = n_var,
        ncols = n_var
      ))
    }
    varp$close()
    ms_rna$add_new_collection(varp, "varp")
  }

  ms_rna$close()

  experiment$ms$set(ms_rna, name = "RNA")
  experiment$ms$close()

  if (is.null(mode)) {
    experiment$close()
  } else {
    experiment$reopen(mode)
  }
  return(experiment)
}

# Creates a SOMASparseNDArray with domains of `[0, 2^31 - 1]` and non-zero
# values at `(0,0)`, `(2^31 - 2, 2^31 - 2)` and `(2^31 - 1, 2^31 - 1)`. This is
# intended to test R's ability to read from arrays created with tiledbsoma-py
# before the default domain was changed to `[0, 2^31)`.
#
#  Row/Column:   0      ...    2147483646 |    2147483647
#  0             1      ...    0          |    0
#  ...           ...    ...    ...        |    ...
#  2147483646    0      ...    2          |    0
#  ---------------------------------------|---------------
#  2147483647    0      ...    0          |    3

create_and_populate_32bit_sparse_nd_array <- function(uri) {
  df <- data.frame(
    soma_dim_0 = bit64::as.integer64(c(0, 2^31 - 2, 2^31 - 1)),
    soma_dim_1 = bit64::as.integer64(c(0, 2^31 - 2, 2^31 - 1)),
    soma_data = c(1L, 2L, 3L)
  )

  arr <- SOMASparseNDArrayCreate(
    uri = uri,
    type = arrow::int32(),
    shape = rep_len(2 ^ 31, length.out = 2L)
  )
  on.exit(arr$close(), add = TRUE, after = FALSE)

  arr$.write_coordinates(df)

  return(uri)
}
