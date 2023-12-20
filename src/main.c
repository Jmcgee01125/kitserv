/* Kitserv
 * Copyright (C) 2023 Jmcgee01125
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "kitserv.h"

#define DEFAULT_PORT_STRING ("8012")
#define DEFAULT_FALLBACK_PATH ("200.html")
#define DEFAULT_FALLBACK_ROOT_PATH ("index.html")
#define DEFAULT_NUM_WORKERS (2)
#define DEFAULT_NUM_SLOTS (128)

static void usage(const char* prog_name)
{
    fprintf(stderr,
            "Usage: %s -w webdir [-p port] [-s slots] [-t threads] [-f fallback] [-r root_fb] [-4] [-6] [-h]\n"
            "\t-w webdir     Root directory from which to serve files.\n"
            "\t-p port       Port to run on (default: %s).\n"
            "\t-s slots      Number of connection slots to allocate (default: %d).\n"
            "\t-t threads    Number of worker threads to use for serving clients (default: %d).\n"
            "\t-f fallback   Path to fallback resource (default: %s).\n"
            "\t-r root_fb    Path to fallback resource when the path is / (default: %s).\n"
            "\t-4            Bind IPv4 only.\n"
            "\t-6            Bind IPv6 only, or both when dual binding is enabled (falls back to IPv4 if no IPv6).\n"
            "\t-h            Show this help.\n",
            prog_name, DEFAULT_PORT_STRING, DEFAULT_NUM_SLOTS, DEFAULT_NUM_WORKERS, DEFAULT_FALLBACK_PATH,
            DEFAULT_FALLBACK_ROOT_PATH);
    exit(1);
}

int main(int argc, char* argv[])
{
    struct kitserv_request_context root_context;
    struct kitserv_config config;
    int opt;

    root_context = (struct kitserv_request_context){
        .root = NULL,
        .root_fallback = DEFAULT_FALLBACK_ROOT_PATH,
        .fallback = DEFAULT_FALLBACK_PATH,
        .use_http_append_fallback = true,
    };

    config = (struct kitserv_config){
        .port_string = DEFAULT_PORT_STRING,
        .num_workers = DEFAULT_NUM_WORKERS,
        .num_slots = DEFAULT_NUM_SLOTS,
        .bind_ipv4 = true,
        .bind_ipv6 = true,
        .silent_mode = false,
        .http_root_context = &root_context,
        .api_tree = NULL,
    };

    while ((opt = getopt(argc, argv, "w:p:s:t:f:r:46h")) != -1) {
        switch (opt) {
            case 'w':
                root_context.root = optarg;
                break;
            case 'p':
                config.port_string = optarg;
                break;
            case 's':
                config.num_slots = atoi(optarg);
                if (config.num_slots < 1) {
                    fprintf(stderr, "Invalid slot count (%d).\n", config.num_slots);
                    exit(1);
                }
                break;
            case 't':
                config.num_workers = atoi(optarg);
                if (config.num_workers < 1) {
                    fprintf(stderr, "Invalid worker count (%d).\n", config.num_workers);
                    exit(1);
                }
                break;
            case 'f':
                root_context.fallback = optarg;
                break;
            case 'r':
                root_context.root_fallback = optarg;
                break;
            case '4':
                config.bind_ipv4 = true;
                config.bind_ipv6 = false;
                break;
            case '6':
                config.bind_ipv4 = false;
                config.bind_ipv6 = true;
                break;
            case 'h':
            default:
                usage(argv[0]);
        }
    }

    if (!root_context.root) {
        usage(argv[0]);
    }

    printf("Starting on port %s.\n", config.port_string);
    printf("Web root:  %s\n", root_context.root);

    kitserv_server_start(&config);

    printf("Kitserv shutting down.\n");

    return 0;
}
