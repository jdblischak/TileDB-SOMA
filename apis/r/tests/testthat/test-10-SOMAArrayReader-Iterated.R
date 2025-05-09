test_that("Iterated Interface from SOMAArrayReader", {
  skip_if(!extended_tests() || covr_tests())
  skip_if_not_installed("pbmc3k.tiledb") # a Suggests: pre-package 3k PBMC data
  # see https://ghrr.github.io/drat/
  library(arrow)
  library(bit64)

  tdir <- tempfile()
  tgzfile <- system.file("raw-data", "soco-pbmc3k.tar.gz", package = "pbmc3k.tiledb")
  untar(tarfile = tgzfile, exdir = tdir)

  uri <- file.path(tdir, "soco", "pbmc3k_processed", "ms", "RNA", "X", "data")
  expect_true(dir.exists(uri))

  somactx <- soma_context()
  sr <- mq_setup(uri, ctxxp = somactx, loglevel = "warn")
  expect_true(inherits(sr, "externalptr"))

  rl <- data.frame()
  while (!tiledbsoma:::mq_complete(sr)) {
    dat <- mq_next(sr)
    D <- soma_array_to_arrow_table(dat)
    expect_true(nrow(D) > 0)
    expect_true(is_arrow_table(D))
    rl <- rbind(rl, D$to_data_frame())
  }
  expect_true(is.data.frame(rl))
  expect_equal(nrow(rl), 4848644)
  expect_equal(ncol(rl), 3)
  rm(sr)
  gc()

  sr <- mq_setup(
    uri,
    ctxxp = somactx,
    dim_points = list(soma_dim_0 = bit64::as.integer64(1))
  )
  expect_true(inherits(sr, "externalptr"))

  rl <- data.frame()
  while (!tiledbsoma:::mq_complete(sr)) {
    dat <- mq_next(sr)
    D <- soma_array_to_arrow_table(dat)
    expect_true(nrow(D) > 0)
    expect_true(is_arrow_table(D))
    rl <- rbind(rl, as.data.frame(D))
  }
  expect_true(is.data.frame(rl))
  expect_equal(nrow(rl), 1838)
  expect_equal(ncol(rl), 3)

  rm(sr)
  gc()

  sr <- mq_setup(uri,
    ctxxp = somactx,
    dim_range = list(soma_dim_1 = cbind(
      bit64::as.integer64(1),
      bit64::as.integer64(2)
    ))
  )
  expect_true(inherits(sr, "externalptr"))

  rl <- data.frame()
  while (!tiledbsoma:::mq_complete(sr)) {
    dat <- mq_next(sr)
    D <- soma_array_to_arrow_table(dat)
    expect_true(nrow(D) > 0)
    expect_true(is_arrow_table(D))
    rl <- rbind(rl, as.data.frame(D))
  }
  expect_true(is.data.frame(rl))
  expect_equal(nrow(rl), 5276)
  expect_equal(ncol(rl), 3)

  ## test completeness predicate on shorter data
  uri <- extract_dataset("soma-dataframe-pbmc3k-processed-obs")
  sr <- mq_setup(uri, somactx)

  expect_false(tiledbsoma:::mq_complete(sr))
  dat <- mq_next(sr)
  expect_true(tiledbsoma:::mq_complete(sr))

  rm(sr)
  gc()
})


test_that("Iterated Interface from SOMA Classes", {
  skip_if(!extended_tests() || covr_tests())
  skip_if_not_installed("pbmc3k.tiledb") # a Suggests: pre-package 3k PBMC data

  tdir <- tempfile()
  tgzfile <- system.file("raw-data", "soco-pbmc3k.tar.gz", package = "pbmc3k.tiledb")
  untar(tarfile = tgzfile, exdir = tdir)
  uri <- file.path(tdir, "soco", "pbmc3k_processed", "ms", "raw", "X", "data")

  ## parameterize test
  test_cases <- c("data.frame", "sparse")

  # The read_complete et al. in this test case are designed to be verified
  # against 16MB buffer size, and the particular provided input dataset.
  # The soma_context() is cached at the package level and passed that way
  # to the SOMADataFrame and SOMASparseNDArray classes
  somactx <- soma_context(c(soma.init_buffer_bytes = as.character(16777216)))

  for (tc in test_cases) {
    sdf <- switch(tc,
      data.frame = SOMADataFrameOpen(uri),
      sparse = SOMASparseNDArrayOpen(uri)
    )
    expect_true(inherits(sdf, "SOMAArrayBase"))

    iterator <- switch(tc,
      data.frame = sdf$read(),
      sparse = sdf$read()$tables()
    )

    expect_true(inherits(iterator, "ReadIter"))
    expect_true(inherits(iterator, "TableReadIter"))

    # Test $concat()
    expect_false(iterator$read_complete())
    dat <- iterator$concat()
    expect_true(iterator$read_complete())
    expect_true(inherits(dat, "Table"))
    expect_equal(dat$num_columns, 3)
    expect_equal(dat$num_rows, 2238732)

    rm(iterator)
    gc()

    # Test $read_next()
    iterator <- switch(tc,
      data.frame = sdf$read(),
      sparse = sdf$read()$tables()
    )

    expect_false(iterator$read_complete())
    for (i in 1:2) {
      expect_false(iterator$read_complete())
      dat_slice <- iterator$read_next()
      expect_true(inherits(dat_slice, "Table"))
      expect_equal(dat_slice$num_columns, 3)

      if (i < 2) {
        expect_equal(dat_slice$num_rows, 2097152)
      } else {
        expect_equal(dat_slice$num_rows, 141580)
      }
    }

    expect_true(iterator$read_complete())
    expect_warning(iterator$read_next()) # returns NULL with warning
    expect_warning(iterator$read_next()) # returns NULL with warning

    sdf$close()

    rm(iterator, sdf)
    gc()
  }
})

