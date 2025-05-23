# Basic on-laptop setup

Build the docs with:

```bash
./build.sh
```

The first time you run this, it will:

1. Create and activate a virtualenv (`venv/`)
1. Install [`requirements_doc.txt`](requirements_doc.txt)
1. Install `..apis/python` (editable)
1. Build the docs (output to `doc/html/`)

Subsequent runs will only perform the 4th step (unless `-r`/`--reinstall` is passed).

Once the docs are built, you can:

```bash
open source/_build/html/index.html
```

or e.g.:

```bash
http-server source/_build/html &
open http://localhost:8080/
```

and inspect them.
