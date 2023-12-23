# Kitserv - Extensible Single-Instance File Server

Kitserv is a simple, single-instance file server with extensible API.

The goal of Kitserv is to provide a simple, easily-deployable stateless file
server that can be linked into a program to quickly host a web server, without
sacrificing performance for ease of use. Kitserv can also be used to host any
directory as a web server, without needing to point a full-fledged server at it.

Kitserv does not provide TLS. Use a reverse proxy or tunnel for TLS support.

## Usage

### Simple Fully-Static File Server

Run `bin/kitserv`, passing arguments as necessary.

### API-Extended Server

Link `lib/libkitserv.a` into your executable. Use the provided interface
(`include/kitserv.h`) to connect your web application to Kitserv's API hooks.

### Documentation

Documentation is contained in the `man` directory.

For the fully-static server, see `kitserv(1)`.

For the libkitserv library, see `kitserv(3)` and associated functions in section
3 manual pages.

## Building from Source

Run `make`. This will build both the library and standalone executable.

Use `make install` or `./install.sh` to install the library. Use the environment
variables described in `install.sh` to customize the installation directory.

## License

Kitserv is licensed under the GNU Affero GPL v3. You are free to redistribute
and modify this code as you see fit, provided that you make the source code
freely available under these terms. For more information, see the LICENSE file.

Kitserv does not reflect its own source code. If you do make modifications, you
are responsible for making sure that the changes are accessible.
