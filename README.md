TODO: write the full readme\
TODO: project started June 2023 versioning is done by release date - 2023.7.22.0

# FileKit

FileKit is a simple cloud storage solution designed for flat file storage.

Many cloud storage solutions provide live editing, detailed sharing, comments,
syncing, and other functionality. FileKit instead focuses on simplicity -
cutting out these features results in a faster, easier to deploy system. In
contrast to other cloud storage solutions, FileKit can be run as a single
standalone process, without requiring a great deal of host system access.

FileKit's features consist simply of the following:

- Upload and download of single files
- Multiple active users
- View-only sharing

## Usage

**For security reasons, it is HIGHLY RECOMMENDED that any publicly-facing
FileKit instance be run behind a reverse proxy to provide TLS support.**\
FileKit does not provide in-flight encryption as part of its services.

Run the `filekit` executable and pass options as necessary. -w and -d are
required, denoting the web directory and data directory (respectively).

- The web directory is the location where the frontend is stored. Request paths
  are relative to this directory. /d/ and /api/ are reserved for api endpoints.
- The data directory is the location where uploads and the database is stored.
  It should be empty at first startup.
- If your system has IPv6 dual binding enabled, you should also pass -6 to
  disable direct IPv4 binding.
- For information about other options, see `filekit -h`.

The server secret (for user authentication) is set via the FILEKIT_SECRET
environment variable. Set this variable before starting. It will be unset during
initialization. For example, `FILEKIT_SECRET=supa_secure ./filekit`

## Building from Source

To run `./install-dependencies.sh`, first install the following:

- `autoconf`
- `libtool`
- `pkg-config`
- `openssl` (e.g., `libssl-dev`)
- `tcl`

Next, run `./install-dependencies.sh` to get jansson, libjwt, and sqlite. You
may also install these libraries separately.

After this is finished, run the provided Makefile to build the FileKit backend.

TODO: instructions for setting up the default frontend.

## API

FileKit uses the following backend API system.

TODO: Note the API system that is used as the interface between the backend and
frontend. This allows a different frontend to be swapped in if the client so
chooses.

Keep in mind that the frontend must be static, potentially using an SPA through
fallback.