test_that("Iterated Interface from SOMA Sparse Matrix", {
  skip_if(!extended_tests() || covr_tests())
  skip_if_not_installed("pbmc3k.tiledb") # a Suggests: pre-package 3k PBMC data
  # skip_if(Sys.getenv("CI", "") != "")         # breaks only in CI so skipping

  tdir <- tempfile()
  tgzfile <- system.file("raw-data", "soco-pbmc3k.tar.gz", package = "pbmc3k.tiledb")
  untar(tarfile = tgzfile, exdir = tdir)
  uri <- file.path(tdir, "soco", "pbmc3k_processed", "ms", "raw", "X", "data")

  # The read_complete et al. in this test case are designed to be verified
  # against 16MB buffer size, and the particular provided input dataset.
  # The soma_context() is cached at the package level and passed that way
  # to the SOMADataFrame and SOMASparseNDArray classes
  somactx <- soma_context(c(soma.init_buffer_bytes = as.character(16777216)))
  snda <- SOMASparseNDArrayOpen(uri)

  expect_true(inherits(snda, "SOMAArrayBase"))

  iterator <- snda$read()$sparse_matrix(zero_based = T)

  nnzTotal <- 0
  rowsTotal <- 0
  for (i in 1:2) {
    expect_false(iterator$read_complete())
    dat <- iterator$read_next()$get_one_based_matrix()
    ## -- nnz <- Matrix::nnzero(dat)
    ##    use length() which is identical for this data set but does not suffer from an issue sometimes seen in CI
    nnz <- length(dat@x)
    expect_gt(nnz, 0)
    nnzTotal <- nnzTotal + nnz
    # the shard dims always match the shape of the whole sparse matrix
    expect_equal(dim(dat), as.integer(snda$shape()))
  }

  expect_true(iterator$read_complete())
  expect_warning(iterator$read_next()) # returns NULL with warning
  expect_warning(iterator$read_next()) # returns NULL with warning
  ## -- expect_equal(nnzTotal, Matrix::nnzero(snda$read()$sparse_matrix(T)$concat()$get_one_based_matrix()))
  ##    use length() which is identical for this data set but does not suffer from an issue sometimes seen in CI
  expect_equal(nnzTotal, length(snda$read()$sparse_matrix(T)$concat()$get_one_based_matrix()@x))
  expect_equal(nnzTotal, 2238732)

  rm(snda)
  gc()
})

test_that("Dimension Point and Ranges Bounds", {
  skip_if(!extended_tests() || covr_tests())
  ctx <- tiledbsoma::SOMATileDBContext$new()
  human_experiment <- load_dataset("soma-exp-pbmc-small", tiledbsoma_ctx = ctx)
  X <- human_experiment$ms$get("RNA")$X$get("data")
  expect_equal(X$shape(), c(80, 230))

  somactx <- soma_context()

  ## 'good case' with suitable dim points
  coords <- list(
    soma_dim_0 = bit64::as.integer64(0:5),
    soma_dim_1 = bit64::as.integer64(0:5)
  )
  sr <- mq_setup(uri = X$uri, ctxxp = somactx, dim_points = coords)

  chunk <- mq_next(sr)
  at <- arrow::as_arrow_table(chunk)
  expect_equal(at$num_rows, 5)
  expect_equal(at$num_columns, 3)
  rm(sr)
  gc()

  ## 'good case' with suitable dim ranges
  ranges <- list(
    soma_dim_0 = matrix(bit64::as.integer64(c(1, 4)), 1),
    soma_dim_1 = matrix(bit64::as.integer64(c(1, 4)), 1)
  )
  sr <- mq_setup(uri = X$uri, somactx, dim_ranges = ranges)

  chunk <- mq_next(sr)
  at <- arrow::as_arrow_table(chunk)
  expect_equal(at$num_rows, 2)
  expect_equal(at$num_columns, 3)

  ## 'bad case' with unsuitable dim points
  coords <- list(
    soma_dim_0 = bit64::as.integer64(81:86),
    soma_dim_1 = bit64::as.integer64(0:5)
  )
  expect_error(mq_setup(uri = X$uri, dim_points = coords))

  ## 'bad case' with unsuitable dim range
  ranges <- list(
    soma_dim_0 = matrix(bit64::as.integer64(c(91, 94)), 1),
    soma_dim_1 = matrix(bit64::as.integer64(c(1, 4)), 1)
  )
  expect_error(mq_setup(uri = X$uri, dim_ranges = ranges))
  rm(sr)
  gc()
})
